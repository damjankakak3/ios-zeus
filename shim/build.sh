#!/bin/bash
# Build the zeus probe (stage 3) into out/zeus-probe3.img. Needs clang + ld.lld.
set -euo pipefail
cd "$(dirname "$0")"
CC="clang --target=aarch64-none-elf"
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mcmodel=small -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
$CC $CFLAGS -c start.S -o out/start.o
$CC $CFLAGS -c probe.c -o out/probe.o
$CC $CFLAGS -c usb.c -o out/usb.o
# ABL picks the load address and there is no relocation pass: only the
# header's image_size (start.o) may be an absolute address.
if llvm-readelf -r out/probe.o out/usb.o | grep -q ABS64; then
	echo "error: absolute relocations in C objects" >&2; exit 1
fi
ld.lld -T probe.ld --build-id=none -N out/start.o out/probe.o out/usb.o -o out/probe.elf
llvm-objcopy -O binary out/probe.elf out/Image
# ABL copies the file; pad to image_size so .bss and the stack exist.
size=$((16#$(llvm-nm out/probe.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" out/Image
python3 ../tools/mkbootimg4.py out/Image out/zeus-probe3.img
sha256sum out/zeus-probe3.img
