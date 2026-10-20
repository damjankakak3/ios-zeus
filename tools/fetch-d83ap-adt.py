#!/usr/bin/env python3
"""Fetch the iPhone 15 Pro (D83AP) device tree from an iOS 26 IPSW, nothing else.

  python3 tools/fetch-d83ap-adt.py            -> research/ipsw/d83ap.adt (not in git)

Asks ipsw.me for the iPhone16,1 firmware list, picks the newest iOS 26.x IPSW
and pulls only Firmware/all_flash/DeviceTree.d83ap.im4p out of it with HTTP
range requests (tools/remotezip.py; a few hundred KB, not the 8 GB IPSW).
tools/mkadt.py copies its /product node. Optional: without it mkadt only sets
the identity properties.
"""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import adt, remotezip  # noqa: E402

OUT_DIR = os.path.join(ROOT, 'research', 'ipsw')
MEMBER = 'DeviceTree.d83ap.im4p'


def main():
    info = json.loads(subprocess.run(['curl', '-sfL', 'https://api.ipsw.me/v4/device/iPhone16,1?type=ipsw'],
                                     check=True, capture_output=True).stdout)
    fws = [f for f in info['firmwares'] if f['version'].startswith('26.')]
    if not fws:
        sys.exit('no iOS 26 IPSW listed for iPhone16,1')
    fw = max(fws, key=lambda f: [int(x) for x in f['version'].split('.')])
    print(f"iOS {fw['version']} ({fw['buildid']}): {fw['url']}")
    entries = [e for e in remotezip.central_dir(fw['url']) if e[0].endswith(MEMBER)]
    if not entries:
        sys.exit(f'{MEMBER} not in the IPSW')
    os.makedirs(OUT_DIR, exist_ok=True)
    remotezip.extract(fw['url'], entries[0], OUT_DIR)
    raw = adt.load(os.path.join(OUT_DIR, entries[0][0]))
    out = os.path.join(OUT_DIR, 'd83ap.adt')
    open(out, 'wb').write(raw)
    root, _ = adt.parse(raw)
    print(f'{out}: {len(raw)} bytes, root compatible {root.prop("compatible")[:40]!r}')


if __name__ == '__main__':
    main()
