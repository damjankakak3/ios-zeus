#!/usr/bin/env python3
"""Extract partitions from an Android full OTA zip (payload.bin) over HTTP.

Only the byte ranges of the wanted partitions are downloaded (curl range
requests), so pulling vendor_boot/dtbo out of a 6 GB OTA costs a few MB.

  otadump.py URL_OR_FILE OUTDIR [partition ...]     (no partitions = list)
"""
import bz2, hashlib, lzma, os, struct, subprocess, sys


class Src:
    def __init__(self, where):
        self.where = where
        self.local = os.path.exists(where)
        if self.local:
            self.size = os.path.getsize(where)
        else:
            hdr = subprocess.run(["curl", "-sSIL", where], check=True,
                                 capture_output=True, text=True).stdout
            lens = [l.split(":", 1)[1] for l in hdr.splitlines()
                    if l.lower().startswith("content-length:")]
            self.size = int(lens[-1])

    def read(self, off, n):
        if n == 0:
            return b""
        if self.local:
            with open(self.where, "rb") as f:
                f.seek(off)
                return f.read(n)
        for _ in range(4):
            r = subprocess.run(["curl", "-sSfL", "-r", f"{off}-{off + n - 1}", self.where],
                               capture_output=True)
            if r.returncode == 0 and len(r.stdout) == n:
                return r.stdout
        raise IOError(f"range {off}+{n} failed")


def zip_member(src, name):
    """Return (data offset, size) of a stored zip member."""
    tail = src.read(src.size - 65558, 65558) if src.size > 65558 else src.read(0, src.size)
    e = tail.rfind(b"PK\x05\x06")
    cd_size, cd_off = struct.unpack_from("<II", tail, e + 12)
    if cd_off == 0xFFFFFFFF:  # zip64
        l = tail.rfind(b"PK\x06\x07")
        (z64,) = struct.unpack_from("<Q", tail, l + 8)
        rec = src.read(z64, 56)
        cd_size, cd_off = struct.unpack_from("<QQ", rec, 40)
    cd = src.read(cd_off, cd_size)
    p = 0
    while p < len(cd):
        meth, = struct.unpack_from("<H", cd, p + 10)
        csize, usize = struct.unpack_from("<II", cd, p + 20)
        nl, xl, cl = struct.unpack_from("<HHH", cd, p + 28)
        (loff,) = struct.unpack_from("<I", cd, p + 42)
        fname = cd[p + 46:p + 46 + nl].decode()
        extra = cd[p + 46 + nl:p + 46 + nl + xl]
        q = 0
        while q < len(extra):  # zip64 extra field
            tag, ln = struct.unpack_from("<HH", extra, q)
            if tag == 1:
                vals = list(struct.unpack_from("<%dQ" % (ln // 8), extra, q + 4))
                if usize == 0xFFFFFFFF: usize = vals.pop(0)
                if csize == 0xFFFFFFFF: csize = vals.pop(0)
                if loff == 0xFFFFFFFF: loff = vals.pop(0)
            q += 4 + ln
        if fname == name:
            if meth != 0:
                raise ValueError(f"{name} is compressed (method {meth})")
            lh = src.read(loff, 30)
            lnl, lxl = struct.unpack_from("<HH", lh, 26)
            return loff + 30 + lnl + lxl, csize
        p += 46 + nl + xl + cl
    raise KeyError(name)


def pb(buf):
    """Minimal protobuf decoder: yields (field, value)."""
    p = 0
    while p < len(buf):
        key, p = varint(buf, p)
        f, wt = key >> 3, key & 7
        if wt == 0:
            v, p = varint(buf, p)
        elif wt == 1:
            v = struct.unpack_from("<Q", buf, p)[0]; p += 8
        elif wt == 2:
            n, p = varint(buf, p); v = buf[p:p + n]; p += n
        elif wt == 5:
            v = struct.unpack_from("<I", buf, p)[0]; p += 4
        else:
            raise ValueError(f"wire type {wt}")
        yield f, v


def varint(buf, p):
    v = s = 0
    while True:
        b = buf[p]; p += 1
        v |= (b & 0x7F) << s; s += 7
        if not b & 0x80:
            return v, p


REPLACE, REPLACE_BZ, ZERO, DISCARD, REPLACE_XZ, ZSTD = 0, 1, 6, 7, 8, 14


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, outdir, want = Src(sys.argv[1]), sys.argv[2], set(sys.argv[3:])
    base, _ = zip_member(src, "payload.bin") if not sys.argv[1].endswith(".bin") else (0, src.size)
    hdr = src.read(base, 24)
    magic, ver, msize = struct.unpack(">4sQQ", hdr[:20])
    assert magic == b"CrAU" and ver == 2, (magic, ver)
    (sigsize,) = struct.unpack(">I", hdr[20:24])
    manifest = src.read(base + 24, msize)
    blobs = base + 24 + msize + sigsize
    block = 4096
    parts = []
    for f, v in pb(manifest):
        if f == 3:
            block = v
        elif f == 13:
            parts.append(v)
    os.makedirs(outdir, exist_ok=True)
    for raw in parts:
        name, size, ops, sha = None, 0, [], None
        for f, v in pb(raw):
            if f == 1: name = v.decode()
            elif f == 7:
                for g, w in pb(v):
                    if g == 1: size = w
                    elif g == 2: sha = w
            elif f == 8: ops.append(v)
        if not want:
            print(f"{name:24} {size:>12}")
            continue
        if name not in want:
            continue
        out = os.path.join(outdir, name + ".img")
        with open(out, "wb") as o:
            o.truncate(size)
            for op in ops:
                t, doff, dlen, ext = REPLACE, 0, 0, []
                for f, v in pb(op):
                    if f == 1: t = v
                    elif f == 2: doff = v
                    elif f == 3: dlen = v
                    elif f == 6: ext.append(dict(pb(v)))
                data = src.read(blobs + doff, dlen) if t not in (ZERO, DISCARD) else b""
                if t == REPLACE_BZ: data = bz2.decompress(data)
                elif t == REPLACE_XZ: data = lzma.decompress(data)
                elif t == ZSTD:
                    import zstandard
                    data = zstandard.ZstdDecompressor().decompress(data, max_output_size=1 << 31)
                elif t not in (REPLACE, ZERO, DISCARD):
                    raise ValueError(f"{name}: op type {t} (not a full OTA?)")
                for e in ext:
                    start, n = e.get(1, 0), e.get(2, 0)
                    o.seek(start * block)
                    o.write(data[:n * block] if data else b"\0" * (n * block))
                    data = data[n * block:]
        with open(out, "rb") as i:
            ok = sha is None or hashlib.sha256(i.read()).digest() == sha
        print(f"{name}: {size} bytes, sha256 {'OK' if ok else 'MISMATCH'} -> {out}")


if __name__ == "__main__":
    main()
