/*
 * USB serial log for the zeus probe: the phone becomes a CDC-ACM device
 * (Linux: /dev/ttyACM*, no driver needed) and streams the probe log.
 *
 * Port of the ios-sweet XNU USB gadget (patches 0073-0075: ep0 state
 * machine, VBUS override, endpoint setup) to bare metal. ABL leaves the
 * DWC3 powered and in device mode after fastboot; we restart only the
 * device controller. MMU off: memory is uncached, so DMA needs no cache
 * maintenance and physical = our addresses. Events are polled.
 * Register layout per the Synopsys DWC3 databook as used by Linux
 * drivers/usb/dwc3 (core.h, gadget.c, ep0.c).
 */
#include "probe.h"

#define GDSC_USB30_PRIM	0x00149004UL	/* zeus DT: qcom,gdsc@149004 */
#define DWC		0x0a600000UL
#define QSCRATCH	0x0a6f8800UL
#define QS_HS_PHY_CTRL	0x10
#define   UTMI_OTG_VBUS_VALID	(1u << 20)
#define   SW_SESSVLD_SEL	(1u << 28)
#define QS_SS_PHY_CTRL	0x30
#define   LANE0_PWR_PRESENT	(1u << 24)

#define GCTL		0xc110
#define GSNPSID		0xc120
#define GEVNTADRLO	0xc400
#define GEVNTADRHI	0xc404
#define GEVNTSIZ	0xc408
#define GEVNTCOUNT	0xc40c
#define DCFG		0xc700
#define DCTL		0xc704
#define DEVTEN		0xc708
#define DSTS		0xc70c
#define DALEPENA	0xc720
#define DEPCMDPAR2(n)	(0xc800 + 0x10 * (n))
#define DEPCMDPAR1(n)	(0xc804 + 0x10 * (n))
#define DEPCMDPAR0(n)	(0xc808 + 0x10 * (n))
#define DEPCMD(n)	(0xc80c + 0x10 * (n))

#define DCTL_RUN_STOP	(1u << 31)
#define DCTL_CSFTRST	(1u << 30)
#define DSTS_DEVCTRLHLT	(1u << 22)

#define DEPCMD_SETEPCONFIG	1
#define DEPCMD_SETTRANSFRESOURCE 2
#define DEPCMD_SETSTALL		4
#define DEPCMD_STARTTRANSFER	6
#define DEPCMD_DEPSTARTCFG	9
#define DEPCMD_CMDACT		(1u << 10)

#define TRB_HWO		(1u << 0)
#define TRB_LST		(1u << 1)
#define TRB_ISP_IMI	(1u << 10)
#define TRB_IOC		(1u << 11)
#define TRBCTL_NORMAL	(1u << 4)
#define TRBCTL_SETUP	(2u << 4)
#define TRBCTL_STATUS2	(3u << 4)
#define TRBCTL_STATUS3	(4u << 4)
#define TRBCTL_DATA	(5u << 4)

/* physical endpoints: number * 2 + direction */
#define EP_INT		3	/* 0x81 interrupt IN (ACM notifications, never used) */
#define EP_RX		4	/* 0x02 bulk OUT (not armed: host writes are NAKed) */
#define EP_TX		5	/* 0x82 bulk IN: the log */

#define EVT_SIZE	4096
static uint32_t evt[EVT_SIZE / 4] __attribute__((aligned(4096)));
static uint32_t trb[4][4] __attribute__((aligned(16)));	/* ep0 OUT, ep0 IN, TX */
static uint8_t setup_pkt[8] __attribute__((aligned(16)));
static uint8_t ep0buf[512] __attribute__((aligned(16)));

static enum { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS2, EP0_WAIT_STATUS3, EP0_STATUS } ep0;
static uint32_t evt_pos, events;
static int up, configured, tx_busy;
static uint32_t tx_pos, tx_len;
static int restart;	/* host opened the port (DTR on): send the log again from the start */

static uint32_t dwc_rd(uint32_t off) { return rd32(DWC + off); }
static void dwc_wr(uint32_t off, uint32_t v) { wr32(DWC + off, v); }

