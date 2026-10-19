/*
 * zeus probe 2: everything probe 1 showed, a dump of all display pipes,
 * and the log streamed over USB as a serial port (usb.c).
 *
 * Safety: no storage code. Memory writes: own image/stack/buffers and the
 * ABL splash framebuffer. Register writes (all via wr32): CTL_0 START of
 * the display controller, and the USB controller + its Qualcomm wrapper
 * (usb.c). Ends with PSCI SYSTEM_RESET after 120 s; Android boots again.
 *
 * MMU off: all data accesses are Device memory (-mstrict-align).
 */
#include "probe.h"
#include "font.h"

uint64_t psci_call(uint64_t fn, uint64_t a, uint64_t b, uint64_t c);
uint32_t usb_events(void);
int usb_configured(void);

#define PSCI_SYSTEM_RESET	0x84000009UL
#define RUN_SECONDS		120

/* Display controller (stock DT: mdp_phys 0xae00000, offsets from it) */
#ifndef MDP_BASE
#define MDP_BASE	0x0ae00000UL	/* tests override it */
#endif
#define CTL0		(MDP_BASE + 0x16000)	/* qcom,sde-ctl-off[0], "primary" */
#define CTL_START	0x1c
#define INTF1		(MDP_BASE + 0x36000)	/* first DSI interface */
#define SSPP_SRC_SIZE	0x00
#define SSPP_OUT_SIZE	0x0c
#define SSPP_OUT_XY	0x10
#define SSPP_SRC0_ADDR	0x14
#define SSPP_YSTRIDE0	0x24
#define SSPP_FORMAT	0x30

/* splash_region in the stock DT; replaced by the live DT value */
#define SPLASH_BASE	0xb8000000UL
#define SPLASH_SIZE	0x2b00000UL

/* Probe 1 photo: 1440x3200, stride 5760; ABL scans out two 720-wide halves. */
static volatile uint8_t *fb = (volatile uint8_t *)SPLASH_BASE;
static uint64_t fb_size = SPLASH_SIZE;
static const uint32_t fb_w = 1440, fb_h = 3200, fb_stride = 1440 * 4;

uint64_t now(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v)); return v; }
uint64_t freq(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v ? v : 19200000; }
void delay_ms(uint64_t ms) { uint64_t end = now() + freq() / 1000 * ms; while (now() < end) ; }

static void kick(void) { wr32(CTL0 + CTL_START, 1); }

/* ---------- log buffer ---------- */

#define LOG_SIZE	(256 * 1024)
char logbuf[LOG_SIZE];
volatile uint32_t loglen;
static int log_on = 1;

static void log_c(char c)
{
	if (log_on && loglen < LOG_SIZE)
		logbuf[loglen++] = c;
}

/* ---------- framebuffer text ---------- */

#define SCALE	3
#define CW	(8 * SCALE)
#define CH	(16 * SCALE)
#define MARGIN	24
#define TOP_ROW	9		/* probe 1: top ~420 lines are covered by noise */
#define BAND_H	300
#define TICK_ROW ((int)((fb_h - BAND_H - MARGIN) / CH) - 1)
#define LAST_ROW (TICK_ROW - 2)
#define C_BG	0x00000000U

static int row = TOP_ROW, col;

static void px(uint32_t x, uint32_t y, uint32_t c)
{
	uint64_t off = (uint64_t)y * fb_stride + (uint64_t)x * 4;
	if (x >= fb_w || y >= fb_h || off + 4 > fb_size)
		return;
	*(volatile uint32_t *)(fb + off) = 0xff000000U | c;
}

static void rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t c)
{
	for (uint32_t y = y0; y < y0 + h; y++)
		for (uint32_t x = x0; x < x0 + w; x++)
			px(x, y, c);
}

static void putc_at(int cl, int r, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	uint32_t x0 = MARGIN + cl * CW, y0 = MARGIN + r * CH;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++)
			rect(x0 + gx * SCALE, y0 + gy * SCALE, SCALE, SCALE,
			     (g[gy] & (0x80 >> gx)) ? fg : C_BG);
}

void puts_c(const char *s, uint32_t fg)
{
	for (; *s; s++) {
		log_c(*s);
		if (*s == '\n') {
			col = 0;
			if (++row > LAST_ROW && log_on) {	/* wrap: clear the text area */
				row = TOP_ROW;
				rect(0, MARGIN + TOP_ROW * CH, fb_w, (LAST_ROW - TOP_ROW + 1) * CH, C_BG);
			}
			continue;
		}
		if ((uint32_t)(MARGIN + (col + 1) * CW) <= fb_w)
			putc_at(col, row, *s, fg);
		col++;
	}
}

