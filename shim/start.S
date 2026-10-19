/*
 * zeus probe entry. arm64 Linux Image header so ABL loads us like a kernel
 * (header layout from the ios-sweet shim). ABL enters at EL1, MMU and
 * D-cache off, x0 = DTB. Everything here is PC-relative: ABL picks the
 * load address.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0			/* DTB from ABL */
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	adr	x1, _head
	bl	probe_main
3:	wfe
	b	3b

/* Every exception goes to probe_exception(slot): red screen, then reset. */
.macro vent slot
	.balign	0x80
	mov	x0, #\slot
	b	exc_common
.endm

	.balign	0x800
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	adr	x1, _stack_top		/* fresh stack: sp may be the problem */
	mov	sp, x1
	bl	probe_exception
4:	wfe
	b	4b

/* PSCI call through SMC (zeus DT: psci method = "smc"). */
	.globl psci_call
psci_call:
	smc	#0
	ret
