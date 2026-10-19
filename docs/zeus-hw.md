# zeus hardware facts

Source: stock EEA firmware OS3.0.3.0.VLBEUXM (Android 15), `vendor_boot` base DTB #10
("Waipio v2 SoC", msm-id 457 rev 2.0) + `dtbo` overlay #35 ("Zeus based on SM8450",
board-id 0x10008). Extracted with `tools/zeus-dt-extract.py`, merged with `fdtoverlay`
(local copy in `dtb/stock/`, not in git).

Everything here is **from the stock DT**, not measured on the phone. ABL edits the DT at
boot (memory, panel choice, some VM regions), so the shim must read the live DT it gets in x0.

## CPU

| cpu | MPIDR | core | capacity |
| --- | --- | --- | --- |
| 0-3 | 0x000-0x300 | Cortex-A510 | 1024 |
| 4-6 | 0x400-0x600 | Cortex-A710 | 2253 |
| 7 | 0x700 | Cortex-X2 | 2386 |

- PSCI 1.0, `method = "smc"`. Gunyah hypervisor below us (`hyp_region` at 0x80000000, `gunyah-*` nodes).
- (Cortex part numbers from the SM8450 spec, not from the DT, which only says `qcom,kryo`.)

## Interrupts and timer

- GICv3: GICD 0x17100000 (64K), GICR 0x17180000 (2M, stride 0x40000 = GICv4-style 256K frames), ITS 0x17140000.
- Arch timer 19.2 MHz. PPIs in the DT: <13> secure phys, <14> NS phys, <11> virt, <12> hyp.
  -> **vtimer INTID 27, ptimer INTID 30** (standard numbering; sweet's SM7150 was 19/18. Verify on hardware.)

## Memory (static reserved regions, all `no-map` unless noted)

```
0x80000000  6M   hyp (Gunyah)          0x9fd00000 49M  adsp
0x80600000-0x80c00000  xbl/aop/smem/cpucp (small)
0x80c00000 70M   cdsp secure heap      0xa6b80000-0xa7000000 xbl ramdump/sc, global sync
0x85700000  7M   video                 0xa7000000  4M  ramoops (map ok)
0x88000000 25M   slpi                  0xb8000000 43M  splash_region = ABL framebuffer (map ok)
0x89900000 32M   cdsp                  0xbb000000  ?   oem_vm (size filled by ABL)
0x8b900000-0x8bc00000 ipa/gpu ucode/spss/spu
0x8bc00000 306M  mpss (modem)          0xe0000000-0xe0b00000 qhee/cpusys_vm/hyp reserved
0x9ee00000  7M   cvp                   0xe0b00000  ?   trust_ui_vm (size filled by ABL)
0x9f500000  8M   camera                0xe55f3000-0xe5700000 trust_ui_vm rings
                                       0xe8800000-0xea000000 tz_stat, tags, qtee
```
- `/memory` is empty in the stock DT (ABL fills it). The phone has 12 GB; the bank layout comes from the live DT.
- Free-looking holes in the low 4 GB: 0xa7400000-0xb8000000 (268 MB), 0xbab00000-0xe0000000 (before oem_vm,
  size unknown), above 0xea000000. Pick the kernel PA only after reading the live DT.
- Dynamic CMA pools (display 164M, qseecom, wlan, audio...) are Linux allocations. They don't apply to us.

## Display

- ABL framebuffer: `splash_region` 0xb8000000, 43 MB (labelled `cont_splash_region`).
- MDSS/SDE at 0xae00000. Primary DSI display, panel chosen by ABL at runtime.
- zeus panel (the only 1440-wide Xiaomi entry): **"xiaomi 38 0c 0a cmd mode dsc dsi panel", 1440x3200,
  COMMAND mode, DSC (slice 720x50), up to 120 Hz.**
- **Big difference from sweet:** sweet's panel was video mode, so writing the framebuffer showed up at once.
  A command-mode panel keeps its own frame memory and only updates when the display controller sends a frame
  (CTL start, or MDP autorefresh). Whether ABL leaves autorefresh on is unknown. **The first shim test answers it.**
  Stride/format of the splash buffer are unknown (guess 1440x3200 a8r8g8b8, stride 5760).

## Input

- Touch: **ST FingerTip (`st,spi`, "fts")** on GENI SPI @0x990000, 12 MHz, reset GPIO 20, IRQ GPIO 21 (TLMM),
  coordinates 0..1439 x 0..3199. (sweet: Goodix over I2C.)
- Buttons: PMK8350 (SID 0) PON at 0x1300: pwrkey + resin (volume down). Volume up: PMIC GPIO (to find).
- SPMI PMIC arbiter at 0xc42d000 (core 0xc400000, chnls 0xc500000, obsrvr 0xc440000, intr 0xc4c0000).
  PMICs: PMK8350 (0), PM8350 (1), PM8350C (2), PM8350B (3, charger), PMR735A (4), PMR735B (5), PM8450 (7), PM8010 (8, 9).

## USB

- DWC3 at 0xa600000 (`androidboot.usbcontroller=a600000.dwc3`), QUSB/SNPS femto HS PHY. Same family as sweet:
  the device-mode, CDC-ECM and USB root plan carries over.

## Debug

- `stdout-path` = GENI UART @0x99c000, 115200. On a retail phone it's only reachable from test pads. Assume
  no serial: the screen and USB are the debug channels.

## Boot image

- vendor_boot header v4, page 4096, kernel_addr 0x8000, ramdisk_addr 0x1000000, dtb_addr 0x1f00000 (offsets
  from ABL's base). Bootconfig: `androidboot.hardware=qcom androidboot.memcg=1 androidboot.usbcontroller=a600000.dwc3`.
- `fastboot boot` of a boot.img: ABL adds the flashed vendor_boot ramdisk and passes the DTB in x0. The shim
  must be position independent: the load address is ABL's choice (to be measured).
