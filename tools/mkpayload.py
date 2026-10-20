#!/usr/bin/env python3
"""Pack the ZEUS kernel collection + ADT into the payload the loader boots.

  mkpayload.py KERNELCACHE ADT OUT [--cmdline "..."]

Port of ios-sweet tools/mkpayload.py, changed for an arm64e MH_FILESET kernel
collection (kmutil output, see .github/workflows/xnu.yml): the kernel rebases
and signs the collection's chained fixups itself at boot
(arm_slide_rebase_and_sign_image -> kernel_collection_slide), so this tool only
lays the segments out flat and finds the kernel's entry point. A plain
MH_EXECUTE kernel is refused: its auth pointers would never be signed.

Payload layout (little endian, offsets from payload start):
  0x000  char  magic[8] = "ZEUSXNU1"
  0x008  u32   version = 1, nsegs
  0x010  u64   virt_base      VA that maps to phys_base
  0x018  u64   entry_va       kernel LC_UNIXTHREAD pc
  0x020  u64   adt_off, adt_size
  0x030  u64   kernel_end_va  end of the highest segment (16K aligned)
  0x038  u64   phys_base      kernel-managed RAM (= ADT /chosen dram-base/size)
  0x040  u64   mem_size
  0x048  char  cmdline[608]
  0x2a8  seg[nsegs] { u64 vmaddr, vmsize, file_off (in payload), file_size }
Segment data and the ADT are 16-byte aligned inside the payload.
Trailer (last 16 bytes): u64 total payload size incl. trailer, "ZEUSEND1". ABL
appends this ramdisk after the vendor_boot one, so the loader finds the
payload start from initrd-end.
"""
import argparse, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

LC_SEGMENT_64, LC_UNIXTHREAD, LC_FILESET_ENTRY = 0x19, 0x5, 0x80000035
MH_EXECUTE, MH_FILESET = 0x2, 0xc
PAGE = 0x4000
L2_BLOCK = 0x2000000           # 32 MB: start.s maps the kernel with L2 blocks
BOOT_LINE_LENGTH = 608
CMDLINE = 0x48
SEG_TABLE = CMDLINE + BOOT_LINE_LENGTH  # 0x2a8
MAGIC = b'ZEUSXNU1'


def load_commands(buf, base=0):
    magic, cputype, cpusub, ftype, ncmds, _, _, _ = struct.unpack_from('<8I', buf, base)
    assert magic == 0xfeedfacf, 'not a 64-bit Mach-O'
    off = base + 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', buf, off)
        yield cmd, off, size
        off += size


def header(buf, base=0):
    _, cputype, cpusub, ftype = struct.unpack_from('<4I', buf, base)
    return cputype, cpusub & 0xff, ftype


