#!/usr/bin/env python3
"""End-to-end check of the xnu loader (shim/loader.c) in QEMU, no phone needed.

  python3 tools/qemu-loader-test.py        (needs qemu-system-aarch64, dtc, fdtput, clang, ld.lld)

Builds probe 5 with the display registers moved into RAM, a fake MH_FILESET
"kernel collection" whose entry point stores x0 (the boot_args pointer) and a
magic word, packs it with the real mkadt.py + mkpayload.py, puts it in the
ramdisk at an odd offset behind fake vendor ramdisk bytes, boots QEMU virt
like ABL would (Image + DTB with linux,initrd-*), and then checks in guest
memory: boot_args fields, the copied segments, the zero-filled tail and the ADT.
"""
import os, shutil, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import mkpayload  # noqa: E402

PHYS_BASE, MEM_SIZE = 0x100000000, 0x40000000   # inside QEMU's 4G at 0x40000000
RD_ADDR, RD_PREFIX = 0x70000000, 12345          # odd: payload starts unaligned
MAILBOX = 0x60100000
MAGIC = 0x4b4f554e58534555                      # "UESXNUOK"
KC_VA = 0xfffffe0007004000

FAKE_KERNEL = r"""
	.text
	.globl _start
_start:
	movz	x1, #0x6010, lsl #16
	str	x0, [x1]
	adr	x2, magic
	ldr	x2, [x2]
	str	x2, [x1, #8]
	dsb	sy
1:	wfe
	b	1b
	.balign 8
magic:	.quad	0x4b4f554e58534555
"""


