#!/usr/bin/env python3
"""ios-zeus probe runner: boot the stage-1 probe on zeus and log everything.

  python3 tools/zeus-probe.py            (from the repo root, after git pull)

What it does, in order:
  1. Builds shim/out/zeus-probe1.img if clang + ld.lld are installed, else uses
     prebuilt/zeus-probe1.img. Checks the image's sha256.
  2. Finds the phone: exactly one fastboot device (or one adb device, which it
     asks to reboot into fastboot). Checks product = zeus and unlocked = yes.
  3. Asks you to type BOOT, then runs `fastboot boot` (RAM only, nothing is
     flashed), and watches USB until Android is back (adb) or 5 minutes pass.
  4. Asks what the screen showed, and writes everything to logs/probe1-*.txt.
     Upload that file (and a photo of the screen) to Claude.

Safety: the only phone commands this script can run are listed in ALLOWED.
Anything else raises an error before it reaches the phone.
"""
import datetime, hashlib, os, re, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREBUILT = os.path.join(ROOT, "prebuilt", "zeus-probe1.img")
BUILT = os.path.join(ROOT, "shim", "out", "zeus-probe1.img")

# argv prefixes this script may send to the phone. Nothing that writes flash.
ALLOWED = [
    ["fastboot", "devices"],
    ["fastboot", "-s", None, "getvar", "product"],
    ["fastboot", "-s", None, "getvar", "unlocked"],
    ["fastboot", "-s", None, "boot", None],
    ["adb", "devices"],
    ["adb", "-s", None, "reboot", "bootloader"],
]

log_lines = []
serials = set()


def redact(s):
    for sn in serials:
        s = s.replace(sn, "SERIAL")
    return s


def log(msg=""):
    stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    line = f"[{stamp}] {redact(msg)}"
    log_lines.append(line)
    print(line, flush=True)


def allowed(cmd):
    for pat in ALLOWED:
        if len(pat) == len(cmd) and all(p is None or p == c for p, c in zip(pat, cmd)):
            return True
    return False


def run(cmd, timeout=15):
    if not allowed(cmd):
        raise RuntimeError(f"refusing to run {cmd}: not in ALLOWED")
    log("$ " + " ".join(cmd))
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        out = (r.stdout + r.stderr).strip()
        for l in out.splitlines():
            log("  " + l)
        log(f"  (exit {r.returncode})")
        return r.returncode, out
    except subprocess.TimeoutExpired:
        log(f"  TIMEOUT after {timeout} s")
        return None, ""


def quiet(cmd):
    """Same as run() for polling, without logging every call."""
    if not allowed(cmd):
        raise RuntimeError(f"refusing to run {cmd}")
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        return r.stdout
    except subprocess.TimeoutExpired:
        return ""


def devices(tool):
    out = quiet([tool, "devices"])
    want = "fastboot" if tool == "fastboot" else "device"
    found = []
    for l in out.splitlines():
        parts = l.split()
        if len(parts) >= 2 and parts[1] == want:
            found.append(parts[0])
            serials.add(parts[0])
    return found


def save_and_exit(code, why=""):
    if why:
        log(why)
    os.makedirs(os.path.join(ROOT, "logs"), exist_ok=True)
    name = "probe1-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + ".txt"
    path = os.path.join(ROOT, "logs", name)
    with open(path, "w") as f:
        f.write("\n".join(log_lines) + "\n")
    print(f"\nLog saved: {path}\nUpload it to Claude (plus a photo of the screen if you took one).")
    sys.exit(code)


def ask(prompt):
    a = input(prompt).strip()
    log(f"Q: {prompt.strip()}  A: {a}")
    return a