def segments(buf, base=0):
    out = []
    for cmd, off, _ in load_commands(buf, base):
        if cmd == LC_SEGMENT_64:
            name = buf[off + 8:off + 24].split(b'\0')[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', buf, off + 24)
            out.append(dict(name=name, vmaddr=vmaddr, vmsize=vmsize, fileoff=fileoff, filesize=filesize))
    return out


def unixthread_pc(buf, base):
    for cmd, off, _ in load_commands(buf, base):
        if cmd == LC_UNIXTHREAD:
            # arm_thread_state64: x0..x28, fp, lr, sp, pc, cpsr
            return struct.unpack_from('<Q', buf, off + 16 + 32 * 8)[0]
    return None


def fileset_entries(buf):
    out = []
    for cmd, off, _ in load_commands(buf):
        if cmd == LC_FILESET_ENTRY:
            vmaddr, fileoff, name_off = struct.unpack_from('<QQI', buf, off + 8)
            name = buf[off + name_off:].split(b'\0')[0].decode()
            out.append(dict(id=name, vmaddr=vmaddr, fileoff=fileoff))
    return out


def adt_dram(adt_blob):
    root, _ = adt.parse(adt_blob)
    chosen = next(c for c in root.children if c.name == 'chosen')
    return (struct.unpack('<Q', chosen.prop('dram-base'))[0],
            struct.unpack('<Q', chosen.prop('dram-size'))[0])


def build(kc_path, adt_path, cmdline):
    buf = open(kc_path, 'rb').read()
    cputype, cpusub, ftype = header(buf)
    assert cputype == 0x0100000c, f'not arm64 (cputype {cputype:#x})'
    if ftype != MH_FILESET:
        raise SystemExit('not an MH_FILESET kernel collection: build it with kmutil (see xnu.yml); '
                         'a plain arm64e kernel never signs its own pointers')
    entries = fileset_entries(buf)
    kernel = next((e for e in entries if e['id'] == 'com.apple.kernel'), None)
    assert kernel, 'no com.apple.kernel in the fileset'
    entry = unixthread_pc(buf, kernel['fileoff'])
    assert entry, 'kernel has no LC_UNIXTHREAD'

    segs = [s for s in segments(buf) if s['vmsize']]
    hdr_va = min(s['vmaddr'] for s in segs if s['fileoff'] == 0)    # the KC mach header
    # virt_base: the 32 MB block holding the KC header, so start.s's first L2
    # block (it starts at the header's block) begins exactly at phys_base.
    virt_base = hdr_va & ~(L2_BLOCK - 1)
    kernel_end = max(s['vmaddr'] + s['vmsize'] for s in segs)
    kernel_end = (kernel_end + PAGE - 1) & ~(PAGE - 1)

    adt_blob = open(adt_path, 'rb').read()
    phys_base, mem_size = adt_dram(adt_blob)
    assert phys_base % L2_BLOCK == 0, 'dram-base must be 32 MB aligned'
    need = kernel_end - virt_base + len(adt_blob) + 2 * PAGE
    assert need < mem_size, 'kernel + ADT do not fit in dram-size'

    out = bytearray(SEG_TABLE + 32 * len(segs))
    table = []
    for s in segs:
        out += b'\0' * (-len(out) & 0xf)
        off = len(out)
        out += buf[s['fileoff']:s['fileoff'] + s['filesize']]
        table.append((s['vmaddr'], s['vmsize'], off, s['filesize']))
    out += b'\0' * (-len(out) & 0xf)
    adt_off = len(out)
    out += adt_blob
    out += b'\0' * (-len(out) & 0xf)

    struct.pack_into('<8sII', out, 0, MAGIC, 1, len(segs))
    struct.pack_into('<7Q', out, 0x10, virt_base, entry, adt_off, len(adt_blob), kernel_end,
                     phys_base, mem_size)
    cl = cmdline.encode()[:BOOT_LINE_LENGTH - 1]
    out[CMDLINE:CMDLINE + len(cl)] = cl
    for i, t in enumerate(table):
        struct.pack_into('<4Q', out, SEG_TABLE + 32 * i, *t)
    out += struct.pack('<Q8s', len(out) + 16, b'ZEUSEND1')
    info = dict(cpusub=cpusub, entries=len(entries), virt_base=hex(virt_base), kc_header=hex(hdr_va),
                entry=hex(entry), kernel_end=hex(kernel_end), phys_base=hex(phys_base),
                mem_size=hex(mem_size), size=len(out),
                segs=[(s['name'], hex(s['vmaddr']), hex(s['vmsize'])) for s in segs])
    return bytes(out), info


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('kernelcache'); ap.add_argument('adt'); ap.add_argument('out')
    ap.add_argument('--cmdline', default='debug=0x14e cpus=1 keepsyms=1 serial=3')
    a = ap.parse_args()
    blob, info = build(a.kernelcache, a.adt, a.cmdline)
    open(a.out, 'wb').write(blob)
    for k, v in info.items():
        if k != 'segs':
            print(f'{k}: {v}')
    for s in info['segs']:
        print('  ', *s)