def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def fake_kc(tmp):
    s = os.path.join(tmp, 'k.s')
    open(s, 'w').write(FAKE_KERNEL)
    run('clang', '--target=aarch64-none-elf', '-c', s, '-o', os.path.join(tmp, 'k.o'))
    run('llvm-objcopy', '-O', 'binary', '-j', '.text', os.path.join(tmp, 'k.o'), os.path.join(tmp, 'k.bin'))
    code = open(os.path.join(tmp, 'k.bin'), 'rb').read()

    def seg(name, vmaddr, vmsize, fileoff, filesize):
        return struct.pack('<II16sQQQQIIII', 0x19, 72, name.encode(), vmaddr, vmsize, fileoff, filesize, 5, 5, 0, 0)
    eid = b'com.apple.kernel\0'
    fse = struct.pack('<IIQQII', 0x80000035, 32 + len(eid) + (-(32 + len(eid)) % 8), KC_VA + 0x1000, 0x1000, 32, 0)
    fse += eid + b'\0' * (-len(fse + eid) % 8)
    cmds = [seg('__TEXT', KC_VA, 0x4000, 0, 0x4000),
            seg('__TEXT_EXEC', KC_VA + 0x4000, 0x4000, 0x4000, 0x4000),
            seg('__DATA', KC_VA + 0x8000, 0x8000, 0x8000, 0x10),
            fse]
    hdr = struct.pack('<8I', 0xfeedfacf, 0x0100000c, 2, 0xc, len(cmds), sum(map(len, cmds)), 0, 0)
    kc = bytearray(0x8010)
    kc[0:len(hdr)] = hdr
    kc[len(hdr):len(hdr) + sum(map(len, cmds))] = b''.join(cmds)
    # inner kernel mach header at 0x1000 with LC_UNIXTHREAD (ARM_THREAD_STATE64)
    state = struct.pack('<33Q', *([0] * 32 + [KC_VA + 0x4000])) + struct.pack('<II', 0, 0)
    th = struct.pack('<IIII', 0x5, 16 + len(state), 6, len(state) // 4) + state
    ih = struct.pack('<8I', 0xfeedfacf, 0x0100000c, 2, 2, 1, len(th), 0, 0)
    kc[0x1000:0x1000 + len(ih) + len(th)] = ih + th
    kc[0x4000:0x4000 + len(code)] = code
    kc[0x8000:0x8010] = bytes(range(16))
    path = os.path.join(tmp, 'fake.kc')
    open(path, 'wb').write(kc)
    return path, code


def monitor(sock, cmd):
    sock.sendall(cmd.encode() + b'\n')
    time.sleep(0.3)
    data = b''
    sock.settimeout(0.5)
    try:
        while True:
            d = sock.recv(65536)
            if not d:
                break
            data += d
    except socket.timeout:
        pass
    return data.decode(errors='replace')


def main():
    for t in ('qemu-system-aarch64', 'dtc', 'fdtput', 'clang', 'ld.lld'):
        if not shutil.which(t):
            sys.exit(f'missing {t}')
    tmp = tempfile.mkdtemp(prefix='zeus-qemu-')
    out = os.path.join(tmp, 'shim')
    env = dict(os.environ, OUT=out, EXTRA_CFLAGS='-DMDP_BASE=0x60000000UL')
    run('bash', os.path.join(ROOT, 'shim', 'build.sh'), env=env)

    kc, code = fake_kc(tmp)
    adt_path = os.path.join(tmp, 'zeus.adt')
    run(sys.executable, os.path.join(ROOT, 'tools', 'mkadt.py'), adt_path,
        '--phys-base', hex(PHYS_BASE), '--mem-size', hex(MEM_SIZE))
    payload, info = mkpayload.build(kc, adt_path, 'debug=0x14e cpus=1 qemu-test')
    rd = os.urandom(RD_PREFIX) + payload
    open(os.path.join(tmp, 'rd.bin'), 'wb').write(rd)
    open(os.path.join(tmp, 'junk.bin'), 'wb').write(b'\xff' * 0x8000)
    to_pa = PHYS_BASE - int(info['virt_base'], 16)
    data_pa = KC_VA + 0x8000 + to_pa

    dtb = os.path.join(tmp, 'virt.dtb')
    run('qemu-system-aarch64', '-M', f'virt,dumpdtb={dtb}', '-cpu', 'max', '-m', '4G', '-nographic')
    run('fdtput', '-p', '-c', dtb, '/reserved-memory/splash_region')
    run('fdtput', '-t', 'x', dtb, '/reserved-memory', '#address-cells', '2')
    run('fdtput', '-t', 'x', dtb, '/reserved-memory', '#size-cells', '2')
    run('fdtput', '-t', 'x', dtb, '/reserved-memory/splash_region', 'reg', '0', '0x50000000', '0', '0x2b00000')
    end = RD_ADDR + len(rd)
    run('fdtput', '-t', 'x', dtb, '/chosen', 'linux,initrd-start', '0', hex(RD_ADDR))
    run('fdtput', '-t', 'x', dtb, '/chosen', 'linux,initrd-end', '0', hex(end))

    with socket.socket() as probe_port:
        probe_port.bind(('127.0.0.1', 0))
        port = probe_port.getsockname()[1]
    q = subprocess.Popen(['qemu-system-aarch64', '-M', 'virt', '-cpu', 'max', '-m', '4G', '-nographic',
                          '-no-reboot', '-serial', 'null', '-kernel', os.path.join(out, 'Image'),
                          '-dtb', dtb,
                          '-device', f'loader,file={tmp}/rd.bin,addr={RD_ADDR:#x},force-raw=on',
                          '-device', f'loader,file={tmp}/junk.bin,addr={data_pa:#x},force-raw=on',
                          '-monitor', f'tcp:127.0.0.1:{port},server,nowait'],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                s = socket.create_connection(('127.0.0.1', port))
                break
            except OSError:
                time.sleep(0.1)
        monitor(s, '')
        t0, ba_pa = time.time(), None
        while time.time() - t0 < 60:
            r = monitor(s, f'xp /2gx {MAILBOX:#x}')
            vals = [int(w, 16) for w in r.split() if w.startswith('0x') and len(w) == 18]
            if len(vals) >= 2 and vals[1] == MAGIC:
                ba_pa = vals[0]
                break
            time.sleep(1)
        if ba_pa is None:
            sys.exit('FAIL: fake kernel never ran (loader stopped or crashed)')
        print(f'fake kernel entered after {time.time() - t0:.0f} s, x0 = {ba_pa:#x}')
        dump = os.path.join(tmp, 'mem.bin')
        top = ba_pa + 0x4000
        lo = KC_VA + to_pa
        r = monitor(s, f'pmemsave {lo:#x} {top - lo:#x} "{dump}"')
        for _ in range(50):
            if os.path.exists(dump) and os.path.getsize(dump) == top - lo:
                break
            time.sleep(0.2)
        else:
            sys.exit(f'FAIL: pmemsave: {r!r}')
        mem = open(dump, 'rb').read()
    finally:
        q.kill()

    def at(pa, n):
        return mem[pa - lo:pa - lo + n]
    errs = []
    rev, ver = struct.unpack('<HH', at(ba_pa, 4))
    virt_base, phys_base, mem_size, top_kd = struct.unpack('<4Q', at(ba_pa + 8, 32))
    dt_va, = struct.unpack('<Q', at(ba_pa + 96, 8))
    dt_len, = struct.unpack('<I', at(ba_pa + 104, 4))
    cmdline = at(ba_pa + 108, 608).split(b'\0')[0].decode()
    checks = [('revision', rev, 2), ('version', ver, 2), ('virtBase', virt_base, int(info['virt_base'], 16)),
              ('physBase', phys_base, PHYS_BASE), ('memSize', mem_size, MEM_SIZE), ('top', top_kd, top),
              ('cmdline', cmdline, 'debug=0x14e cpus=1 qemu-test'),
              ('__TEXT_EXEC', at(KC_VA + 0x4000 + to_pa, len(code)), code),
              ('__DATA file part', at(data_pa, 16), bytes(range(16))),
              ('__DATA zero fill', at(data_pa + 16, 0x8000 - 16), b'\0' * (0x8000 - 16)),
              ('ADT', at(dt_va + to_pa, dt_len), open(adt_path, 'rb').read())]
    for name, got, want in checks:
        ok = got == want
        print(f'{"ok  " if ok else "FAIL"} {name}' + ('' if ok else f': {str(got)[:60]} != {str(want)[:60]}'))
        if not ok:
            errs.append(name)
    shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(1 if errs else 0)


if __name__ == '__main__':
    main()
