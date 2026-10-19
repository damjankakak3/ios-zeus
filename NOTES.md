# ios-zeus notes

## Handoff (2026-10-19)

- Goal: iOS 26 (iPhone 11 build, N104AP) on zeus, booted with `fastboot boot`, root from ramdisk/USB.
- State: clean repo skeleton only. **Nothing has been run on the phone.**
- Next: user runs `docs/local-agent-prompt.md` with the local agent -> `recon/zeus-recon.md`
  (device, bootloader unlocked?, slot, `max-download-size`, build). Then get the DTB from
  `vendor_boot.img` of the matching stock firmware.

## Facts (checked 2026-10-19)

- iOS 26 and iOS 27 support iPhone 11 and newer, SE 2 and newer (iOS 27 kept iOS 26's list).
- Newest published XNU tag: `xnu-12377.121.6` (26 family). No iOS 27 kernel source.
- iOS 17+ userland is arm64e only. SM8450 cores (Cortex-X2 / A710 / A510) are ARMv9.0 and have FEAT_PAuth.

## Hardware facts (zeus)

(empty - fill from recon and the DTB; mark each one measured or from DT)

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
