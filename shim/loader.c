/*
 * zeus xnu loader: act as iBoot for the ZEUS kernel collection.
 * Port of ios-sweet shim/loader.c, changed for zeus:
 *   - the payload (tools/mkpayload.py) is the boot.img ramdisk, which ABL
 *     appends after the vendor_boot ramdisk: find it by the trailer
 *     {u64 size, "ZEUSEND1"} at initrd-end, or by scanning for the magic;
 *     it may start at any byte, so copies handle unaligned sources
 *   - the RAM given to xnu is checked against the live DT: inside one
 *     /memory bank, no /reserved-memory region, and not touching this
 *     image, the DTB, the ramdisk or the framebuffer
 *   - stale ABL cache lines: every destination line is cleaned+invalidated
 *     (DC CIVAC) before the MMU-off writes, so nothing dirty can be evicted
 *     over the kernel later
 *   - the USB log gets 10 s to reach the PC, then the device disconnects
 *
 * Writes only RAM inside [phys_base, phys_base + mem_size) from the ADT.
 * No storage code. If anything is wrong it returns a reason and the probe
 * resets the phone (Android boots again).
 */
#include "probe.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL
#define MAX_SEGS		64

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* ---- unaligned-safe access to the payload (MMU off: Device memory) ---- */

static uint8_t rd8(uint64_t a) { return *(volatile uint8_t *)a; }

static uint64_t ld64(uint64_t a)
{
	if (!(a & 7))
		return *(volatile uint64_t *)a;
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | rd8(a + i);
	return v;
}

static uint32_t ld32(uint64_t a)
{
	if (!(a & 3))
		return *(volatile uint32_t *)a;
	return (uint32_t)rd8(a) | (uint32_t)rd8(a + 1) << 8 | (uint32_t)rd8(a + 2) << 16 | (uint32_t)rd8(a + 3) << 24;
}

/* dst is 8-byte aligned (segments and the ADT start on 16K pages) */
static void copy_out(uint64_t dst, uint64_t src, uint64_t n)
{
	uint64_t i = 0;
	if (!(src & 7))
		for (; i + 8 <= n; i += 8)
			*(volatile uint64_t *)(dst + i) = *(volatile uint64_t *)(src + i);
	else
		for (; i + 8 <= n; i += 8)
			*(volatile uint64_t *)(dst + i) = ld64(src + i);
	for (; i < n; i++)
		*(volatile uint8_t *)(dst + i) = rd8(src + i);
}

static void zero_out(uint64_t dst, uint64_t n)
{
	uint64_t i = 0;
	for (; i < n && ((dst + i) & 7); i++)
		*(volatile uint8_t *)(dst + i) = 0;
	for (; i + 8 <= n; i += 8)
		*(volatile uint64_t *)(dst + i) = 0;
	for (; i < n; i++)
		*(volatile uint8_t *)(dst + i) = 0;
}

static void civac(uint64_t base, uint64_t size)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	uint64_t lsz = 4UL << ((ctr >> 16) & 0xf);
	for (uint64_t a = base & ~(lsz - 1); a < base + size; a += lsz)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static int overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs)
{
	return a < b + bs && b < a + as;
}

/* ---- payload lookup ---- */

static int magic_at(uint64_t a, const char *m)
{
	for (int i = 0; i < 8; i++)
		if (rd8(a + i) != (uint8_t)m[i])
			return 0;
	return 1;
}

static uint64_t find_payload(uint64_t rd_start, uint64_t rd_end)
{
	if (rd_end - rd_start > 16 && magic_at(rd_end - 8, "ZEUSEND1")) {
		uint64_t size = ld64(rd_end - 16);
		if (size <= rd_end - rd_start && magic_at(rd_end - size, "ZEUSXNU1"))
			return rd_end - size;
	}
	puts_c("no trailer, scanning the ramdisk\n", C_GRAY);
	for (uint64_t a = rd_start; a + 8 <= rd_end; a++)
		if (rd8(a) == 'Z' && magic_at(a, "ZEUSXNU1"))
			return a;
	return 0;
}

static uint64_t chosen_u(const uint8_t *dtb, const char *prop, int *ok)
{
	uint32_t l;
	const uint8_t *v = fdt_prop(dtb, "/chosen", prop, &l);
	*ok = v && (l == 4 || l == 8);
	if (!*ok)
		return 0;
	return l == 8 ? fdt_be64(v) : fdt_be32(v);
}

/* ---- checks against the live DT ---- */

static int in_memory_bank(const uint8_t *dtb, uint64_t base, uint64_t size)
{
	uint32_t l;
	const uint8_t *v = fdt_prop(dtb, "/memory", "reg", &l);
	for (uint32_t i = 0; v && i + 16 <= l; i += 16) {
		uint64_t b = fdt_be64(v + i), s = fdt_be64(v + i + 8);
		if (base >= b && base + size <= b + s)
			return 1;
	}
	return 0;
}

static int bad_reserved;
static uint64_t chk_base, chk_size;

