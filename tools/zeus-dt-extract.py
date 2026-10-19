#!/usr/bin/env python3
"""Pull the device trees out of a Xiaomi fastboot ROM, without touching the phone.

  python3 zeus-dt-extract.py ROM.zip|ROM.tgz [OUT.tar.gz]

Reads images/vendor_boot.img and images/dtbo.img from the package, splits the
base DTBs (vendor_boot) and overlays (dtbo) into single .dtb files, records the
vendor_boot header (cmdline, bootconfig), and packs everything into
zeus-dt.tar.gz (a few MB). Stdlib only; dtc is not needed. Public firmware
holds no serial numbers, so the result is safe to share.
"""
import io, os, struct, sys, tarfile, zipfile


def member(pkg, name):
    """Bytes of images/<name> from a .zip or .tgz fastboot package."""
    if zipfile.is_zipfile(pkg):
        with zipfile.ZipFile(pkg) as z:
            for n in z.namelist():
                if n.endswith("images/" + name):
                    return z.read(n)
    else:
        with tarfile.open(pkg) as t:  # streams through the archive
            for m in t:
                if m.name.endswith("images/" + name):
                    return t.extractfile(m).read()
    sys.exit(f"images/{name} not found in {pkg}")


def split_fdts(blob):
    out, p = [], 0
    while p + 8 <= len(blob) and blob[p:p + 4] == b"\xd0\x0d\xfe\xed":
        (n,) = struct.unpack_from(">I", blob, p + 4)
        out.append(blob[p:p + n])
        p += n
    return out


def vendor_boot(d, info):
    assert d[:8] == b"VNDRBOOT", "not a vendor_boot image"
    ver, page, kaddr, raddr, rsize = struct.unpack_from("<5I", d, 8)
    hsize, dtbsize = struct.unpack_from("<II", d, 2096)
    (dtbaddr,) = struct.unpack_from("<Q", d, 2104)
    al = lambda x: (x + page - 1) // page * page
    info += [f"vendor_boot header v{ver} page {page}",
             f"kernel_addr {kaddr:#x} ramdisk_addr {raddr:#x} dtb_addr {dtbaddr:#x}",
             f"tags_addr {struct.unpack_from('<I', d, 2076)[0]:#x}",
             "cmdline: " + d[28:28 + 2048].split(b"\0")[0].decode(errors="replace"),
             "name: " + d[2080:2096].split(b"\0")[0].decode(errors="replace")]
    doff = al(hsize) + al(rsize)
    if ver >= 4:
        tsz, _, _, bcs = struct.unpack_from("<4I", d, 2112)
        bc = d[doff + al(dtbsize) + al(tsz):][:bcs]
        info.append("bootconfig:\n" + bc.rstrip(b"\0").decode(errors="replace"))
    return split_fdts(d[doff:doff + dtbsize])


def dtbo(d, info):
    magic, total, hsz, esz, cnt, eoff, page, ver = struct.unpack_from(">8I", d, 0)
    assert magic == 0xD7B7AB1E, "not a dtbo image"
    info.append(f"dtbo: {cnt} overlays")
    out = []
    for i in range(cnt):
        size, off, id_, rev, *custom = struct.unpack_from(">8I", d, eoff + i * esz)
        info.append(f"  dtbo{i}: id {id_:#x} rev {rev:#x} custom {[hex(c) for c in custom]}")
        out.append(d[off:off + size])
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    pkg = sys.argv[1]
    dest = sys.argv[2] if len(sys.argv) > 2 else "zeus-dt.tar.gz"
    info = [f"package: {os.path.basename(pkg)}"]
    files = {}
    base = vendor_boot(member(pkg, "vendor_boot.img"), info)
    if not base:
        sys.exit("no base DTB in vendor_boot.img (damaged download?)")
    for i, b in enumerate(base):
        files[f"zeus-dt/vendor_boot-dtb{i}.dtb"] = b
    for i, b in enumerate(dtbo(member(pkg, "dtbo.img"), info)):
        files[f"zeus-dt/dtbo{i}.dtb"] = b
    info.append(f"{len(files)} device tree blobs")
    files["zeus-dt/info.txt"] = ("\n".join(info) + "\n").encode()
    with tarfile.open(dest, "w:gz") as t:
        for name, data in files.items():
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))
    print("\n".join(info))
    print(f"\nwrote {dest} ({os.path.getsize(dest) // 1024} KiB) - upload this file")


if __name__ == "__main__":
    main()
