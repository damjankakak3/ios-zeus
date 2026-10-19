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

## Lessons from sweet that still apply (re-do for xnu-12377)

Timer on Group 1 IRQ instead of FIQ; PSCI via SMC instead of HVC; no XPU carve-out mapped cacheable; built-in
IOKit stand-ins (interrupt controller, PMGR, platform expert) instead of closed kexts; no KDK libraries linked.
