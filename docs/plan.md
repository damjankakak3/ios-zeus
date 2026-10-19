# Plan: iOS 26 on zeus without touching Android

Written 2026-10-19. Facts are marked **[fact]** (checked), guesses **[guess]**.

## Hard rule: internal UFS is read-only for this project

zeus is the user's daily driver. Android stays. Unlike sweet, nothing is ever written to
internal storage: no `fastboot flash/erase/format`, no `dd of=`, no root in `userdata`.

How we make that a guarantee and not a promise:
- The zeus XNU kernel has **no UFS driver at all** (the sweet driver `ufsroot0` is not
  compiled for the ZEUS config). A kernel bug cannot write a disk it has no driver for.
- Everything boots with `fastboot boot` (RAM only, gone after reboot).
- The bootloader must already be **unlocked**. Xiaomi unlocking **wipes the phone**. If it
  is locked, stop: the project cannot start without a factory reset.

## Where the iOS root lives instead

1. **Ramdisk in the boot image** (first bring-up). launchd + a shell + a few daemons, `rd=md0`,
   like early sweet. Size is limited by the ABL download buffer (`max-download-size`).
2. **Root served over USB from the PC** (the real setup). The phone is a USB device (DWC3
   gadget mode, already working on sweet: patches 0072-0077). A small block protocol
   on vendor bulk endpoints: read/write N blocks at LBA. A Python/libusb daemon on the PC
   serves `ios-root.img`. XNU side: a block device (`rd=usbroot0`) like sweet's UFS block
   device, but over USB. HS gives ~30-40 MB/s; SM8450 DWC3 can do SuperSpeed later.
   The phone must stay plugged into the PC while iOS runs.
3. (Later, optional) USB-C SSD in host mode needs xHCI + mass storage written from scratch,
   since Apple's USB stack is closed. Not worth it before 2 works.

## Why zeus can go past iOS 16 (and why sweet could not)

- **[fact]** The real limit on sweet is **PAC**, not "A11". iOS 17+ userland (shared cache,
  every binary) is `arm64e` only, because all iOS 17+ iPhones are A12+. Sweet's Kryo 470 is
  ARMv8.2, which has no pointer authentication, so it can only run arm64 iOS 16 (the iPhone X build).
- **[fact]** SM8450 = Cortex-X2 / A710 / A510, ARMv9.0-A, which includes FEAT_PAuth (v8.3).
  So arm64e code can run. The kernel must be built arm64e and manage the PAC keys
  with standard registers. Apple-only PAC registers (APCTL etc.) in XNU need replacing. **[guess: doable]**
- **[fact]** iOS 27 supports the same devices as iOS 26: iPhone 11 and newer, SE 2 and newer.
- **[fact]** The newest public XNU source is `xnu-12377.121.6` (iOS/macOS 26 family). The
  **iOS 27 kernel source is not published** (checked 2026-10-19). Kernel and userland must
  match (syscalls, Mach traps, IOKit user clients, sandbox), so:
  - **Target: iOS 26.x userland on xnu-12377** (pick the IPSW whose build matches a published tag).
  - iOS 27 later, once Apple publishes its xnu source.

## Device identity: iPhone 11 (`iPhone12,1`, `N104AP`, A13 / T8030)

The same approach as sweet's iPhone X: ADT root compatible plus `/product` from the real
N104AP device tree in the IPSW, with hw.product, hw.target and the home-button type.
Open risks with every A13+ identity:
- **AMX**: A13+ have Apple's undocumented matrix unit. If frameworks issue AMX
  instructions on an "A13" without checking, we get SIGILL. Check how Accelerate/BNNS
  read capabilities (commpage) and report no AMX. **[unknown, research first]**
- Apple-only system registers and instructions used by userland (none known; check).
- No GPU: iOS 26 "Liquid Glass" may need Metal more than iOS 16's CPU CoreAnimation fallback did. **[unknown]**

## SM8450 bring-up differences from SM7150 (all to be verified on hardware)

- Gunyah hypervisor below the kernel. We enter at EL1 with stage-2 restrictions, so never map
  carve-outs cacheable (same XPU lesson as sweet, stricter).
- GICv3 again (addresses from the DT). PSCI via SMC. 8 cores in 3 clusters (1+3+4).
- PMIC arbiter SPMI v7 (sweet: v5). Different panel and touch IC. Read them from the DT.
- Boot image header v4 + `vendor_boot` (DTB lives there). The shim must handle this.

## Order of work

0. Local agent recon (no writes): `docs/local-agent-prompt.md` -> `recon/zeus-recon.md`.
1. DTB from the stock firmware (`vendor_boot.img` of the exact installed build), framebuffer
   address/size, memory map, GIC, PSCI. Fill "Hardware facts (zeus)" in NOTES.md.
2. Shim for zeus: `fastboot boot`, paint the ABL framebuffer, reboot. This proves the boot path.
3. ZEUS XNU config on xnu-12377 (arm64e, no UFS), ramdisk root, console on the framebuffer.
4. USB gadget + USB root daemon.
5. iOS 26 iPhone 11 root image, then the userland bring-up as on sweet.

## Effort, honestly

PAC support is a small part. Everything below the userland is new for SM8450 (shim, memory map,
interrupts, display, touch, buttons, USB) and every sweet XNU patch was written for xnu-8796 and
must be redone for xnu-12377. Expect sweet-sized work (sweet took ~85 kernel patches), not 1-4 patches.