void puthex(uint64_t v, int digits, uint32_t fg)
{
	char b[17];
	for (int i = 0; i < digits; i++)
		b[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 0xf];
	b[digits] = 0;
	puts_c(b, fg);
}

void putdec(uint64_t v, uint32_t fg)
{
	char b[21];
	int i = 20;
	b[i] = 0;
	do { b[--i] = '0' + v % 10; v /= 10; } while (v);
	puts_c(b + i, fg);
}

void line(const char *label, uint64_t v, int digits)
{
	puts_c(label, C_GRAY);
	puthex(v, digits, C_WHITE);
	puts_c("\n", 0);
}

static void bands(void)
{
	rect(0, fb_h - BAND_H, fb_w / 3, BAND_H, C_RED);
	rect(fb_w / 3, fb_h - BAND_H, fb_w / 3, BAND_H, C_GREEN);
	rect(2 * fb_w / 3, fb_h - BAND_H, fb_w - 2 * (fb_w / 3), BAND_H, C_BLUE);
}

/* ---------- minimal FDT reader ---------- */

static uint32_t be32(const void *p)
{
	const uint8_t *b = p;
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static uint64_t be64(const void *p) { return (uint64_t)be32(p) << 32 | be32((const uint8_t *)p + 4); }

static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

/* node name matches a path component: exact, or "name@unit" vs "name" */
static int name_match(const char *node, const char *comp, int clen)
{
	for (int i = 0; i < clen; i++)
		if (node[i] != comp[i])
			return 0;
	return node[clen] == 0 || node[clen] == '@';
}

/* Property of the first node matching an absolute path; NULL if missing. */
static const uint8_t *fdt_prop(const uint8_t *dtb, const char *path, const char *prop, uint32_t *len)
{
	if (be32(dtb) != 0xd00dfeed)
		return 0;
	const uint8_t *p = dtb + be32(dtb + 8);
	const char *strs = (const char *)dtb + be32(dtb + 12);
	const char *comp = path + 1;	/* next path component to match */
	int depth = 0, matched = 0;	/* matched = path components found so far */
	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == 1) {		/* BEGIN_NODE */
			const char *name = (const char *)p;
			int nl = 0;
			while (name[nl]) nl++;
			p += (nl + 4) & ~3;
			depth++;
			int clen = 0;
			while (comp[clen] && comp[clen] != '/') clen++;
			if (depth == matched + 2 && clen && name_match(name, comp, clen)) {
				matched++;
				comp += clen + (comp[clen] == '/');
			}
			if (depth == 1)	/* root */
				matched = 0;
		} else if (tok == 2) {	/* END_NODE */
			if (depth == matched + 1 && matched > 0)
				return 0;	/* left a matched node: not found */
			depth--;
		} else if (tok == 3) {	/* PROP */
			uint32_t l = be32(p), nameoff = be32(p + 4);
			const uint8_t *val = p + 8;
			p += 8 + ((l + 3) & ~3);
			if (*comp == 0 && depth == matched + 1 && streq(strs + nameoff, prop)) {
				if (len) *len = l;
				return val;
			}
		} else if (tok == 4) {	/* NOP */
		} else {
			return 0;	/* END or garbage */
		}
	}
}

/* ---------- probe ---------- */

void probe_exception(uint64_t slot)
{
	static int nested;
	uint64_t esr, elr, far;
	__asm__ volatile("mrs %0, esr_el1; mrs %1, elr_el1; mrs %2, far_el1" : "=r"(esr), "=r"(elr), "=r"(far));
	if (nested > 1)		/* faulting inside the handler: just wait for the reset below */
		for (;;) ;
	rect(0, MARGIN + TOP_ROW * CH, fb_w, 8 * CH, C_RED);
	row = TOP_ROW; col = 0;
	log_on = 1;
	puts_c("EXCEPTION - photo this screen\n", C_WHITE);
	line("slot ", slot, 2);
	line("ESR  ", esr, 16);
	line("ELR  ", elr, 16);
	line("FAR  ", far, 16);
	puts_c("reset in 30 s\n", C_WHITE);
	if (!nested++)	/* a fault inside kick() comes back here with nested set */
		kick();
	delay_ms(30000);
	psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;) ;
}

