#!/usr/bin/env python3
"""Minimal Android boot image v4 (kernel + ramdisk, page 4096, no signature).

  mkbootimg4.py KERNEL OUT.img [--ramdisk FILE] [--cmdline STR]

ABL adds the flashed vendor_boot (its ramdisk and DTB) when it boots this
with `fastboot boot`. Without --ramdisk an empty gzip'd newc cpio is used.
"""
import argparse, gzip, struct

PAGE = 4096


def empty_cpio():
    name = b"TRAILER!!!\0"
    hdr = b"070701" + b"".join(b"%08X" % v for v in
                                (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0))
    blob = hdr + name
    blob += b"\0" * (-len(blob) % 4)
    return gzip.compress(blob + b"\0" * (-len(blob) % 512), mtime=0)


def pad(b):
    return b + b"\0" * (-len(b) % PAGE)


ap = argparse.ArgumentParser()
ap.add_argument("kernel")
ap.add_argument("out")
ap.add_argument("--ramdisk")
ap.add_argument("--cmdline", default="")
a = ap.parse_args()
kernel = open(a.kernel, "rb").read()
ramdisk = open(a.ramdisk, "rb").read() if a.ramdisk else empty_cpio()
cmd = a.cmdline.encode()
assert len(cmd) < 1536
# boot_img_hdr_v4: magic, kernel_size, ramdisk_size, os_version, header_size,
# reserved[4], header_version, cmdline[1536], signature_size
hdr = struct.pack("<8sIIII4II1536sI", b"ANDROID!", len(kernel), len(ramdisk),
                  0, 1584, 0, 0, 0, 0, 4, cmd, 0)
assert len(hdr) == 1584
open(a.out, "wb").write(pad(hdr) + pad(kernel) + pad(ramdisk))
