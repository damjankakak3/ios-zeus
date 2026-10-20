#!/bin/bash
# Build the zeus probe 5 (probe 4 + xnu loader) into out/zeus-probe5.img. Needs clang + ld.lld.
# OUT=dir and EXTRA_CFLAGS=... are for tools/qemu-loader-test.py.
set -euo pipefail
cd "$(dirname "$0")"
CC="clang --target=aarch64-none-elf"
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mcmodel=small -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -Werror ${EXTRA_CFLAGS:-}"
OUT=${OUT:-out}
mkdir -p "$OUT"
$CC $CFLAGS -c start.S -o "$OUT"/start.o
$CC $CFLAGS -c probe.c -o "$OUT"/probe.o
$CC $CFLAGS -c usb.c -o "$OUT"/usb.o
$CC $CFLAGS -c loader.c -o "$OUT"/loader.o
# ABL picks the load address and there is no relocation pass: only the
# header's image_size (start.o) may be an absolute address.
if llvm-readelf -r "$OUT"/probe.o "$OUT"/usb.o "$OUT"/loader.o | grep -q ABS64; then
	echo "error: absolute relocations in C objects" >&2; exit 1
fi
ld.lld -T probe.ld --build-id=none -N "$OUT"/start.o "$OUT"/probe.o "$OUT"/usb.o "$OUT"/loader.o -o "$OUT"/probe.elf
llvm-objcopy -O binary "$OUT"/probe.elf "$OUT"/Image
# ABL copies the file; pad to image_size so .bss and the stack exist.
size=$((16#$(llvm-nm "$OUT"/probe.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" "$OUT"/Image
python3 ../tools/mkbootimg4.py "$OUT"/Image "$OUT"/zeus-probe5.img
sha256sum "$OUT"/zeus-probe5.img
