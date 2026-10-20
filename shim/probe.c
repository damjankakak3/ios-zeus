/*
 * zeus probe 4: screen + USB serial log (usb.c), the memory watch from
 * probe 3 (the noise was stale ABL cache lines: fixed by DC CIVAC), and a
 * pointer authentication test (can EL1 set the PAC keys under Gunyah?).
 *
 * Safety: no storage code. Memory writes: own image/stack/buffers and the
 * ABL splash framebuffer, and a 32 MB test area at MEMTEST_BASE (free RAM
 * between our image and the DTB ABL passed). Register writes (all via wr32): CTL_0 START of
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

/* Two 16 MB test areas: A is cleaned+invalidated from the caches before the
 * pattern is written, B is not. Inside /memory, below the DTB (0xb71fc000
 * on zeus), not in any static reserved region. */
#define MEMTEST_BASE	0xb0000000UL
#define MEMTEST_HALF	0x01000000UL

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
void screen_kick(void) { kick(); }

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

uint32_t fdt_be32(const void *p)
{
	const uint8_t *b = p;
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

uint64_t fdt_be64(const void *p) { return (uint64_t)fdt_be32(p) << 32 | fdt_be32((const uint8_t *)p + 4); }
#define be32 fdt_be32
#define be64 fdt_be64

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
const uint8_t *fdt_prop(const uint8_t *dtb, const char *path, const char *prop, uint32_t *len)
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


/* For each direct child of the node at `path` (a one-component path such as
 * "/reserved-memory"): cb(child name, base, size) per <u64 base, u64 size>
 * pair of its "reg" (2 address + 2 size cells, as on SM8450). */
void fdt_children_reg(const uint8_t *dtb, const char *path, void (*cb)(const char *, uint64_t, uint64_t))
{
	if (be32(dtb) != 0xd00dfeed)
		return;
	const uint8_t *p = dtb + be32(dtb + 8);
	const char *strs = (const char *)dtb + be32(dtb + 12);
	const char *want = path + 1;
	int wl = 0;
	while (want[wl]) wl++;
	int depth = 0, in_parent = 0;
	const char *child = 0;
	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == 1) {
			const char *name = (const char *)p;
			int nl = 0;
			while (name[nl]) nl++;
			p += (nl + 4) & ~3;
			depth++;
			if (depth == 2 && name_match(name, want, wl))
				in_parent = 1;
			else if (depth == 3 && in_parent)
				child = name;
		} else if (tok == 2) {
			if (depth == 2 && in_parent)
				return;
			if (depth == 3)
				child = 0;
			depth--;
		} else if (tok == 3) {
			uint32_t l = be32(p), nameoff = be32(p + 4);
			const uint8_t *val = p + 8;
			p += 8 + ((l + 3) & ~3);
			if (depth == 3 && in_parent && child && streq(strs + nameoff, "reg"))
				for (uint32_t i = 0; i + 16 <= l; i += 16)
					cb(child, be64(val + i), be64(val + i + 8));
		} else if (tok == 4) {
		} else {
			return;
		}
	}
}

/* ---------- caches and memory watch ---------- */

/* DC CIVAC by address works with the MMU off too: write back and drop any
 * lines ABL left in the caches for [base, base+size). */
static void cache_flush(uint64_t base, uint64_t size)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	uint64_t line = 4UL << ((ctr >> 16) & 0xf);
	for (uint64_t a = base & ~(line - 1); a < base + size; a += line)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t pattern(uint64_t a) { return (uint32_t)(a >> 2) ^ 0xa5a5a5a5U; }

static void mem_fill(uint64_t base, uint64_t size)
{
	for (uint64_t a = base; a < base + size; a += 4)
		*(volatile uint32_t *)a = pattern(a);
}

/* mismatching words in [base, base+size); first bad address and value */
static uint64_t mem_check(uint64_t base, uint64_t size, uint64_t *first, uint32_t *val)
{
	uint64_t bad = 0;
	for (uint64_t a = base; a < base + size; a += 4) {
		uint32_t v = *(volatile uint32_t *)a;
		if (v != pattern(a)) {
			if (!bad++) { *first = a; *val = v; }
		}
	}
	return bad;
}