static void reserved_cb(const char *name, uint64_t base, uint64_t size)
{
	if (!overlaps(base, size, chk_base, chk_size))
		return;
	bad_reserved++;
	puts_c("RESERVED OVERLAP ", C_RED);
	puts_c(name, C_WHITE);
	puts_c(" ", 0);
	puthex(base, 10, C_WHITE);
	puts_c(" ", 0);
	puthex(size, 8, C_WHITE);
	puts_c("\n", 0);
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

const char *loader_boot(const uint8_t *dtb, uint64_t image_lo, uint64_t image_hi,
			uint64_t fb_base, uint64_t fb_size)
{
	int ok1, ok2;
	uint64_t rd_start = chosen_u(dtb, "linux,initrd-start", &ok1);
	uint64_t rd_end = chosen_u(dtb, "linux,initrd-end", &ok2);
	if (!ok1 || !ok2 || rd_end <= rd_start)
		return "no initrd in /chosen";
	line("initrd ", rd_start, 10);
	line("   end ", rd_end, 10);

	uint64_t p = find_payload(rd_start, rd_end);
	if (!p)
		return "no ZEUSXNU1 payload in the ramdisk";
	line("payload", p, 10);
	if (ld32(p + 8) != 1)
		return "payload version is not 1";
	uint32_t nsegs = ld32(p + 12);
	uint64_t virt_base = ld64(p + 0x10), entry_va = ld64(p + 0x18);
	uint64_t adt_off = ld64(p + 0x20), adt_size = ld64(p + 0x28);
	uint64_t kernel_end = ld64(p + 0x30), phys_base = ld64(p + 0x38), mem_size = ld64(p + 0x40);
	uint64_t to_pa = phys_base - virt_base;		/* pa = va + to_pa */
	if (nsegs == 0 || nsegs > MAX_SEGS)
		return "bad segment count";
	line("phys   ", phys_base, 10);
	line("size   ", mem_size, 10);
	line("virt   ", virt_base, 16);
	line("entry  ", entry_va, 16);
	line("segs   ", nsegs, 2);

	uint64_t mpidr;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	if (mpidr & 0xffff)
		return "not running on core 0 (ADT marks cpu0 as the boot CPU)";
	if (!in_memory_bank(dtb, phys_base, mem_size))
		return "xnu RAM is not inside one live /memory bank";
	chk_base = phys_base;
	chk_size = mem_size;
	bad_reserved = 0;
	fdt_children_reg(dtb, "/reserved-memory", reserved_cb);
	if (bad_reserved)
		return "xnu RAM overlaps /reserved-memory";
	if (overlaps(rd_start, rd_end - rd_start, phys_base, mem_size) ||
	    overlaps(image_lo, image_hi - image_lo, phys_base, mem_size) ||
	    overlaps((uint64_t)dtb, 0x200000, phys_base, mem_size) ||
	    overlaps(fb_base, fb_size, phys_base, mem_size))
		return "xnu RAM overlaps the ramdisk, the loader, the DTB or the framebuffer";

	/* Layout: segments at their VA, then ADT, then boot_args (16K pages). */
	uint64_t lo = ~0UL;
	for (uint32_t i = 0; i < nsegs; i++) {
		uint64_t s = p + 0x2a8 + 32 * i;
		uint64_t va = ld64(s), vs = ld64(s + 8);
		if (va < virt_base || va + vs > kernel_end || (va & 7))
			return "segment outside the kernel range";
		if (va < lo)
			lo = va;
	}
	uint64_t adt_pa = kernel_end + to_pa;
	uint64_t ba_pa = (adt_pa + adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uint64_t top = ba_pa + PAGE_16K;
	if (top > phys_base + mem_size)
		return "kernel + ADT do not fit";
	line("adt    ", adt_pa, 10);
	line("bootarg", ba_pa, 10);
	line("top    ", top, 10);
	puts_c("checks passed. USB log time: 10 s\n", C_GREEN);
	screen_kick();

	uint64_t until = now() + 10 * freq();
	while (now() < until)
		usb_poll();
	usb_stop();

	puts_c("copying kernel...\n", C_GRAY);
	screen_kick();
	civac(lo + to_pa, top - (lo + to_pa));
	for (uint32_t i = 0; i < nsegs; i++) {
		uint64_t s = p + 0x2a8 + 32 * i;
		uint64_t va = ld64(s), vs = ld64(s + 8), off = ld64(s + 16), fs = ld64(s + 24);
		if (fs > vs)
			fs = vs;
		copy_out(va + to_pa, p + off, fs);
		zero_out(va + to_pa + fs, vs - fs);
	}
	copy_out(adt_pa, p + adt_off, adt_size);

	zero_out(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = virt_base;
	ba->physBase = phys_base;
	ba->memSize = mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = fb_base;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = 1440 * 4;
	ba->Video.v_width = 1440;
	ba->Video.v_height = 3200;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - to_pa;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = (char)rd8(p + 0x48 + i);
	ba->memSizeActual = mem_size;

	uint64_t entry_pa = entry_va + to_pa;
	line("jump   ", entry_pa, 10);
	puts_c("jumping to xnu (ZEUS marks follow)\n", C_YEL);
	screen_kick();
	delay_ms(50);

	/* Everything was written MMU-off; drop stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