static int epcmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	dwc_wr(DEPCMDPAR0(ep), p0);
	dwc_wr(DEPCMDPAR1(ep), p1);
	dwc_wr(DEPCMDPAR2(ep), p2);
	dwc_wr(DEPCMD(ep), cmd | DEPCMD_CMDACT);
	uint64_t end = now() + freq() / 100;
	while (now() < end) {
		uint32_t v = dwc_rd(DEPCMD(ep));
		if (!(v & DEPCMD_CMDACT)) {
			if ((v >> 12) & 0xf) {
				puts_c("USB: ep", C_RED); putdec(ep, C_RED);
				puts_c(" cmd ", C_RED); puthex(cmd & 0xf, 1, C_RED);
				puts_c(" status ", C_RED); puthex((v >> 12) & 0xf, 1, C_RED); puts_c("\n", 0);
				return -1;
			}
			return 0;
		}
	}
	puts_c("USB: ep", C_RED); putdec(ep, C_RED); puts_c(" cmd timeout\n", C_RED);
	return -1;
}

static void start_trb(unsigned ep, uint32_t *t, const void *buf, uint32_t len, uint32_t ctl)
{
	uint64_t b = (uint64_t)(uintptr_t)buf, tp = (uint64_t)(uintptr_t)t;
	t[0] = (uint32_t)b;
	t[1] = (uint32_t)(b >> 32);
	t[2] = len;
	t[3] = ctl | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	__asm__ volatile("dsb sy" ::: "memory");
	epcmd(ep, DEPCMD_STARTTRANSFER, (uint32_t)(tp >> 32), (uint32_t)tp, 0);
}

static void start_setup(void)
{
	ep0 = EP0_SETUP;
	start_trb(0, trb[0], setup_pkt, 8, TRBCTL_SETUP);
}

/* VID 0x1209 / PID 0x0001: pid.codes test ID, same as ios-sweet. */
static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
	0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t cfg_desc[67] = {
	9, 2, 67, 0, 2, 1, 0, 0x80, 250,		/* 2 interfaces, 500 mA */
	9, 4, 0, 0, 1, 0x02, 0x02, 0x00, 0,		/* comm interface 0: CDC ACM */
	5, 0x24, 0x00, 0x10, 0x01,			/* header, CDC 1.10 */
	5, 0x24, 0x01, 0x00, 1,				/* call management: data interface 1 */
	4, 0x24, 0x02, 0x02,				/* ACM: line coding + state */
	5, 0x24, 0x06, 0, 1,				/* union: control 0, data 1 */
	7, 5, 0x81, 0x03, 16, 0, 9,			/* ep 0x81 interrupt IN */
	9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,		/* data interface 1 */
	7, 5, 0x82, 0x02, 0x00, 0x02, 0,		/* ep 0x82 bulk IN, 512 */
	7, 5, 0x02, 0x02, 0x00, 0x02, 0,		/* ep 0x02 bulk OUT, 512 */
};
static uint8_t line_coding[7] = { 0x00, 0xc2, 0x01, 0x00, 0, 0, 8 };	/* 115200 8N1 */

static uint32_t string_desc(unsigned idx, uint8_t *out)
{
	const char *s;
	if (idx == 0) {
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;
		return 4;
	}
	if (idx == 1) s = "ios-zeus";
	else if (idx == 2) s = "Zeus probe log";
	else if (idx == 3) s = "0002";
	else return 0;
	uint32_t n = 2;
	for (; *s; s++) { out[n++] = (uint8_t)*s; out[n++] = 0; }
	out[0] = (uint8_t)n; out[1] = 3;
	return n;
}

static void copy(uint8_t *d, const uint8_t *s, uint32_t n) { while (n--) *d++ = *s++; }