#define BANDS	32		/* 100 lines each */
static uint32_t band_sum[BANDS];

static uint32_t band_hash(int b)
{
	uint32_t h = 0;
	uint64_t lines = fb_h / BANDS;
	volatile uint32_t *p = (volatile uint32_t *)(fb + b * lines * fb_stride);
	for (uint64_t i = 0; i < lines * fb_stride / 4; i += 4)	/* every 4th word */
		h = (h << 5 | h >> 27) ^ p[i];
	return h;
}

static void bands_snapshot(void)
{
	for (int b = 0; b < BANDS; b++)
		band_sum[b] = band_hash(b);
}

/* Log which 100-line bands changed since the last snapshot. */
static void bands_report(void)
{
	static uint8_t changed[BANDS];
	int first = -1, n = 0;
	for (int b = 0; b < BANDS; b++) {	/* hash everything before printing anything */
		uint32_t h = band_hash(b);
		uint32_t y0 = b * (fb_h / BANDS), ty = MARGIN + TICK_ROW * CH;
		changed[b] = h != band_sum[b] && !(y0 + fb_h / BANDS > ty && y0 < ty + CH);
		if (changed[b]) {
			if (first < 0) first = b;
			n++;
		}
	}
	uint32_t sample[4] = { 0 };
	uint64_t a = (uint64_t)fb + (first < 0 ? 0 : first) * (fb_h / BANDS) * fb_stride;
	for (int i = 0; i < 4; i++)
		sample[i] = *(volatile uint32_t *)(a + 4 * i);
	puts_c("fb bands changed:", C_GRAY);
	for (int b = 0; b < BANDS; b++)
		if (changed[b]) {
			puts_c(" ", 0);
			putdec(b, C_WHITE);
		}
	puts_c(n ? "\n" : " none\n", n ? C_WHITE : C_GREEN);
	if (first >= 0) {
		puts_c("  band ", C_GRAY); putdec(first, C_WHITE);
		puts_c(" @", C_GRAY); puthex(a, 10, C_WHITE);
		puts_c(":", C_GRAY);
		for (int i = 0; i < 4; i++) {
			puts_c(" ", 0);
			puthex(sample[i], 8, C_WHITE);
		}
		puts_c("\n", 0);
	}
}

static void mem_report(void)
{
	for (int k = 0; k < 2; k++) {
		uint64_t first = 0;
		uint32_t val = 0;
		uint64_t base = MEMTEST_BASE + k * MEMTEST_HALF;
		uint64_t bad = mem_check(base, MEMTEST_HALF, &first, &val);
		puts_c(k ? "  memB(no flush) bad " : "  memA(flushed) bad ", C_GRAY);
		putdec(bad, bad ? C_RED : C_GREEN);
		if (bad) {
			puts_c(" first ", C_GRAY); puthex(first, 10, C_WHITE);
			puts_c("=", C_GRAY); puthex(val, 8, C_WHITE);
		}
		puts_c("\n", 0);
	}
}


/* ---------- pointer authentication ---------- */

#define APIAKEYLO	"S3_0_C2_C1_0"
#define APIAKEYHI	"S3_0_C2_C1_1"
#define ID_AA64ISAR1	"S3_0_C0_C6_1"
#define ID_AA64ISAR2	"S3_0_C0_C6_2"

/* PAC with key IA: does EL1 own the keys, and does sign + auth round-trip?
 * A trap to the hypervisor would show up as the red exception screen. */
