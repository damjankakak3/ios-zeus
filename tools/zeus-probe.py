#!/usr/bin/env python3
"""ios-zeus probe runner: boot the probe on zeus and capture its log. No questions.

  python3 tools/zeus-probe.py [--probe 1|2|3]     (from the repo root, after git pull)

  1. Builds shim/out/zeus-probe3.img if clang + ld.lld exist, else uses prebuilt/.
  2. Finds the phone: exactly one fastboot device, or one adb device which it
     reboots into fastboot. Stops unless product = zeus and unlocked = yes.
  3. `fastboot boot` (RAM only, nothing is flashed).
  4. Probe 2+: waits for the phone's USB serial port ("Zeus probe log",
     1209:0001) and saves everything the phone sends until it resets.
  5. Watches until Android is back (adb) or 5 minutes pass.

Writes logs/probeN-<time>.txt (runner) and logs/probeN-<time>-phone.txt (phone
log). Upload both. The serial number is masked.

Safety: the only phone commands this script can run are listed in ALLOWED.
"""
import argparse, datetime, glob, hashlib, os, re, shutil, subprocess, sys, termios, time, tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LATEST = 3	# shim/ builds this probe; older ones run from prebuilt/

# argv patterns this script may send to the phone (None = any). Nothing that writes flash.
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
STAMP = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
T0 = time.time()


def redact(s):
    for sn in serials:
        s = s.replace(sn, "SERIAL")
    return s


def log(msg=""):
    line = f"[+{time.time() - T0:7.1f}s] {redact(msg)}"
    log_lines.append(line)
    print(line, flush=True)


def allowed(cmd):
    return any(len(p) == len(cmd) and all(a is None or a == b for a, b in zip(p, cmd)) for p in ALLOWED)


def run(cmd, timeout=15):
    if not allowed(cmd):
        raise RuntimeError(f"refusing to run {cmd}: not in ALLOWED")
    log("$ " + " ".join(cmd))
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        log(f"  TIMEOUT after {timeout} s")
        return None, ""
    out = (r.stdout + r.stderr).strip()
    for l in out.splitlines():
        log("  " + l)
    log(f"  (exit {r.returncode})")
    return r.returncode, out


def devices(tool):
    cmd = [tool, "devices"]
    assert allowed(cmd)
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=10).stdout
    except subprocess.TimeoutExpired:
        return []
    want = "fastboot" if tool == "fastboot" else "device"
    found = [l.split()[0] for l in out.splitlines() if len(l.split()) >= 2 and l.split()[1] == want]
    serials.update(found)
    return found


def finish(code, why):
    log(why)
    os.makedirs(os.path.join(ROOT, "logs"), exist_ok=True)
    path = os.path.join(ROOT, "logs", f"probe{ARGS.probe}-{STAMP}.txt")
    with open(path, "w") as f:
        f.write("\n".join(log_lines) + "\n")
    print(f"\nRunner log: {path}")
    if os.path.exists(PHONE_LOG):
        print(f"Phone log:  {PHONE_LOG}")
    print("Upload these to Claude.")
    sys.exit(code)


