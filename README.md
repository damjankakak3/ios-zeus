# ios-zeus

Apple's **iOS 26** userland on the **Xiaomi 12 Pro** (`zeus`, Snapdragon 8 Gen 1 / SM8450),
presented to iOS as an **iPhone 15 Pro** (`iPhone16,1` / `D83AP`, A17 Pro).

A clean project. It borrows ideas from [`ios-sweet`](https://github.com/dipa-studio/ios-sweet)
(iOS 16 on the Redmi Note 10 Pro) but shares no code history. Code from sweet is ported one
file at a time, when it is needed, and cleaned up on the way.

> Planning stage. Nothing runs on the phone yet.

## Ground rules

- **Android stays.** zeus is a daily phone. Nothing is ever written to its internal storage.
  Everything boots with `fastboot boot` (RAM only), and the kernel has no UFS driver.
- The iOS root comes from a ramdisk in the boot image, later from the PC over USB.
- No Apple files, device dumps or serial numbers in git.

## Why iOS 26 and not 16 or 27

- iOS 17 and later are `arm64e` only, which needs pointer authentication (PAC, ARMv8.3).
  sweet's ARMv8.2 CPU has none, so it is capped at iOS 16. SM8450 (ARMv9.0) has PAC.
- The kernel must match the userland. The newest XNU source Apple has published is
  `xnu-12377.121.6`, the iOS 26 family. The iOS 27 kernel is not open source yet.

See `docs/plan.md`.
