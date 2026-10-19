# ios-zeus notes

## Handoff (2026-10-19)

- Goal: iOS 26 (iPhone 11 build, N104AP) on zeus, booted with `fastboot boot`, root from ramdisk/USB.
- Phone (from the user): zeus 12/256 GB EEA, **bootloader unlocked**, HyperOS EEA (build: assume
  OS3.0.3.0.VLBEUXM, the newest EEA, unless the user says otherwise).
- The local agent gave nothing useful; the user ran `tools/zeus-dt-extract.py` on their fastboot ROM zip
  instead. **Stock DT read: see `docs/zeus-hw.md`** (DTBs in `dtb/stock/`, not in git).
- Key findings: panel is **command mode** (1440x3200 DSC, splash fb 0xb8000000) -> writing the fb may not
  show without an MDP kick; vtimer is INTID 27 (not 19 as on sweet); touch is ST FTS over SPI.
- **Nothing has been run on the phone.**
- Next: shim stage 1 = `fastboot boot` an image that paints the splash framebuffer, then sends the frame
  (try autorefresh/CTL start if a plain write doesn't show), shows the live DT memory and load address,
  and resets. That checks the boot path and whether the display works without the user flashing anything.

## Facts (checked 2026-10-19)

- iOS 26 and iOS 27 support iPhone 11 and newer, SE 2 and newer (iOS 27 kept iOS 26's list).
- Newest published XNU tag: `xnu-12377.121.6` (26 family). No iOS 27 kernel source.
- iOS 17+ userland is arm64e only. SM8450 cores (Cortex-X2 / A710 / A510) are ARMv9.0 and have FEAT_PAuth.

## Hardware facts (zeus)

From the stock DT, not measured yet: `docs/zeus-hw.md`.

## Lessons from sweet worth reusing (ideas, re-implement cleanly)

- Boot: stock ABL -> `fastboot boot` shim image -> own MMU setup, read live FDT -> XNU Mach-O +
  generated ADT + boot-args. Kernel at a PA above all XPU carve-outs; never map carve-outs cacheable.
- Debug: no UART. Console drawn into the ABL framebuffer; QEMU `virt` twin of the ADT for smoke tests.
  On zeus the log goes out over USB (sweet patch 0077 idea), not to a disk ring.
- Interrupts: secure Group 0 is not reachable from NS EL1. Timer as Group 1 IRQ; PSCI via SMC.
- SMP via PSCI CPU_ON. IOKit built-in stand-ins (GIC, PMGR, platform expert) instead of closed kexts.
- Identity: ADT root `compatible` + `/product` from the real device's ADT (from the IPSW), hw.product,
  hw.target. MobileGestalt caches answers on disk.
- Activation: ADT /chosen production/security-mode values select mobileactivationd's local path.
- HID: Apple's open-source IOHIDFamily user client compiled over a small IOHIDEventService.
- Display: IOSurface + IOMobileFramebuffer (AppleCLCD) services, CPU rendering, present a real iPhone size.
- Root: launchd.plist edits, Setup skipped by preferences, crash-looping daemons disabled.

## Open questions

- PAC: replace Apple-only PAC system registers in arm64e XNU with standard FEAT_PAuth key handling.
- AMX: A13+ userland may issue Apple matrix (AMX) instructions -> SIGILL on Cortex. How do
  Accelerate/BNNS detect AMX (commpage bits?) - must report none.
- GPU: does iOS 26 CoreAnimation/"Liquid Glass" still have a CPU path?
- Gunyah hypervisor under EL1 on SM8450: memory we may touch, PSCI behaviour.