def get_image():
    name = f"zeus-probe{ARGS.probe}.img"
    built = os.path.join(ROOT, "shim", "out", name)
    prebuilt = os.path.join(ROOT, "prebuilt", name)
    img = prebuilt
    if ARGS.probe == LATEST and all(shutil.which(t) for t in ("clang", "ld.lld", "llvm-objcopy", "llvm-nm")):
        r = subprocess.run(["bash", os.path.join(ROOT, "shim", "build.sh")], capture_output=True, text=True)
        log("built locally" if r.returncode == 0 else "local build failed, using prebuilt: " + r.stderr.strip()[-300:])
        if r.returncode == 0:
            img = built
    data = open(img, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    log(f"image {os.path.relpath(img, ROOT)} {len(data)} bytes sha256 {sha}")
    want = open(prebuilt + ".sha256").read().split()[0]
    if sha != want:
        log(f"note: differs from prebuilt {want[:16]}... (other compiler version)")
    if data[:8] != b"ANDROID!":
        finish(1, "not a boot image, stopping")
    return img


def find_probe_tty():
    """/dev/ttyACMn of the USB device with product 'Zeus probe log'."""
    for p in glob.glob("/sys/bus/usb/devices/*/product"):
        try:
            if open(p).read().strip() != "Zeus probe log":
                continue
        except OSError:
            continue
        for t in glob.glob(os.path.join(os.path.dirname(p), "*", "tty", "ttyACM*")):
            return "/dev/" + os.path.basename(t)
    return None


def capture_serial(appear_by, deadline):
    """Save the phone's log until its USB device goes away (reset) or deadline."""
    dev = None
    while time.time() < appear_by and not dev:
        dev = find_probe_tty()
        if not dev:
            time.sleep(0.5)
    if not dev:
        log("phone USB serial port never appeared (no USB log this run)")
        return
    log(f"phone USB serial port: {dev}")
    out = open(PHONE_LOG, "wb")
    try:
        fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    except PermissionError:
        log(f"no permission for {dev}: reading it with sudo "
            "(to avoid this: sudo usermod -aG dialout $USER, or uucp on Arch, then log in again)")
        p = subprocess.Popen(["sudo", "sh", "-c", f"stty -F {dev} raw -echo && cat {dev}"], stdout=out)
        while time.time() < deadline and find_probe_tty():
            time.sleep(0.5)
        p.terminate()
        out.close()
        log(f"phone log saved: {os.path.getsize(PHONE_LOG)} bytes")
        return
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[3] &= ~termios.ECHO
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    os.set_blocking(fd, False)
    total = 0
    while time.time() < deadline:
        try:
            chunk = os.read(fd, 65536)
        except BlockingIOError:
            chunk = b""
        except OSError:
            break		# device gone: the phone reset
        if chunk:
            out.write(chunk)
            out.flush()
            total += len(chunk)
            sys.stdout.write(chunk.decode(errors="replace"))
            sys.stdout.flush()
        else:
            if not find_probe_tty():
                break
            time.sleep(0.05)
    os.close(fd)
    out.close()
    log(f"phone log saved: {total} bytes")


def main():
    log(f"ios-zeus probe {ARGS.probe} runner, repo {ROOT}")
    rev = subprocess.run(["git", "-C", ROOT, "log", "-1", "--format=%h %s"], capture_output=True, text=True).stdout.strip()
    log(f"git: {rev}")
    for tool in ("fastboot", "adb"):
        if not shutil.which(tool):
            finish(1, f"{tool} not found: install android-tools / platform-tools")
    img = get_image()

    fb = devices("fastboot")
    if not fb:
        ad = devices("adb")
        if len(ad) > 1:
            finish(1, f"{len(ad)} adb devices connected: unplug the others")
        if len(ad) == 1:
            run(["adb", "-s", ad[0], "reboot", "bootloader"])
            for _ in range(60):
                time.sleep(1)
                fb = devices("fastboot")
                if fb:
                    break
    if len(fb) != 1:
        finish(1, f"need exactly one fastboot device, found {len(fb)}. "
                  "Put the phone in fastboot (Power + Volume Down) and run again")
    sn = fb[0]

    rc, out = run(["fastboot", "-s", sn, "getvar", "product"])
    if rc is None:
        finish(1, "fastboot hung. Hold Power + Volume Down to re-enter fastboot, then retry")
    m = re.search(r"product:\s*(\S+)", out)
    if not m or m.group(1) != "zeus":
        finish(1, f"product is {m.group(1) if m else 'unknown'}, not zeus: stopping")
    rc, out = run(["fastboot", "-s", sn, "getvar", "unlocked"])
    m = re.search(r"unlocked:\s*(\S+)", out)
    if not m or m.group(1) != "yes":
        finish(1, "bootloader does not report 'unlocked: yes', stopping")

    t_boot = time.time()
    rc, out = run(["fastboot", "-s", sn, "boot", img], timeout=90)
    if rc != 0:
        finish(1, "fastboot boot failed (see above). Nothing was changed on the phone")
    log("probe is running on the phone (resets by itself after ~120 s)")

    if ARGS.probe >= 2:
        capture_serial(t_boot + 130, t_boot + 200)

    log("waiting for Android (adb) or 5 minutes")
    state = None
    while time.time() - t_boot < 300:
        now = "fastboot" if devices("fastboot") else "android" if devices("adb") else "none"
        if now != state:
            log(f"USB state: {now} at +{time.time() - t_boot:.1f} s after boot")
            state = now
        if now == "android":
            break
        time.sleep(1)
    finish(0, "done" if state == "android" else "done (Android/adb not seen: fine if USB debugging is off)")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", type=int, choices=(1, 2, 3), default=3)
    ARGS = ap.parse_args()
    PHONE_LOG = os.path.join(ROOT, "logs", f"probe{ARGS.probe}-{STAMP}-phone.txt")
    os.makedirs(os.path.join(ROOT, "logs"), exist_ok=True)
    main()