def get_image():
    img = None
    if shutil.which("clang") and shutil.which("ld.lld") and shutil.which("llvm-objcopy"):
        log("building the probe (clang found)")
        r = subprocess.run(["bash", os.path.join(ROOT, "shim", "build.sh")], capture_output=True, text=True)
        for l in (r.stdout + r.stderr).strip().splitlines():
            log("  " + l)
        if r.returncode == 0:
            img = BUILT
        else:
            log("build failed, falling back to the prebuilt image")
    if img is None:
        img = PREBUILT
    data = open(img, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    log(f"image {os.path.relpath(img, ROOT)} {len(data)} bytes sha256 {sha}")
    want = open(PREBUILT + ".sha256").read().split()[0]
    if sha != want:
        log(f"WARNING: differs from prebuilt sha256 {want} (local build with another compiler?)")
    if data[:8] != b"ANDROID!":
        save_and_exit(1, "image is not a boot image, stopping")
    return img


def main():
    log(f"ios-zeus probe runner, repo {ROOT}")
    rev = subprocess.run(["git", "-C", ROOT, "log", "-1", "--format=%h %s"], capture_output=True, text=True).stdout.strip()
    log(f"git: {rev}")
    for tool in ("fastboot", "adb"):
        if not shutil.which(tool):
            save_and_exit(1, f"{tool} not found: install android-tools / platform-tools")
    img = get_image()

    fb = devices("fastboot")
    if not fb:
        ad = devices("adb")
        if len(ad) == 1:
            a = ask("Phone is in Android. Reboot it into fastboot now? [y/N] ")
            if a.lower() != "y":
                save_and_exit(1, "stopped by user")
            run(["adb", "-s", ad[0], "reboot", "bootloader"])
            for _ in range(60):
                time.sleep(1)
                fb = devices("fastboot")
                if fb:
                    break
        elif len(ad) > 1:
            save_and_exit(1, f"{len(ad)} adb devices connected: unplug the others")
    if len(fb) != 1:
        save_and_exit(1, f"need exactly one fastboot device, found {len(fb)}. "
                         "Put the phone in fastboot (Power + Volume Down) and run again")
    sn = fb[0]
    log("one fastboot device found")

    rc, out = run(["fastboot", "-s", sn, "getvar", "product"])
    if rc is None:
        save_and_exit(1, "fastboot hung. Hold Power + Volume Down to re-enter fastboot, then retry")
    m = re.search(r"product:\s*(\S+)", out)
    if not m or m.group(1) != "zeus":
        save_and_exit(1, f"product is {m.group(1) if m else 'unknown'}, not zeus: stopping")
    rc, out = run(["fastboot", "-s", sn, "getvar", "unlocked"])
    m = re.search(r"unlocked:\s*(\S+)", out)
    if not m or m.group(1) != "yes":
        save_and_exit(1, "bootloader does not report unlocked: yes, stopping")

    print("""
About to run:  fastboot boot zeus-probe1.img
 - loads the probe into RAM only, nothing is written to the phone's storage
 - the screen should show "ios-zeus probe 1" with a red/green/blue strip,
   or a red EXCEPTION screen; either way PHOTOGRAPH it
 - the phone resets by itself after ~60 s and Android starts normally
 - if it is stuck for more than 2 minutes, hold Power for ~15 s
""")
    if ask("Type BOOT to start: ") != "BOOT":
        save_and_exit(1, "stopped by user")

    t0 = time.time()
    rc, out = run(["fastboot", "-s", sn, "boot", img], timeout=90)
    log(f"fastboot boot returned after {time.time() - t0:.1f} s")
    if rc != 0:
        save_and_exit(1, "fastboot boot failed (see above). Nothing was changed on the phone")

    # Watch USB: when fastboot disappears, when Android (adb) or fastboot shows up again.
    log("watching USB for 5 minutes (Ctrl+C stops early)")
    state = "fastboot"
    try:
        while time.time() - t0 < 300:
            f, a = devices("fastboot"), devices("adb")
            now = "fastboot" if f else "android-adb" if a else "none"
            if now != state:
                log(f"USB: {state} -> {now} at +{time.time() - t0:.1f} s")
                state = now
            if now == "android-adb":
                log("Android is back")
                break
            time.sleep(1)
    except KeyboardInterrupt:
        log("watch stopped with Ctrl+C")
    if state != "android-adb":
        log("Android (adb) not seen. Normal if USB debugging is off or the phone is still in the probe")

    print("\nWhat did the screen show?")
    print("  1 = 'ios-zeus probe 1' text + red/green/blue strip")
    print("  2 = red EXCEPTION screen")
    print("  3 = stayed on the Xiaomi / unlocked-bootloader screen")
    print("  4 = black screen")
    print("  5 = something else")
    ask("Number: ")
    ask("Colours of the bottom strip left to right (or 'none'): ")
    ask("Did it reset by itself? About how many seconds? : ")
    ask("Anything else you noticed: ")
    save_and_exit(0, "done")


if __name__ == "__main__":
    main()