static void pac_test(void)
{
	uint64_t isar1, isar2, sctlr, lo, hi;
	__asm__ volatile("mrs %0, " ID_AA64ISAR1 "; mrs %1, " ID_AA64ISAR2 : "=r"(isar1), "=r"(isar2));
	line("PAC ISAR1 ", isar1, 16);
	line("PAC ISAR2 ", isar2, 16);
	puts_c("  APA ", C_GRAY); putdec((isar1 >> 4) & 0xf, C_WHITE);
	puts_c(" API ", C_GRAY); putdec((isar1 >> 8) & 0xf, C_WHITE);
	puts_c(" GPA ", C_GRAY); putdec((isar1 >> 24) & 0xf, C_WHITE);
	puts_c(" GPI ", C_GRAY); putdec((isar1 >> 28) & 0xf, C_WHITE);
	puts_c(" APA3 ", C_GRAY); putdec((isar2 >> 12) & 0xf, C_WHITE);
	puts_c("\n", 0);
	if (!(((isar1 >> 4) & 0xf) || ((isar1 >> 8) & 0xf) || ((isar2 >> 12) & 0xf))) {
		puts_c("PAC: not implemented\n", C_RED);
		return;
	}
	puts_c("PAC: writing APIAKey...\n", C_GRAY);
	__asm__ volatile("msr " APIAKEYLO ", %0; msr " APIAKEYHI ", %1; isb"
			 :: "r"(0x0123456789abcdefUL), "r"(0xfedcba9876543210UL));
	__asm__ volatile("mrs %0, " APIAKEYLO "; mrs %1, " APIAKEYHI : "=r"(lo), "=r"(hi));
	line("  key lo  ", lo, 16);
	line("  key hi  ", hi, 16);
	__asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	__asm__ volatile("msr sctlr_el1, %0; isb" :: "r"(sctlr | (1UL << 31)));	/* EnIA */
	uint64_t p = 0x0000001234567890UL, mod = 0x42, signed_p = p, auth = 0;
	__asm__ volatile(".arch_extension pauth\n pacia %0, %1" : "+r"(signed_p) : "r"(mod));
	auth = signed_p;
	__asm__ volatile(".arch_extension pauth\n autia %0, %1" : "+r"(auth) : "r"(mod));
	__asm__ volatile("msr sctlr_el1, %0; isb" :: "r"(sctlr));		/* back as it was */
	line("  ptr     ", p, 16);
	line("  signed  ", signed_p, 16);
	line("  auth    ", auth, 16);
	puts_c(auth == p && signed_p != p ? "PAC: WORKS\n" : "PAC: FAILED\n",
	       auth == p && signed_p != p ? C_GREEN : C_RED);
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
	cache_flush((uint64_t)fb, fb_size);	/* drop ABL's cache lines first */
	rect(0, 0, fb_w, fb_h, C_BG);
	bands();
	puts_c("ios-zeus probe 5 (probe 4 + xnu loader)\n", C_YEL);
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
	pac_test();
	kick();

	/* xnu payload in the ramdisk (tools/mkpayload.py): boot it (loader.c). */
	if (fdt_prop(dtb, "/chosen", "linux,initrd-start", &l)) {
		extern char _head[], _end[];
		const char *why = loader_boot(dtb, (uint64_t)_head, (uint64_t)_end, (uint64_t)fb, fb_size);
		if (why) {
			puts_c("LOADER STOPPED: ", C_RED);
			puts_c(why, C_WHITE);
			puts_c("\nnothing was written. reset in 30 s\n", C_WHITE);
			kick();
			uint64_t until = now() + 30 * freq();
			while (now() < until)
				usb_poll();
			psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
			for (;;) ;
		}
	}

	cache_flush(MEMTEST_BASE, MEMTEST_HALF);
	mem_fill(MEMTEST_BASE, 2 * MEMTEST_HALF);
	puts_c("memtest A ", C_GRAY); puthex(MEMTEST_BASE, 10, C_WHITE);
	puts_c(" B ", C_GRAY); puthex(MEMTEST_BASE + MEMTEST_HALF, 10, C_WHITE);
	puts_c(" 16M each, fb flushed\n", C_GRAY);
	mem_report();
	bands_snapshot();

	uint64_t start = now(), next_tick = 0, next_watch = 5;
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
		if (t / 1000 >= next_watch) {
			next_watch += 5;
			bands_report();		/* changes since the last snapshot */
			mem_report();
			puts_c("t=", C_GRAY); putdec(t / 1000, C_WHITE);
			puts_c(" usb events ", C_GRAY); putdec(usb_events(), C_WHITE);
			puts_c(usb_configured() ? " configured\n" : "\n", C_WHITE);
			bands_snapshot();	/* after our own printing */
		}
	}
	puts_c("reset\n", C_YEL);
	delay_ms(200);
	psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;) ;
}
