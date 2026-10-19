# ios-zeus notes

## Handoff (2026-10-19)

- Goal: iOS 26 (iPhone 11 build, N104AP) on zeus, booted with `fastboot boot`, root from ramdisk/USB.
- Phone (from the user): zeus 12/256 GB EEA, **bootloader unlocked**, HyperOS EEA (build: assume
  OS3.0.3.0.VLBEUXM, the newest EEA, unless the user says otherwise).
- The local agent gave nothing useful; the user ran `tools/zeus-dt-extract.py` on their fastboot ROM zip
  instead. **Stock DT read: see `docs/zeus-hw.md`** (DTBs in `dtb/stock/`, not in git).
- Key findings: panel is **command mode** (1440x3200 DSC, splash fb 0xb8000000) -> writing the fb may not
  show without an MDP kick; vtimer is INTID 27 (not 19 as on sweet); touch is ST FTS over SPI.
- **2026-10-19 PROBE 1 WORKS ON THE PHONE (user report):** `fastboot boot` of zeus-probe1.img shows the probe text and
  the red/green/blue strip at once. So ABL runs our Image, and the splash fb + CTL_0 kick reach the cmd-mode panel.
  Photo (IMG_0013) read: load 0xa8000000, DTB 0xb71fc000 (0xc81c4 bytes), CNTFRQ 19.2 MHz, fb 0xb8000000 size
  0x2b00000, SSPP0 SRC0_ADDR in splash with SRC_SIZE 0x0c8002d0 (720x3200: ABL scans out two 720-wide halves),
  stride 0x1680, format 0x000237ff. Live /memory: 0x80000000+0x6a000000, 0xf4600000+0xba00000,
  0x800000000+0x37500000, 0x837b00000+0x1e00000, 0x840000000+0xc0000000, 0x900000000+0x180000000 (~11.7 GB).
  CTL0 0x00 = 0x0100002d. INTF1 0x280.. contains 0xc80/0xc85/0xc81 (3200-line TE values) and 0x8000003c
  (guess: autorefresh on). **Top ~420 lines of the screen show noise, not our text** (guess: another pipe/layer,
  e.g. ABL's unlocked warning, scanned from a stale buffer) -> probe 2 dumps all 8 pipes.
- **2026-10-19 PROBE 2 ON THE PHONE: USB SERIAL LOG WORKS** (user photo + runner log). GDSC 0xf822f000 (on),
  GSNPSID 0x33313130 (DWC_usb31 1.30a), GCTL 0x00102001, DCTL 0x10f00000, DSTS 0x00d32f04 before; QS HSPHY 0;
  after run DSTS 0x00820001; "connected, speed 0" (high speed) twice, configured, LOG ON; host got /dev/ttyACM0
  3.9 s after `fastboot boot`. So USB DMA works through the SMMU as ABL left it. Phone log file: not uploaded yet.
  **The noise band grows**: ~420 lines at t=0 (probe 1), ~half the screen at t=20 s (probe 2). Something overwrites
  the framebuffer RAM (or the display shows something else) over time. Candidates: stale dirty cache lines from ABL
  written back over our uncached writes; another agent (VM/DSP) using that RAM; a display-side effect.
- **2026-10-19 PROBE 3 ON THE PHONE (user report): no more noise.** DC CIVAC over the splash region before drawing
  fixed it -> the noise was **stale dirty cache lines left by ABL**, written back over our uncached writes.
  Consequence: the kernel loader must clean+invalidate every region it writes (kernel, ADT, boot args) before
  XNU turns caches on. Probe 3 phone log (memA/memB test areas): not uploaded yet.
- **Probe 4 (built, QEMU-tested at EL1, NOT run yet):** probe 3 + PAC test: ID_AA64ISAR1/2, write APIAKeyLo/Hi
  at EL1, read back, SCTLR_EL1.EnIA on, PACIA + AUTIA round trip, EnIA restored. QEMU: "PAC: WORKS". On the
  phone a trap of the key write to Gunyah would show as the red exception screen. Runner default = probe 4.
- Probe 3 details: probe 2 + DC CIVAC over the whole splash region before drawing;
  32 MB RAM test at 0xb0000000 (A = flushed first, B = not), pattern-checked every 5 s; framebuffer hashed in 32
  bands of 100 lines every 5 s (ticker band skipped, snapshot after our own printing) -> USB log says which bands
  changed and a sample of the new data. QEMU: no false positives over 30 s. Runner default is now probe 3.
- **XNU started (2026-10-19):** repo is public (free macOS runners). `.github/workflows/xnu.yml` builds
  xnu-12377.121.6 (macOS 26.5) via blacktop/darwin-xnu-build 6639065; first CI run 39316275160 = stock VMAPPLE
  DEVELOPMENT: never got a macOS runner (runner_id 0), cancelled after 15 min in the queue. Re-dispatched as run
  39321757281: **SUCCESS** (stock VMAPPLE DEVELOPMENT kernel, build step 14 min, artifact xnu-kernel 12221482445,
  76 MB). Patch 0001 = ZEUS machine config (VMAPPLE.h copy without SME, -march=armv8.5a, MAX_CPUS 8; no closed
  KDK lib linked: blacktop's link_kdk.patch only links lib<cfg>.os for T6000/T6020/T8101/T8103/T8112/VMAPPLE ->
  expect undefined symbols at link = what we must write). Workflow default MACHINE_CONFIG is now ZEUS.
  ZEUS run 39327608114 (0001 v1): 253 compile errors, all `CPMU_PMC_COUNT` undeclared (Apple configs get it from a
  private SDK platform header) -> 0001 v2 defines it as 10 in ZEUS.h. Also seen: blacktop's link_kdk.patch and
  remove_applefeatures_include.patch are SKIPPED (do not apply to 12377.121.6) even in the passing VMAPPLE run, so
  that stock kernel linked without the closed KDK library. Full logs: get_workflow_run_logs_url + curl (191 MB zip;
  the signed URL works once and the download through this sandbox's proxy often breaks off).
  Run 39332084864 (0001 v2): failed, no compile error found in the recoverable 436 MB of log -> CI now tees the build
  output and an "Error summary" step writes out/errors.txt (compiler errors, undefined symbols, failed make targets),
  prints it as the last job lines and puts it in the xnu-kernel artifact. Read it with get_job_logs tail.
  Run 39335734524 (0001 v2): **everything compiles**; only the final link of kernel.development.zeus fails
  (linker message not caught by the summary regex) -> summary now also prints 80 lines before each
  'linker command failed'. Plan + source findings: `docs/xnu-plan.md` (VMAPPLE = no
  PPL/SPTM, GICv3, paravirtual PAC hooks -> fixed standard PAC keys on zeus; drop SME; GICR stride 0x40000).
- Probe 2 details: `shim/usb.c` = CDC-ACM USB serial log (port of sweet
  0073-0075 to bare metal: QSCRATCH VBUS override, DCTL soft reset, polled event buffer, ep0 state machine, ACM
  descriptors, bulk IN 0x82 streams `logbuf`; resend from the start when the host raises DTR, so ModemManager
  probing can't eat it). Reads GDSC 0x149004 first and skips USB if the power domain is off. Open risk: the USB
  DMA goes through the SMMU (DT iommu-dma-addr-pool); works only if ABL's setup still lets DMA through.
  Screen: text from row 9 (below the noise), 8-pipe table, CTL0/INTF1 dumps, USB register lines, ticker; reset 120 s.
  `prebuilt/zeus-probe2.img` sha256 262335356c0f...
- **Runner** `tools/zeus-probe.py` (default probe 2, `--probe 1` for the old one): no questions. One fastboot device
  (or reboots the one adb device into fastboot), product zeus + unlocked yes, `fastboot boot`, then captures the
  phone's ttyACM ("Zeus probe log", 1209:0001) to `logs/probe2-*-phone.txt` until the phone resets (sudo fallback
  if the user lacks dialout/uucp), then waits for Android. Tested with fake fastboot/adb + a pty as the phone.
- Probe stage 1 details (probe 1, `--probe 1`): `shim/` (start.S, probe.c, probe.ld, build.sh; font from
  sweet) -> `shim/out/zeus-probe1.img` (boot.img v4 from `tools/mkbootimg4.py`, kernel = probe Image, empty
  cpio; ABL adds the flashed vendor_boot + DTB). Prebuilt copy `prebuilt/zeus-probe1.img`, sha256
  a2c4b42ef122... (build is reproducible with Ubuntu clang 18).
  - Does: draw on the splash fb (DT `splash_region`, default 1440x3200 stride 5760), CTL_0 START kick (0xae1601c),
    find the SSPP whose SRC0_ADDR is inside the splash region and redraw with its size/stride, print EL, MPIDR,
    load/DTB address, live /memory, SSPP, CTL0/INTF1/PP0 register dumps, kick every 250 ms, PSCI SYSTEM_RESET at 60 s.
    Any exception: red screen with ESR/ELR/FAR, one guarded kick, reset after 30 s.
  - Only writes: own RAM, splash fb, CTL_0 START. No storage code.
  - QEMU (virt, fake splash + fake SSPP regs in RAM): screen correct, SSPP found, reset after 61 s. At EL1 with the
    real MDP address (unmapped in QEMU): red EXCEPTION screen, ESR 0x96000050 FAR 0xae1601c, no fault loop.
- Runner allowlist: fastboot devices/getvar product/getvar unlocked/boot, adb devices/reboot bootloader.
- Next after the result: if text shows -> stage 2 = USB device mode (DWC3 0xa600000) streaming the log to the PC
  (sweet 0072-0077 idea) so we get real logs instead of photos. If nothing shows: work out the cmd-mode frame
  trigger from the register dump / photo.

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