static void dump_regs(const char *label, uint64_t base, uint32_t from, int n)
{
	puts_c(label, C_GRAY);
	line("", base + from, 8);
	for (int i = 0; i < n; i++) {
		puthex(rd32(base + from + 4 * i), 8, C_WHITE);
		puts_c((i % 4 == 3 || i == n - 1) ? "\n" : " ", 0);
	}
}

void probe_main(const uint8_t *dtb, uint64_t load)
{
	uint32_t l;
	const uint8_t *v = fdt_prop(dtb, "/reserved-memory/splash_region", "reg", &l);
	if (v && l >= 16) {
		fb = (volatile uint8_t *)be64(v);
		fb_size = be64(v + 8);
	}
	rect(0, 0, fb_w, fb_h, C_BG);
	bands();
	puts_c("ios-zeus probe 2\n", C_YEL);
	kick();

	uint64_t el, mpidr, midr;
	__asm__ volatile("mrs %0, CurrentEL; mrs %1, mpidr_el1; mrs %2, midr_el1" : "=r"(el), "=r"(mpidr), "=r"(midr));
	line("EL     ", el >> 2, 1);
	line("MPIDR  ", mpidr, 8);
	line("MIDR   ", midr, 8);
	line("load   ", load, 10);
	line("dtb    ", (uint64_t)dtb, 10);
	line("fb     ", (uint64_t)fb, 10);
	puts_c("memory (base size)\n", C_GRAY);
	v = fdt_prop(dtb, "/memory", "reg", &l);
	for (uint32_t i = 0; v && i + 16 <= l && i < 16 * 8; i += 16) {
		puthex(be64(v + i), 10, C_WHITE);
		puts_c(" ", 0);
		puthex(be64(v + i + 8), 10, C_WHITE);
		puts_c("\n", 0);
	}

	/* every pipe: who scans out what (explains the noise band at the top) */
	static const uint32_t sspp_off[8] = { 0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000 };
	puts_c("pipe addr srcsize outsize outxy strd fmt\n", C_GRAY);
	for (int i = 0; i < 8; i++) {
		uint64_t b = MDP_BASE + sspp_off[i];
		putdec(i, C_GRAY); puts_c(" ", 0);
		puthex(rd32(b + SSPP_SRC0_ADDR), 8, C_WHITE); puts_c(" ", 0);
		puthex(rd32(b + SSPP_SRC_SIZE), 8, C_WHITE); puts_c(" ", 0);
		puthex(rd32(b + SSPP_OUT_SIZE), 8, C_WHITE); puts_c(" ", 0);
		puthex(rd32(b + SSPP_OUT_XY), 8, C_WHITE); puts_c(" ", 0);
		puthex(rd32(b + SSPP_YSTRIDE0) & 0xffff, 4, C_WHITE); puts_c(" ", 0);
		puthex(rd32(b + SSPP_FORMAT), 8, C_WHITE); puts_c("\n", 0);
	}
	dump_regs("CTL0 ", CTL0, 0x00, 12);
	dump_regs("INTF1 ", INTF1, 0x280, 16);
	kick();

	usb_start();
	kick();

	uint64_t start = now(), next_tick = 0, next_log = 10;
	for (;;) {
		usb_poll();
		uint64_t t = (now() - start) * 1000 / freq();	/* ms */
		if (t >= RUN_SECONDS * 1000)
			break;
		if (t >= next_tick) {
			next_tick = t + 500;
			int r = row, c = col;
			log_on = 0;		/* the ticker is screen-only */
			row = TICK_ROW; col = 0;
			puts_c("alive ", C_GRAY);
			putdec(t / 1000, C_GREEN);
			puts_c(" s  usb ev ", C_GRAY);
			putdec(usb_events(), C_WHITE);
			puts_c(usb_configured() ? " LOG ON " : "        ", C_GREEN);
			puts_c("reset at 120 ", C_GRAY);
			row = r; col = c;
			log_on = 1;
			kick();
		}
		if (t / 1000 >= next_log) {
			next_log += 10;
			puts_c("t=", C_GRAY); putdec(t / 1000, C_WHITE);
			puts_c(" usb events ", C_GRAY); putdec(usb_events(), C_WHITE);
			puts_c(usb_configured() ? " configured\n" : "\n", C_WHITE);
		}
	}
	puts_c("reset\n", C_YEL);
	delay_ms(200);
	psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;) ;
}
