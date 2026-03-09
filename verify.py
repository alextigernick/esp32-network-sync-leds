#!/usr/bin/env python3
"""
Verify OTA firmware on a self-sync-leds node.

Usage:
    python verify.py <ip>

Calls POST /ota/verify on the node, marking the running firmware as valid
and cancelling the rollback window. If not called after an OTA update, the
bootloader will roll back to the previous firmware on next reboot.
"""

import sys
import urllib.request
import urllib.error


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    ip = sys.argv[1]
    url = f"http://{ip}/ota/verify"
    print(f"Verifying → POST {url}")

    try:
        req = urllib.request.Request(url, data=b"", method="POST")
        with urllib.request.urlopen(req, timeout=10) as resp:
            print(resp.read().decode(errors="replace"))
    except urllib.error.HTTPError as e:
        print(f"HTTP error {e.code}: {e.read().decode(errors='replace')}")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
