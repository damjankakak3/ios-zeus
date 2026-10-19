/*
 * zeus probe, stage 1: does `fastboot boot` reach our code, and can we
 * get pixels onto the command-mode panel?
 *
 * Safety: this program has no storage code. The only memory it writes is
 * its own image, its stack and the ABL splash framebuffer. The only
 * hardware register it writes is CTL_0 START of the display controller
 * (mdp_write below). Everything else is reads. It ends with PSCI
 * SYSTEM_RESET; Android boots again after that.
 *
 * Runs with the MMU off (all data accesses are Device memory: keep them
 * aligned, -mstrict-align).
 */
#include <stdint.h>
#include "font.h"

uint64_t psci_call(uint64_t fn, uint64_t a, uint64_t b, uint64_t c);

#define PSCI_SYSTEM_RESET	0x84000009UL

/* Display controller (stock DT: mdp_phys 0xae00000, offsets from it) */
#ifndef MDP_BASE
#define MDP_BASE	0x0ae00000UL	/* tests override it */
#endif
#define CTL0		(MDP_BASE + 0x16000)	/* qcom,sde-ctl-off[0], "primary" */
#define CTL_START	0x1c
#define INTF1		(MDP_BASE + 0x36000)	/* first DSI interface */
#define PP0		(MDP_BASE + 0x6a000)
#define SSPP_SRC_SIZE	0x00
#define SSPP_SRC0_ADDR	0x14
#define SSPP_YSTRIDE0	0x24
#define SSPP_FORMAT	0x30

/* splash_region in the stock DT; replaced by the live DT value */
#define SPLASH_BASE	0xb8000000UL
#define SPLASH_SIZE	0x2b00000UL

static volatile uint8_t *fb = (volatile uint8_t *)SPLASH_BASE;
static uint64_t fb_size = SPLASH_SIZE;
static uint32_t fb_w = 1440, fb_h = 3200, fb_stride = 1440 * 4, fb_bpp = 4;

static inline uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)a; }

/* The one register write in this program. */
static void mdp_write(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t now(void) { uint64_t v; __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v)); return v; }
static uint64_t freq(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v ? v : 19200000; }
static void delay_ms(uint64_t ms) { uint64_t end = now() + freq() / 1000 * ms; while (now() < end) ; }

/* ---------- framebuffer text ---------- */

#define SCALE	4
#define CW	(8 * SCALE)
#define CH	(16 * SCALE)
#define MARGIN	32

#define C_BG	0x00000000U
#define C_WHITE	0x00ffffffU
#define C_GRAY	0x00a0a0a0U
#define C_RED	0x00ff0000U
#define C_GREEN	0x0000ff00U
#define C_BLUE	0x000000ffU
#define C_YEL	0x00ffff00U

static int row;

static void px(uint32_t x, uint32_t y, uint32_t c)
{
	uint64_t off = (uint64_t)y * fb_stride + (uint64_t)x * fb_bpp;
	if (x >= fb_w || y >= fb_h || off + fb_bpp > fb_size)
		return;
	if (fb_bpp == 4) {
		*(volatile uint32_t *)(fb + off) = 0xff000000U | c;
	} else {
		fb[off] = c; fb[off + 1] = c >> 8; fb[off + 2] = c >> 16;
	}
}

static void rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t c)
{
	for (uint32_t y = y0; y < y0 + h; y++)
		for (uint32_t x = x0; x < x0 + w; x++)
			px(x, y, c);
}

static void putc_at(int col, int r, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	uint32_t x0 = MARGIN + col * CW, y0 = MARGIN + r * CH;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++)
			rect(x0 + gx * SCALE, y0 + gy * SCALE, SCALE, SCALE,
			     (g[gy] & (0x80 >> gx)) ? fg : C_BG);
}

static int col;

static void puts_c(const char *s, uint32_t fg)
{
	for (; *s; s++) {
		if (*s == '\n') { row++; col = 0; continue; }
		putc_at(col++, row, *s, fg);
	}
}

static void puthex(uint64_t v, int digits, uint32_t fg)
{
	char b[17];
	for (int i = 0; i < digits; i++)
		b[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 0xf];
	b[digits] = 0;
	puts_c(b, fg);
}

static void putdec(uint64_t v, uint32_t fg)
{
	char b[21];
	int i = 20;
	b[i] = 0;
	do { b[--i] = '0' + v % 10; v /= 10; } while (v);
	puts_c(b + i, fg);
}

static void line(const char *label, uint64_t v, int digits)
{
	puts_c(label, C_GRAY);
	puthex(v, digits, C_WHITE);
	puts_c("\n", 0);
}

