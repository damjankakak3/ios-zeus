# Prompt for the local agent (paste everything below the line)

---

You are helping with a hobby project on a Linux PC. A phone is connected over USB: a
**Xiaomi 12 Pro (codename `zeus`)**. It is the owner's **daily phone with Android on it.
Android and all its data must survive.** Your job today is **only to read information**.
You do not change anything on the phone.

## Absolute rules (break one = stop and tell the owner)

NEVER run any of these, not even "to test":
- `fastboot flash`, `fastboot erase`, `fastboot format`, `fastboot -w`, `fastboot update`,
  `fastboot flashall`, `fastboot set_active`, `fastboot oem ...`, `fastboot flashing ...`
  (this includes `flashing unlock` and `flashing lock`; both WIPE the phone)
- `fastboot reboot edl`, `adb reboot edl`, anything with `dd`, `mkfs`, `rm`, `wipe`, `format`
- `adb root`, `adb remount`, `adb disable-verity`, `adb install`, any `su`
- any command you were not given below. If you think you need one, ASK the owner first.

ALLOWED commands, exactly these:
- `adb devices`, `adb shell getprop <name>`, `adb shell getprop`, `adb shell cat /proc/cpuinfo`,
  `adb shell cat /proc/meminfo`, `adb shell uname -a`, `adb shell ls -l /dev/block/by-name/`
  (listing only), `adb reboot bootloader`
- `fastboot devices`, `fastboot getvar <one name>` (only the names listed in step 3, one at a
  time, never `getvar all`), `fastboot reboot`
- `fastboot boot <file>` ONLY for a file the owner hands you, and ONLY after they say "boot it".
  `fastboot boot` only loads into RAM and is allowed. `fastboot flash` is never allowed.

If any command hangs for more than 60 s, press Ctrl+C, do not retry, and tell the owner
(they may have to hold Power + Volume Down to get back into fastboot).
Never unplug or reboot the phone on your own unless a step says so.

## Steps

### 1. Android side (the phone is booted into Android, USB debugging on)
Run each and save the output:
```
adb devices
adb shell getprop ro.product.device
adb shell getprop ro.product.model
adb shell getprop ro.build.fingerprint
adb shell getprop ro.build.version.incremental
adb shell getprop ro.boot.flash.locked
adb shell getprop ro.boot.verifiedbootstate
adb shell getprop ro.boot.slot_suffix
adb shell getprop ro.boot.hardware.platform
adb shell getprop ro.board.platform
adb shell uname -a
adb shell cat /proc/cpuinfo
adb shell cat /proc/meminfo
adb shell ls -l /dev/block/by-name/
```

**STOP CHECK:** if `ro.product.device` is not `zeus`, stop and report.
If `ro.boot.flash.locked` is `1` or `ro.boot.verifiedbootstate` is `green`, the bootloader
is **LOCKED**. Stop. Tell the owner: "Bootloader is locked; unlocking would erase the phone."
Do nothing else.

### 2. Into fastboot
Ask the owner: "OK to reboot into fastboot? Android will come back after." Only on yes:
`adb reboot bootloader`, wait 20 s, `fastboot devices`.

### 3. Fastboot info (one at a time, stop at the first hang)
```
fastboot getvar product
fastboot getvar unlocked
fastboot getvar current-slot
fastboot getvar max-download-size
fastboot getvar version-bootloader
```

### 4. Back to Android
`fastboot reboot`, then wait for `adb devices` to show the phone. Tell the owner when
Android is up again.

### 5. Report
Write `recon/zeus-recon.md` in the project folder with:
- every command and its full, exact output (copy, don't summarise)
- anything that failed or hung, with the exact error
- do NOT include the serial number (replace it with `SERIAL`)

Then say "done" and wait. Do not try to continue to further steps on your own.
