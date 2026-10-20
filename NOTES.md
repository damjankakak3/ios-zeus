# ios-zeus notes

## Handoff (2026-10-19)

- Goal: iOS 26 (iPhone 15 Pro build, iPhone16,1 / D83AP; user's choice 2026-10-20, was iPhone 11) on zeus, booted with `fastboot boot`, root from ramdisk/USB.
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
  'linker command failed'.
  Run 39338815163: link fails with `ld: library .../KDK_26.5_25F71.kdk/System/Library/KernelSupport/
  libZEUS.os.DEVELOPMENT.a not found`: xnu-12377's own MakeInc.def force_loads a closed lib<CONFIG>.os.<KCONF>.a
  for every machine config (so blacktop's link_kdk.patch is "already upstream" and the stock VMAPPLE kernel DOES
  contain closed code). 0001 v3 excludes ZEUS from it; CI now lists libVMAPPLE.os members/symbols (out/kdk-libs).
  Run 39341366443 (0001 v3): link fails on hundreds of undefined pmap_*/machine_*/cache/locore symbols.
  **libVMAPPLE.os.DEVELOPMENT.a (closed, KDK) = the arm64 core**: caches_asm pinst cswitch pmap pmap_cs pmap_data
  pmap_iommu pmap_misc pmap_ppl_interface pcb locore gxf_exceptions machine_routines_asm machine_routines_apple
  machine_routines_nos amcc_rorgn_* start sptm_stubs sart dart nvmeppl uat IOUnifiedAddressTranslator.
  The SOURCE is in the public tree, gated by options nos_arm_asm / nos_arm_pmap (osfmk/conf/files.arm64), which
  Apple's configs leave off. 0001 v4 adds config/MASTER.arm64.zeus.MacOSX (doconf picks MASTER.<cpu>.<soc>.<platform>
  first) = MacOSX config + `nos_arm_asm nos_arm_pmap` in ARM_EXTRAS_BASE, CPU_COUNTERS_BASE empty (no Apple CPMU).
  Run 39343976704 (0001 v4): only 3 errors: start.s needs arm64/tunables/tunables.s (unpublished -> empty
  APPLY_TUNABLES macro), machine_routines_asm.s CPU_OVRD (Apple reg -> NO_CPU_OVRD in the ZEUS board config, like
  other Apple SoCs), pmap.c includes arm64/ppl/sart.h + uat.h (unpublished -> skipped with sart_bootstrap for ZEUS).
  Run 39346711070 (v5): 2 errors left in osfmk/arm/pmap/pmap.c: ptep_get_iommu (panic text only) and
  pmap_ppl_lockdown_pages/unlockdown (closed; PPL page lockdown) -> ZEUS no-op stubs (no monitor).
  Run 39349482046 (v6): 1 error: pmap_phys_write_disable used before its static definition -> forward decl.
  Run 39352098696 (v7): no compile/link errors in the summary, only make[6] build_all Error 2 (non-compiler
  failure: suspect exports/symbol-set check) -> summary now prints 25 lines before each failed make step.
  Run 39354666184: 'No rule to make target osfmk/arm64/amcc_rorgn_{ppl,ppl_amcc,common,pv_ctrr}.c' (listed in
  files.arm64 for nos_arm_asm, never published). Fix: empty placeholder files + drop HAS_PARAVIRTUALIZED_CTRR from
  ZEUS.h (no KTRR/CTRR on zeus; all rorgn_* uses are under KERNEL_INTEGRITY_* guards).
  Run 39357184943 (1809227): EVERYTHING COMPILES. Final link of kernel.development.zeus fails, undefined (closed
  KDK symbols): CleanPoC_DcacheRegion_Force_nopreempt(_nohid), ml_device_is_prod_fused, ml_enable_monitor,
  pmap_pending_preemption (out-of-line in DEVELOPMENT + SCHED_HYGIENE_DEBUG). Fix (0001 v10): ZEUS-only versions in
  caches_asm.s (branch to CleanPoC_DcacheRegion_internal), machine_routines.c (no-op / FALSE), pmap.c
  (= _pmap_pending_preemption_real). Guess: the summary list may have been truncated; next run will tell.
  Run 39359821848 (eafdefe): **kernel.development.zeus LINKS** (fact: config/allsymbols is made from it and its
  dSYM was installed). Failed only at config check_all_exports / Private.symbolset: 67 exported names from closed
  code (IOUnifiedAddressTranslator, pmap_iommu_*, sart/nvme/t*dart/sptm_gen3dart/uat descs, pmap_cs_configuration,
  pmap_has_iofilter_protected_write). Fix (0001 v11): config/ZEUS.unexported + config/Makefile filters the export
  lists into config/zeus-exports/ for ZEUS only. Next: artifact should contain kernel.development.zeus -> 0002.
  **Run 39364183407 (d86abb1): SUCCESS.** Artifact xnu-kernel (id 12239376415, expires 2027-01-18) holds
  kernel.development.zeus, 22283960 bytes, + kernel.development.zeus.link/. sha256 not recorded (Collect's shasum
  hit the .link dir; fixed in the workflow). Patch 0001 = BUILD only; nothing in it makes the kernel run on Cortex.
  Next: 0002 runtime (hypercalls, PAC keys, GICR stride, Apple sysregs, CPU-off path), then the loader.
  **0002 v1 (xnu/patches/0002-zeus-no-hypercalls-fixed-pac.patch, untested on CI when written):**
  PAC_INIT_KEY_STATE writes fixed keys (ZEUS_PAC_KEY_LO/HI in ZEUS.h, +1 per key) to the standard
  AP{IA,IB,DA,DB,GA}Key{Lo,Hi}_EL1; SET_JOP/ROP_KEY_REGISTERS and cswitch key reprogramming are off;
  ml_enable/disable_user_jop_key no hvc; default rop/jop pid 0; hvg_is_hcall_available() = false (no VMAPPLE
  hypercall ever issued on Gunyah); PSCI CPU_OFF/SYSTEM_SUSPEND via smc; GICR_PE_SIZE 0x40000.
  Findings (source): the Apple-hardware sysreg paths are already skipped for APPLEVIRTUALPLATFORM (VMAPPLE
  guests can't touch them either), and the CPU-off S3_5_C15_C1_1 write is dead code for VMAPPLE (PSCI first)
  -> the earlier RUNTIME TODOs are not blockers. Kept APPLE_ARM64_ARCH_FAMILY: cache code assumes "fully
  coherent" (CleanPoU no-op, dc civac for PoC). Guess: fine for CPU-only code on SM8450; non-coherent DMA
  (USB) must do its own cache maintenance in our drivers. PV ticket locks: only built for virtualized SoCs.
  research/xnu now has a local commit ffdc90a = 0001; 0002 is `git diff` against it.
  **Run 39366356838 (cb29ee6, 0001+0002): SUCCESS.** kernel.development.zeus 22283528 bytes, sha256
  6eea31abc4e1207e2d3bfa6da08858e4d70258326db099f6bc93aeed12fb6ec9, artifact id 12239564715.
  Not run on hardware. Next: loader (design in docs/xnu-plan.md "Loader design"), then 0003 USB console.
  **0003 zeus-screen-console (pushed with the kmutil step, CI pending):** osfmk/arm64/zeus_early.c (from sweet
  0003/0010/0012): text console on the ABL framebuffer, kicks CTL_0 START (command-mode panel) at most every 20 ms
  and on every progress mark; start.s maps fb (WC) + MDSS block 0x0a000000 (Device) VA=PA in the bootstrap V=P L2;
  arm_vm_init adds permanent aliases at VM_MIN_KERNEL_ADDRESS + 1/2 x 32MB and relocates before TTBR0 goes;
  progress marks 1-14 (bar at the bottom + text); panic text on screen from arm_init on; pe_serial.c driver
  "zeus,fbcon" (ADT /defaults serial-device -> phandle) routes kprintf to the same console.
  **Fact (source): a plain MH_EXECUTE arm64e kernel never signs its auth pointers** (arm_slide_rebase_and_sign_image
  only rebases/signs a MH_FILESET KC or a static kernelcache). Sweet avoided this with a non-PAC kernel. So CI now
  builds a kernel-only boot KC: `kmutil create -a arm64e -n boot -x -k kernel ...` -> out/kernelcache.development.zeus.
- **XNU boot chain (written 2026-10-20, QEMU-tested, NOT run on the phone yet):**
  - `shim/loader.c` (port of sweet shim/loader.c) inside probe 5 (`prebuilt/zeus-probe5.img`): after the probe 4
    checks + PAC test, if the ramdisk holds a ZEUSXNU1 payload: finds it via the trailer at initrd-end (ABL puts
    our ramdisk after the vendor_boot one; start may be unaligned), checks xnu RAM is inside one live /memory bank,
    hits no /reserved-memory child and not loader/DTB/ramdisk/fb, requires MPIDR core 0, gives the USB log 10 s,
    disconnects USB, DC CIVAC on the destination, copies segments + ADT, writes boot_args (Rev/Ver 2, Video = splash
    fb), IC IALLU, jumps with x0 = boot_args PA. Any failed check: reason on screen + USB, nothing written, reset.
  - `tools/mkadt.py` (port of sweet's): zeus GIC/timer/8 CPUs (cpu0 "running"), RAM 0x900000000 + 6 GB,
    /defaults serial-device -> zeus-fbcon node, identity D83AP / iPhone16,1, chip-id 0x8130, /product from the
    real D83AP tree if research/ipsw/d83ap.adt exists (optional).
  - `tools/mkpayload.py`: MH_FILESET only (refuses MH_EXECUTE), entry = com.apple.kernel LC_UNIXTHREAD,
    virt_base = 32 MB block of the KC header, trailer {size, "ZEUSEND1"}.
  - `tools/qemu-loader-test.py`: fake fileset kernel in QEMU virt, payload at an odd ramdisk offset. Result
    2026-10-20: all checks ok (boot_args fields, segment copy, zero fill, ADT, x0).
  - Runner: `python3 tools/zeus-probe.py --xnu xnu-kernel.zip` (CI artifact zip or kernelcache file).
  - Blocked: publishing the KC as a public GitHub release (auto-mode denied "public surface"); user decides.
- **0004 zeus-gic-group1-irq (CI pending):** VMAPPLE takes the vtimer (PPI 27) as a Group 0 FIQ and sends every
  other IRQ to gCPUIC (only exists with the closed kexts). Guess (sweet's lesson + Linux uses Group 1 only): Gunyah's
  vGIC does not deliver Group 0 to this VM. ZEUS: pe_init_fiq puts SGIs/PPIs in Group 1 (prio 0xA0, PPI 27 + SGI
  0-15 enabled), ICC_BPR1/PMR, GICD EnableGrp1A, ICC_IGRPEN1; sleh_irq -> zeus_gic_irq: IAR1, 27 = rtclock_intr,
  <16 = cpu_signal_handler, others counted; EOIR1. Without it the first timer wait would hang.
- **0005 zeus-platform-expert-gic-pmgr (CI pending):** iokit/Kernel/arm/ZeusPlatform.cpp. AppleARMSMP's cpu_boot_thread
  (USE_APPLEARMSMP) waits for an IOPlatformExpert, a child controller of PassthruInterruptController (gAIC) and an
  IOPMGR; VMAPPLE gets them from closed kexts. ZeusPlatformExpert (sweet 0016, matched on 'xiaomi,zeus' in
  KernelConfigTables) starts ZeusGIC (registers as IOInterruptController<gic phandle>, hooks into the Passthru
  controller when it is published, sendIPI = ICC_SGI1R SGI 1) and ZeusPMGR (enableCPUCore = PSCI CPU_ON via smc,
  idle hooks no-ops). mkadt: gic gets interrupt-controller + #interrupt-cells 1; cpus get interrupt-parent + 3
  interrupts (aic_ipis path). First boot uses cpus=1.
  Run 39444636078 (216b151, 0001-0005): everything compiles except 2 errors in ZeusPlatform.cpp (const OSSymbol for
  registerInterruptController; IOInterruptHandler's 3rd param is void *) -> fixed. kmutil step was skipped (build failed).
  Run 39449773831 attempt 3 (ed8d05d; attempts 1-2 were cancelled after 15 min without a runner): **0001-0005 BUILD
  OK**, kernel.development.zeus 22292512 bytes sha256 0109036c4902a7e4...4bd0. kmutil: "Missing Developer Kit ... KDK
  matching your build 25G83" (runner OS) although --kdk KDK_26.5_25F71 was given -> now --build 25F71
  --allow-missing-kdk --no-variant-extension.
  **Run 39464767608 (8f66c4b): build + kmutil OK.** out/kernelcache.development.zeus = MH_FILESET arm64e (cpusub E),
  20889600 bytes, sha256 2a879830c49eb651a2abce587493122fe35c96436c94f72d37479c6f2ebfe6d0; KC header
  0xfffffe0007004000, entry com.apple.kernel at 0xfffffe0007008000; segments __TEXT __PRELINK_TEXT __DATA_CONST
  __TEXT_EXEC __PRELINK_INFO __DATA __LINKEDIT (ends 0xfffffe00083f0000). kernel.development.zeus sha256 4a08c6b8...
  **NEXT: first phone test** `python3 tools/zeus-probe.py --xnu xnu-kernel.zip` (artifact of this run). Not run yet.
  RUNTIME TODO: apple_arm64_common.h sets APPLE_ARM64_ARCH_FAMILY -> Apple sysreg accesses everywhere; each must be
  disabled for ZEUS before it runs on Cortex (sweet used a non-Apple-family config for this reason).
  RUNTIME TODO: with NO_CPU_OVRD, the CPU-off WFI path writes Apple S3_5_C15_C1_1 (IPI clear) -> UNDEF on Cortex.
  Also noted: kernel is built with -DXNU_TARGET_OS_OSX / PLATFORM MacOSX (sweet did the same and ran iOS userland). Plan + source findings: `docs/xnu-plan.md` (VMAPPLE = no
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