static void clear(void)
{
	rect(0, 0, fb_w, fb_h, C_BG);
	row = col = 0;
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

static void kick(void)
{
	mdp_write(CTL0 + CTL_START, 1);
}

void probe_exception(uint64_t slot)
{
	static int nested;

	uint64_t esr, elr, far;
	__asm__ volatile("mrs %0, esr_el1; mrs %1, elr_el1; mrs %2, far_el1" : "=r"(esr), "=r"(elr), "=r"(far));
	rect(0, 0, fb_w, 12 * CH, C_RED);
	row = 1; col = 0;
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
		puts_c((i % 4 == 3) ? "\n" : " ", 0);
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

	/* 1. draw with the DT defaults: visible already if the panel refreshes itself */
	clear();
	rect(0, fb_h - 400, fb_w / 3, 400, C_RED);
	rect(fb_w / 3, fb_h - 400, fb_w / 3, 400, C_GREEN);
	rect(2 * fb_w / 3, fb_h - 400, fb_w - 2 * (fb_w / 3), 400, C_BLUE);
	puts_c("ios-zeus probe 1\n", C_YEL);
	puts_c("bands: red green blue\n\n", C_WHITE);
	kick();

	/* 2. find the pipe ABL scans out from; trust its geometry */
	static const uint32_t sspp_off[8] = { 0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000 };
	int found = -1;
	uint32_t s_addr = 0, s_size = 0, s_stride = 0, s_fmt = 0;
	for (int i = 0; i < 8; i++) {
		uint64_t b = MDP_BASE + sspp_off[i];
		uint32_t a = rd32(b + SSPP_SRC0_ADDR);
		if (a >= (uint64_t)fb && a < (uint64_t)fb + fb_size) {
			found = i;
			s_addr = a;
			s_size = rd32(b + SSPP_SRC_SIZE);
			s_stride = rd32(b + SSPP_YSTRIDE0) & 0xffff;
			s_fmt = rd32(b + SSPP_FORMAT);
			break;
		}
	}
	if (found >= 0 && (s_size & 0xffff) && s_stride) {
		uint32_t w = s_size & 0xffff, h = s_size >> 16;
		uint32_t bpp = s_stride / w;
		if ((bpp == 3 || bpp == 4) && h) {
			fb_size -= s_addr - (uint64_t)fb;
			fb = (volatile uint8_t *)(uint64_t)s_addr;
			fb_w = w; fb_h = h; fb_stride = s_stride; fb_bpp = bpp;
			clear();
			rect(0, fb_h - 400, fb_w / 3, 400, C_RED);
			rect(fb_w / 3, fb_h - 400, fb_w / 3, 400, C_GREEN);
			rect(2 * fb_w / 3, fb_h - 400, fb_w - 2 * (fb_w / 3), 400, C_BLUE);
			puts_c("ios-zeus probe 1\n", C_YEL);
			puts_c("bands: red green blue\n\n", C_WHITE);
		}
	}
	kick();

	uint64_t el, mpidr, sctlr;
	__asm__ volatile("mrs %0, CurrentEL; mrs %1, mpidr_el1; mrs %2, sctlr_el1" : "=r"(el), "=r"(mpidr), "=r"(sctlr));
	line("EL     ", el >> 2, 1);
	line("MPIDR  ", mpidr, 8);
	line("SCTLR  ", sctlr, 8);
	line("load   ", load, 10);
	line("dtb    ", (uint64_t)dtb, 10);
	line("dtbsz  ", be32(dtb + 4), 8);
	line("cntfrq ", freq(), 8);
	line("fb     ", (uint64_t)fb, 10);
	line("fbsize ", fb_size, 8);
	puts_c("sspp   ", C_GRAY);
	puthex(found & 0xff, 2, C_WHITE);
	puts_c(" ", 0); puthex(s_size, 8, C_WHITE);
	puts_c(" ", 0); puthex(s_stride, 4, C_WHITE);
	puts_c(" ", 0); puthex(s_fmt, 8, C_WHITE);
	puts_c("\n", 0);

	v = fdt_prop(dtb, "/memory", "reg", &l);
	puts_c("memory (base size)\n", C_GRAY);
	for (uint32_t i = 0; v && i + 16 <= l && i < 16 * 8; i += 16) {
		puthex(be64(v + i), 10, C_WHITE);
		puts_c(" ", 0);
		puthex(be64(v + i + 8), 10, C_WHITE);
		puts_c("\n", 0);
	}
	if (!v)
		puts_c("  none\n", C_RED);

	dump_regs("CTL0 ", CTL0, 0x00, 12);
	dump_regs("INTF1 ", INTF1, 0x280, 16);
	dump_regs("PP0 ", PP0, 0x00, 16);

	/* 3. keep sending frames for a minute so the panel shows them, then reset */
	int tick_row = row + 1;
	uint64_t start = now();
	for (int t = 0; t < 240; t++) {
		row = tick_row; col = 0;
		puts_c("alive ", C_GRAY);
		putdec((now() - start) / freq(), C_GREEN);
		puts_c(" s, reset at 60  \n", C_GRAY);
		kick();
		delay_ms(250);
	}
	psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;) ;
}
