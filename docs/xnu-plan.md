# Plan: XNU (iOS 26 generation) on zeus

Source: `xnu-12377.121.6` = macOS 26.5 (`distribution-macOS` release.json, checked 2026-10-19). Built on
GitHub Actions by `.github/workflows/xnu.yml` with blacktop/darwin-xnu-build (pinned commit), our patches
from `xnu/patches/` first. Local reading copy: `research/xnu` (not in git).

## Template: VMAPPLE (facts from the source)

`pexpert/pexpert/arm64/VMAPPLE.h`:
- `NO_MONITOR` -> no PPL, no SPTM. (SPTM is chosen per SoC from Apple's device map database; VMAPPLE and
  T8101/T8103 are not in `SPTM_ENABLED_SOCS`.) A closed monitor is the one thing we could not replace.
- `HAS_GIC_V3`, 16K pages (`__ARM_16K_PG__`), `ARM_PARAMETERIZED_PMAP`, PL011 UART.
- `CPU_HAS_APPLE_PAC` + `HAS_PARAVIRTUALIZED_PAC`: PAC key handling goes through hypercalls
  (`osfmk/arm64/hv_hvc.h`: SET_INITIAL_STATE, GET_DEFAULT_KEYS, SET_A_KEYS, SET_B_KEYS, SET_EL0_DIVERSIFIER,
  SET_G_KEY), used in `pac_asm.h`, `cswitch.s`, `machine_routines_asm.s`, `machine_routines_apple.c`.
- `HAS_ARM_FEAT_SME/SME2` and `-march=armv8.5a+sme2` (makedefs/MakeInc.def): **zeus has no SME**. The ZEUS config
  needs its own flags (`-march=armv8.5-a` or `armv9-a` without SME, no SME context switching).
- `GICR_PE_SIZE 0x20000`; **zeus GICR stride is 0x40000** (GICv4 frames, DT `redistributor-stride`).

## PAC plan (the iOS 17+ blocker that zeus's CPU removes)

iOS 26 userland is arm64e only, so the kernel must be arm64e too: it signs and checks user pointers in
thread state, signal frames and exception handling.

- Cortex-X2/A710/A510 (ARMv9.0) implement FEAT_PAuth with the standard key registers (APIAKey, APIBKey,
  APDAKey, APDBKey, APGAKey `_EL1`).
- Apple CPUs keep kernel keys and the EL0 "diversifier" separate in hardware; standard ARM has one set of keys
  for EL1 and EL0.
- **Plan: one fixed key set for kernel and all user processes, written once per CPU at boot.** The
  paravirtual PAC hooks become no-ops (or direct `msr` writes of the same constants). Every pointer is signed
  and checked consistently, but PAC gives no isolation. That is fine for this project.
- Unknown: whether Gunyah lets EL1 write the key registers (HCR_EL2.APK/API). Linux on this phone runs with
  pointer authentication, so probably yes. **Probe 4 will test it**: read ID_AA64ISAR1_EL1, write APIAKey,
  do PACIA + AUTIA.

## Boot path (to build)

1. Loader (from the probe): Mach-O kernel + Apple device tree (from the iPhone 11 / N104AP device tree in the
   IPSW, filled in like sweet's `tools/mkadt.py`) + boot_args, carried in the boot.img ramdisk, MMU off, jump.
2. ZEUS machine config: VMAPPLE minus hypercalls (PAC, timebase offset, WFK/kick), GICv3 at 0x17100000 with
   0x40000 GICR stride, Group 1 IRQs, arch timer (vtimer INTID 27), PSCI via SMC, no SME, console over the
   probe's USB serial (CDC-ACM) code.
3. First goal: kernel printf reaches `logs/` over USB. Then interrupts, timer, 8 CPUs.
4. Root over USB (block protocol on the same DWC3; PC serves the image). No UFS driver at all.

## Loader design (next step after 0002 builds)

XNU's entry contract (pexpert/pexpert/arm64/boot.h, osfmk/arm64/start.s): MMU off, x0 = physical address of a
`boot_args` (Revision 2, Version 2): virtBase, physBase, memSize, topOfKernelData, Video (base, rowBytes, width,
height, depth), machineType, deviceTreeP + deviceTreeLength (Apple device tree, ADT), CommandLine[608], bootFlags,
memSizeActual. **XNU gets one contiguous RAM range: [physBase, physBase + memSize).**

- **RAM for XNU: bank 0x900000000 + 0x180000000 (6 GB).** Fact: the live DT of probe 1 lists that bank, and every
  static reserved region in the stock DT is below 4 GB (docs/zeus-hw.md). Guess: ABL/Gunyah reserve nothing
  there. Check: dump the live DT /reserved-memory in the loader's log before jumping. Android's memory is not
  touched by this (it is RAM only and the phone resets afterwards).
- **boot.img:** kernel field = the loader (probe code base: USB log, display, DC CIVAC), ramdisk = a small
  container {magic, kernel Mach-O, ADT, command line}. The loader finds the ramdisk via the live DT
  /chosen linux,initrd-start/-end.
- **Mach-O load:** kernel.development.zeus is a plain MH_EXECUTE (not a fileset kernelcache). virtBase = lowest
  LC_SEGMENT_64 vmaddr (page aligned); copy each segment to physBase + (vmaddr - virtBase), zero-fill to vmsize;
  entry = LC_UNIXTHREAD pc translated the same way. Then ADT, then boot_args, each 16 KB aligned;
  topOfKernelData = end of boot_args.
- **Caches:** clean every written line to PoC (DC CIVAC), then IC IALLU, DSB, ISB, MMU off, jump with x0.
- **ADT:** built on the PC from the iPhone 11 (N104AP) device tree in the iOS 26 IPSW (Apple artifact: fetched
  by the tool, never committed), with /chosen, /memory-map, 8 /cpus (MPIDR from the live DT), /arm-io with the GIC
  (0x17100000, GICR stride 0x40000), arch timer (vtimer INTID 27), and our own nodes. Tool: `tools/mkadt.py`
  (re-written, idea from sweet).
- **Video:** the ABL splash framebuffer 0xb8000000, 1440x3200, rowBytes 5760, 32 bpp; it lies outside
  [physBase, +memSize), so XNU must map it as device/WC memory. Open: check arm_vm_init for how Video.v_baseAddr
  is mapped when it is not inside physBase's range.
- **Console (patch 0003):** VMAPPLE's console is a PL011; zeus has none we can use. Port the probe's DWC3
  CDC-ACM code into pexpert as a serial driver so kernel printf lands in `logs/` like the probe log.
- Stop conditions for the first boot: panic/exception screen on the framebuffer (the loader keeps the probe's
  exception handler until XNU installs VBAR) and PSCI SYSTEM_RESET after a timeout, so the phone always returns
  to Android.

## Lessons from sweet that still apply (re-do for xnu-12377)

Timer on Group 1 IRQ instead of FIQ; PSCI via SMC instead of HVC; no XPU carve-out mapped cacheable; built-in
IOKit stand-ins (interrupt controller, PMGR, platform expert) instead of closed kexts; no KDK libraries linked.
