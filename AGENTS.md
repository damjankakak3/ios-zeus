# Working on ios-zeus

- Read `NOTES.md` first: goal, hardware facts, current state, next action.
- The user switches agents often. Update the handoff in `NOTES.md` after each milestone and
  before stopping: commits, CI run IDs, exact boot results, unfinished work. Mark facts vs guesses.
- **zeus is the user's daily phone. Android must survive.** Never write internal storage:
  no `fastboot flash/erase/format/-w/update/set_active/oem/flashing`, no `dd of=`, no root on
  UFS. The ZEUS kernel has no UFS driver. Only `fastboot boot` (RAM) is used.
- The bootloader must already be unlocked; unlocking a Xiaomi wipes it. Never run unlock or lock.
- The user drives the phone with a local agent (`docs/local-agent-prompt.md`). Give it exact
  commands and stop conditions, and expect verbatim output back in `recon/`.
- `ios-sweet` (branch `main` of this repo, or dipa-studio/ios-sweet) is a reference only. Port
  code from it per file, with a note of where it came from. Do not merge its history.
- Keep device dumps, serial numbers and Apple artifacts out of git. Commit completed work.
