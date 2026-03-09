#!/usr/bin/env python3
"""
OTA flash all nodes discovered through one seed node.

Usage:
    python flash_all.py <seed-ip>
    python flash_all.py <seed-ip> [path/to/firmware.bin]

The script queries GET http://<seed-ip>/state to discover all peers, then
flashes the seed node itself plus every peer in parallel.

After flashing, run verify.py <ip> on each node to make the new firmware
permanent. Without verification the bootloader will roll back on next reboot.
"""

import json
import os
import sys
import threading
import urllib.error
import urllib.request

FIRMWARE_DEFAULT = os.path.join(
    os.path.dirname(__file__),
    ".pio", "build", "seeed_xiao_esp32c3", "firmware.bin"
)


def get_peers(seed_ip: str) -> list[dict]:
    """Query /state on the seed node and return the peers list."""
    url = f"http://{seed_ip}/state"
    try:
        with urllib.request.urlopen(url, timeout=10) as resp:
            data = json.loads(resp.read())
            return data.get("peers", [])
    except Exception as e:
        print(f"[error] Could not reach {seed_ip}/state: {e}")
        sys.exit(1)


def verify_node(ip: str, name: str) -> None:
    """POST /ota/verify to commit the currently running firmware before flashing a new one."""
    label = f"{name} ({ip})"
    try:
        req = urllib.request.Request(f"http://{ip}/ota/verify", data=b"", method="POST")
        with urllib.request.urlopen(req, timeout=10) as resp:
            print(f"[pre-verify ok]   {label} — {resp.read().decode(errors='replace').strip()}")
    except Exception as e:
        print(f"[pre-verify skip] {label} — {e}")


def flash_node(ip: str, name: str, fw_data: bytes) -> bool:
    """Upload firmware.bin to a single node via multipart POST /ota."""
    boundary = "----ESP32OTABoundary"
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

    label = f"{name} ({ip})"
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            print(f"[ok]    {label} — upload complete (HTTP {resp.status}), rebooting")
            return True
    except urllib.error.HTTPError as e:
        print(f"[fail]  {label} — HTTP {e.code}: {e.read().decode(errors='replace')}")
        return False
    except Exception as e:
        print(f"[fail]  {label} — {e}")
        return False


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    seed_ip = sys.argv[1]
    fw_path = sys.argv[2] if len(sys.argv) > 2 else FIRMWARE_DEFAULT

    if not os.path.exists(fw_path):
        print(f"Error: firmware not found at {fw_path}")
        print("Run 'pio run -e seeed_xiao_esp32c3' first.")
        sys.exit(1)

    with open(fw_path, "rb") as f:
        fw_data = f.read()
    print(f"Firmware: {fw_path} ({len(fw_data):,} bytes)")

    # Discover peers through the seed node
    peers = get_peers(seed_ip)

    # Build the full target list: seed node + all peers
    # Avoid duplicating the seed if it also appears in the peer list
    peer_ips = {p["ip"] for p in peers}
    targets = [{"name": "seed", "ip": seed_ip}] if seed_ip not in peer_ips else []
    targets += peers

    if not targets:
        print("No nodes found.")
        sys.exit(1)

    print(f"Flashing {len(targets)} node(s):")
    for t in targets:
        print(f"  {t['name']} — {t['ip']}")
    print()

    results: dict[str, bool] = {}
    lock = threading.Lock()

    def worker(node):
        verify_node(node["ip"], node["name"])
        ok = flash_node(node["ip"], node["name"], fw_data)
        with lock:
            results[node["ip"]] = ok

    threads = [threading.Thread(target=worker, args=(t,), daemon=True) for t in targets]
    for th in threads:
        th.start()
    for th in threads:
        th.join()

    ok_count = sum(results.values())
    fail_count = len(results) - ok_count
    print(f"\nDone: {ok_count} succeeded, {fail_count} failed.")

    if ok_count:
        ips = " ".join(ip for ip, ok in results.items() if ok)
        print(f"Run 'python verify.py <ip>' on each node once you confirm it's working.")

    sys.exit(0 if fail_count == 0 else 1)


if __name__ == "__main__":
    main()