static void configure(void)
{
	if (configured)
		return;
	if (epcmd(0, DEPCMD_DEPSTARTCFG | (2u << 16), 0, 0, 0) ||
	    epcmd(EP_RX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3), (1u << 8) | ((uint32_t)EP_RX << 25), 0) ||
	    epcmd(EP_RX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    epcmd(EP_TX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3) | (1u << 17), (1u << 8) | ((uint32_t)EP_TX << 25), 0) ||
	    epcmd(EP_TX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    epcmd(EP_INT, DEPCMD_SETEPCONFIG, (3u << 1) | (16u << 3) | (2u << 17),
		  (1u << 8) | (8u << 16) | ((uint32_t)EP_INT << 25), 0) ||
	    epcmd(EP_INT, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		puts_c("USB: data endpoints failed\n", C_RED);
		return;
	}
	dwc_wr(DALEPENA, dwc_rd(DALEPENA) | (1u << EP_RX) | (1u << EP_TX) | (1u << EP_INT));
	configured = 1;
	puts_c("USB: configured, log streaming\n", C_GREEN);
}

static void stall(void)
{
	epcmd(0, DEPCMD_SETSTALL, 0, 0, 0);
	start_setup();
}

static void handle_setup(void)
{
	uint8_t type = setup_pkt[0], req = setup_pkt[1];
	uint16_t value = setup_pkt[2] | setup_pkt[3] << 8;
	uint16_t length = setup_pkt[6] | setup_pkt[7] << 8;
	uint32_t n = 0;
	int ok = 1, in = (type & 0x80) != 0;

	if ((type & 0x60) == 0x20) {			/* CDC class requests */
		if (req == 0x21 && in) {		/* GET_LINE_CODING */
			copy(ep0buf, line_coding, 7); n = 7;
		} else if (req == 0x22) {		/* SET_CONTROL_LINE_STATE */
			if (value & 1)
				restart = 1;
		} else if (req == 0x20 || req == 0x23) {
			/* SET_LINE_CODING (data ignored), SEND_BREAK */
		} else {
			ok = 0;
		}
	} else if (type & 0x60) {
		ok = 0;
	} else if (req == 6) {				/* GET_DESCRIPTOR */
		switch (value >> 8) {
		case 1: copy(ep0buf, dev_desc, sizeof(dev_desc)); n = sizeof(dev_desc); break;
		case 2: copy(ep0buf, cfg_desc, sizeof(cfg_desc)); n = sizeof(cfg_desc); break;
		case 3: n = string_desc(value & 0xff, ep0buf); ok = n != 0; break;
		default: ok = 0; break;
		}
	} else if (req == 5) {				/* SET_ADDRESS */
		dwc_wr(DCFG, (dwc_rd(DCFG) & ~(0x7fu << 3)) | ((uint32_t)(value & 0x7f) << 3));
	} else if (req == 9) {				/* SET_CONFIGURATION */
		if (value == 1)
			configure();
	} else if (req == 8) {				/* GET_CONFIGURATION */
		ep0buf[0] = configured; n = 1;
	} else if (req == 0) {				/* GET_STATUS */
		ep0buf[0] = 0; ep0buf[1] = 0; n = 2;
	} else if (req == 10) {				/* GET_INTERFACE */
		ep0buf[0] = 0; n = 1;
	} else if (req == 11 || req == 1 || req == 3) {	/* SET_INTERFACE, CLEAR/SET_FEATURE */
	} else {
		ok = 0;
	}
	if (!ok) {
		stall();
		return;
	}
	if (length == 0) {
		ep0 = EP0_WAIT_STATUS2;
		return;
	}
	ep0 = EP0_DATA;
	if (in)
		start_trb(1, trb[1], ep0buf, n > length ? length : n, TRBCTL_DATA);
	else
		start_trb(0, trb[0], ep0buf, 64, TRBCTL_DATA);
}

static void tx_kick(void)
{
	if (!configured || tx_busy)
		return;
	if (restart) {
		restart = 0;
		tx_pos = 0;
	}
	uint32_t end = loglen;
	if (tx_pos >= end)
		return;
	tx_len = end - tx_pos;
	if (tx_len > 16384)
		tx_len = 16384;
	tx_busy = 1;
	start_trb(EP_TX, trb[2], logbuf + tx_pos, tx_len, TRBCTL_NORMAL);
}

static void event(uint32_t ev)
{
	if (ev & 1) {					/* device event */
		uint32_t type = (ev >> 8) & 0xf;
		if (type == 1) {			/* USB reset */
			dwc_wr(DCFG, dwc_rd(DCFG) & ~(0x7fu << 3));
		} else if (type == 2) {			/* connect done: ep0 max packet 64 */
			epcmd(0, DEPCMD_SETEPCONFIG, (64u << 3) | (2u << 30), (1u << 8) | (1u << 10), 0);
			epcmd(1, DEPCMD_SETEPCONFIG, (64u << 3) | (2u << 30), (1u << 8) | (1u << 10) | (1u << 25), 0);
			puts_c("USB: connected, speed ", C_GREEN);
			putdec(dwc_rd(DSTS) & 7, C_GREEN);
			puts_c("\n", 0);
		} else if (type == 0) {
			configured = 0;
			tx_busy = 0;
		}
		return;
	}
	unsigned ep = (ev >> 1) & 0x1f, type = (ev >> 6) & 0xf, status = (ev >> 12) & 0xf;
	if (ep == EP_TX && type == 1) {			/* log chunk sent */
		tx_pos += tx_len;
		tx_busy = 0;
		return;
	}
	if (ep > 1)
		return;
	/* control transfers, as Linux dwc3 ep0.c */
	if (type == 1) {				/* XferComplete */
		if (ep0 == EP0_SETUP)
			handle_setup();
		else if (ep0 == EP0_DATA)
			ep0 = EP0_WAIT_STATUS3;
		else if (ep0 == EP0_STATUS)
			start_setup();
	} else if (type == 3 && status == 2) {		/* XferNotReady, status phase */
		if (ep0 == EP0_WAIT_STATUS2 || ep0 == EP0_WAIT_STATUS3) {
			uint32_t ctl = ep0 == EP0_WAIT_STATUS3 ? TRBCTL_STATUS3 : TRBCTL_STATUS2;
			ep0 = EP0_STATUS;
			start_trb(ep, trb[ep], ep0buf, 0, ctl);
		}
	}
}

void usb_poll(void)
{
	if (!up)
		return;
	uint32_t count = dwc_rd(GEVNTCOUNT) & 0xffff;
	for (uint32_t done = 0; done < count; done += 4) {
		uint32_t ev = evt[evt_pos / 4];
		evt_pos = (evt_pos + 4) % EVT_SIZE;
		events++;
		event(ev);
	}
	if (count)
		dwc_wr(GEVNTCOUNT, count);
	tx_kick();
}

void usb_start(void)
{
	uint32_t gdsc = rd32(GDSC_USB30_PRIM);
	line("USB GDSC ", gdsc, 8);
	if (!(gdsc & (1u << 31))) {
		puts_c("USB: power domain off, no USB log\n", C_RED);
		return;
	}
	puts_c("USB: reading DWC3...\n", C_GRAY);
	line("GSNPSID  ", dwc_rd(GSNPSID), 8);
	line("GCTL     ", dwc_rd(GCTL), 8);
	line("DCTL     ", dwc_rd(DCTL), 8);
	line("DSTS     ", dwc_rd(DSTS), 8);
	line("QS HSPHY ", rd32(QSCRATCH + QS_HS_PHY_CTRL), 8);

	/* VBUS session override (Linux dwc3_qcom_vbus_override_enable) */
	wr32(QSCRATCH + QS_HS_PHY_CTRL, rd32(QSCRATCH + QS_HS_PHY_CTRL) | UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL);
	wr32(QSCRATCH + QS_SS_PHY_CTRL, rd32(QSCRATCH + QS_SS_PHY_CTRL) | LANE0_PWR_PRESENT);

	/* stop whatever ABL left running, then device soft reset */
	dwc_wr(DCTL, dwc_rd(DCTL) & ~DCTL_RUN_STOP);
	delay_ms(10);
	dwc_wr(DCTL, DCTL_CSFTRST);
	uint64_t end = now() + freq() / 2;
	while ((dwc_rd(DCTL) & DCTL_CSFTRST) && now() < end)
		;
	if (dwc_rd(DCTL) & DCTL_CSFTRST) {
		puts_c("USB: soft reset stuck\n", C_RED);
		return;
	}
	uint64_t e = (uint64_t)(uintptr_t)evt;
	dwc_wr(GEVNTADRLO, (uint32_t)e);
	dwc_wr(GEVNTADRHI, (uint32_t)(e >> 32));
	dwc_wr(GEVNTSIZ, (1u << 31) | EVT_SIZE);	/* interrupt masked: polled */
	dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);
	evt_pos = 0;
	dwc_wr(DCFG, dwc_rd(DCFG) & ~(7u | (0x7fu << 3)));	/* high speed, address 0 */
	dwc_wr(DEVTEN, 0x7);				/* disconnect, reset, connect done */

	if (epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0) ||
	    epcmd(0, DEPCMD_SETEPCONFIG, 512u << 3, (1u << 8) | (1u << 10), 0) ||
	    epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    epcmd(1, DEPCMD_SETEPCONFIG, 512u << 3, (1u << 8) | (1u << 10) | (1u << 25), 0) ||
	    epcmd(1, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		puts_c("USB: ep0 setup failed\n", C_RED);
		return;
	}
	dwc_wr(DALEPENA, 0x3);
	start_setup();
	dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_RUN_STOP);
	end = now() + freq() / 2;
	while ((dwc_rd(DSTS) & DSTS_DEVCTRLHLT) && now() < end)
		;
	line("USB run, DSTS ", dwc_rd(DSTS), 8);
	up = 1;
}

/* Before jumping to xnu: stop the controller (soft disconnect) so it does
 * no more DMA into our buffers and the PC sees the device go away. */
void usb_stop(void)
{
	if (!up)
		return;
	dwc_wr(DCTL, dwc_rd(DCTL) & ~DCTL_RUN_STOP);
	uint64_t end = now() + freq() / 10;
	while (!(dwc_rd(DSTS) & DSTS_DEVCTRLHLT) && now() < end)
		;
	up = 0;
}

uint32_t usb_events(void) { return events; }
int usb_configured(void) { return configured; }
