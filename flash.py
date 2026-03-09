#!/usr/bin/env python3
"""
OTA flash script for self-sync-leds nodes.

Usage:
    python flash.py <ip>
    python flash.py <ip> [path/to/firmware.bin]

Default firmware path: .pio/build/seeed_xiao_esp32c3/firmware.bin

After flashing, run verify.py <ip> to mark the new firmware as permanent.
If verify.py is not called, the bootloader will roll back on next reboot.
"""

import sys
import os
import urllib.request
import urllib.error

FIRMWARE_DEFAULT = os.path.join(
    os.path.dirname(__file__),
    ".pio", "build", "seeed_xiao_esp32c3", "firmware.bin"
)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    ip = sys.argv[1]
    fw_path = sys.argv[2] if len(sys.argv) > 2 else FIRMWARE_DEFAULT

    if not os.path.exists(fw_path):
        print(f"Error: firmware not found at {fw_path}")
        print("Run 'pio run -e seeed_xiao_esp32c3' first.")
        sys.exit(1)

    fw_size = os.path.getsize(fw_path)
    print(f"Flashing {fw_path} ({fw_size} bytes) → http://{ip}/ota")

    boundary = "----ESP32OTABoundary"
    with open(fw_path, "rb") as f:
        fw_data = f.read()

    body = (
        f"--{boundary}\r\n"
        f"Content-Disposition: form-data; name=\"firmware\"; filename=\"firmware.bin\"\r\n"
        f"Content-Type: application/octet-stream\r\n"
        f"\r\n"
    ).encode() + fw_data + f"\r\n--{boundary}--\r\n".encode()

    req = urllib.request.Request(
        f"http://{ip}/ota",
        data=body,
        method="POST",
    )
    req.add_header("Content-Type", f"multipart/form-data; boundary={boundary}")
    req.add_header("Content-Length", str(len(body)))

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            print(f"Upload OK ({resp.status}). Node is rebooting...")
            print(f"Run 'python verify.py {ip}' once you confirm it's working.")
    except urllib.error.HTTPError as e:
        print(f"HTTP error {e.code}: {e.read().decode(errors='replace')}")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
