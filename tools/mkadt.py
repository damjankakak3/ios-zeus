#!/usr/bin/env python3
"""Build the Apple Device Tree (ADT) the ZEUS xnu boots with.

  mkadt.py OUT.adt [--d83ap research/ipsw/d83ap.adt] [--dump]

Port of ios-sweet tools/mkadt.py: same idea (only what xnu's early boot and
userland identity read), zeus hardware from docs/zeus-hw.md, and the identity
of an iPhone 15 Pro (iPhone16,1 / D83AP). /product is copied from the real
D83AP device tree when it is available. That tree is an Apple artifact and
stays out of git; get it with tools/fetch-d83ap-adt.sh.
"""
import argparse, hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D83AP_ADT = os.path.join(ROOT, 'research', 'ipsw', 'd83ap.adt')

# Kernel-managed RAM: the 6 GB bank above 4 GB (live DT of probe 1). Every
# static reserved region in the stock DT is below 4 GB; the loader checks the
# live /reserved-memory before it jumps.
PHYS_BASE = 0x900000000
MEM_SIZE = 0x180000000

# /arm-io children are relative to this; it must not be 0 (serial_init bails).
SOC_BASE = 0x01000000
GICD, GICD_SIZE = 0x17100000, 0x10000
GICR, GICR_SIZE = 0x17180000, 0x200000     # 8 x 256K GICv4 frames
VTIMER_INTID = 27
TIMEBASE = 19200000

FB, FB_SIZE = 0xb8000000, 0x2b00000         # ABL splash_region
FB_W, FB_H, FB_STRIDE = 1440, 3200, 1440 * 4

# cpu nodes: MPIDR Aff1 = core number (stock DT reg). The boot CPU is the one
# with state "running"; ABL runs us on core 0 (the loader stops if not).
CORES = [(0x000, 'arm,cortex-a510', 1785600000), (0x100, 'arm,cortex-a510', 1785600000),
         (0x200, 'arm,cortex-a510', 1785600000), (0x300, 'arm,cortex-a510', 1785600000),
         (0x400, 'arm,cortex-a710', 2496000000), (0x500, 'arm,cortex-a710', 2496000000),
         (0x600, 'arm,cortex-a710', 2496000000), (0x700, 'arm,cortex-x2', 2995200000)]

ZEUS_ECID = 0x5A45555300000001              # 'ZEUS' + 1, synthetic (not the phone's)
BOOT_MANIFEST_HASH = hashlib.sha384(b'zeus boot manifest').digest()

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def d83ap_product(path):
    """/product of the real D83AP tree, minus iBoot placeholders and bookkeeping."""
    if not path or not os.path.exists(path):
        print(f'warning: {path} missing: no /product node (identity props only)', file=sys.stderr)
        return None
    root, _ = adt.parse(adt.load(path))
    for c in root.children:
        if c.name == 'product':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')


def build(d83ap=D83AP_ADT, seed=None, phys_base=PHYS_BASE, mem_size=MEM_SIZE):
    seed = seed if seed is not None else os.urandom(64)
    phandles = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandles)))

    # The screen console. /defaults serial-device points at it (pe_serial.c
    # "zeus,fbcon" driver, xnu patch 0003).
    fbcon_ph = next(phandles)
    fbcon = node('zeus-fbcon', [
        ('AAPL,phandle', u32(fbcon_ph)),
        ('compatible', cstr('zeus,fbcon')),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('ios-zeus-loader')),
        ('system-firmware-version', cstr('ios-zeus-loader')),
        # Notes from sweet (userland, iOS 16; recheck for iOS 26):
        # /sbin/mount -P 1 needs boot-manifest-hash; libimg4 needs crypto-hash-method;
        # display-scale feeds MobileGestalt's screen class; unique-chip-id + chip-id
        # give IOPlatformUUID; the production/security tuple selects mobileactivationd's
        # "Hactivation" path (no SEP, no baseband).
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        ('display-scale', u32(3)),
        ('unique-chip-id', u64(ZEUS_ECID)),
        ('chip-id', u32(0x8130)),       # A17 Pro (T8130), what the D83AP userland expects
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # local boot policy (kern_csr.c): SIP relaxed, unsealed root allowed
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    defaults = node('defaults', [
        ph(),
        ('serial-device', u32(fbcon_ph)),
        ('no-effaceable-storage', u32(1)),
    ])

    # GIC: interrupt controller for the CPU nubs' IPI specifiers (xnu patch 0005
    # ZeusGIC registers under IODTInterruptControllerName = its phandle).
    gic_ph = next(phandles)

    cpu_nodes = []
    for i, (mpidr, compat, hz) in enumerate(CORES):
        cpu_nodes.append(node(f'cpu{i}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr(compat) + cstr('ARM,v9')),
            ('reg', u32(mpidr)),
            ('cpu-id', u32(i)),
            ('state', cstr('running' if i == 0 else 'stopped')),
            ('timebase-frequency', u32(TIMEBASE)),
            ('clock-frequency', u32(hz)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(3200000000)),
            ('peripheral-frequency', u32(TIMEBASE)),
            ('fixed-frequency', u32(TIMEBASE)),
            ('l2-cache-id', u32(i)),
            ('l3-cache-id', u32(0)),
            # AppleARMSMP: three interrupts = legacy IPI layout (sources 0 and 2 are IPIs)
            ('interrupt-parent', u32(gic_ph)),
            ('interrupts', u32(1, 2, 3)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ('AAPL,phandle', u32(gic_ph)),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', cstr('GICv3')),
        ('#interrupt-cells', u32(1)),
        ('virtual-timer-intid', u32(VTIMER_INTID)),
        ('reg', u64(GICD - SOC_BASE, GICD_SIZE, GICR - SOC_BASE, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('zeus-io')),
        ('compatible', cstr('zeus-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        node('wdt', [ph(), ('no-pmu', u32(1))]),     # watchdogd: no PMU boot stage
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('zeus,display')),
            ('canvas-width', u32(FB_W)),
            ('canvas-height', u32(FB_H)),
        ])])

    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),               # no Home button (like D83AP)
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d83ap_product(d83ap)
    product = [node('product', [ph()] + product_props)] if product_props else []

    # compatible[0] = hw.model / hw.target (D83AP), model = hw.machine (iPhone16,1).
    # 'xiaomi,zeus' stays for our platform expert to match.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D83AP') + cstr('iPhone16,1') + cstr('xiaomi,zeus') + cstr('qcom,sm8450') + cstr('AppleARM')),
        ('model', cstr('iPhone16,1')),
        ('target-type', cstr('D83')),
        ('target-sub-type', cstr('D83AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons] + product)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--d83ap', default=D83AP_ADT, help='raw or IM4P D83AP DeviceTree (for /product)')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--phys-base', type=lambda x: int(x, 0), default=PHYS_BASE, help='tests only')
    ap.add_argument('--mem-size', type=lambda x: int(x, 0), default=MEM_SIZE, help='tests only')
    a = ap.parse_args()
    blob = adt.serialize(build(a.d83ap, phys_base=a.phys_base, mem_size=a.mem_size))
    open(a.out, 'wb').write(blob)
    if a.dump:
        adt.dump(adt.parse(blob)[0])
    print(f'{a.out}: {len(blob)} bytes', file=sys.stderr)
