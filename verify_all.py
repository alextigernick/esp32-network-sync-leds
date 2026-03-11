#!/usr/bin/env python3
"""
Verify OTA firmware on all nodes discovered through one seed node.

Usage:
    python verify_all.py <seed-ip>

Queries GET http://<seed-ip>/state to discover all peers, then calls
POST /ota/verify on the seed node and every peer in parallel, marking
the running firmware as valid and cancelling the rollback window.
"""

import json
import sys
import threading
import urllib.error
import urllib.request


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


def verify_node(ip: str, name: str) -> bool:
    """POST /ota/verify to commit the currently running firmware."""
    label = f"{name} ({ip})"
    try:
        req = urllib.request.Request(f"http://{ip}/ota/verify", data=b"", method="POST")
        with urllib.request.urlopen(req, timeout=10) as resp:
            print(f"[ok]   {label} — {resp.read().decode(errors='replace').strip()}")
            return True
    except urllib.error.HTTPError as e:
        print(f"[fail] {label} — HTTP {e.code}: {e.read().decode(errors='replace').strip()}")
        return False
    except Exception as e:
        print(f"[fail] {label} — {e}")
        return False


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    seed_ip = sys.argv[1]

    peers = get_peers(seed_ip)

    peer_ips = {p["ip"] for p in peers}
    targets = [{"name": "seed", "ip": seed_ip}] if seed_ip not in peer_ips else []
    targets += peers

    if not targets:
        print("No nodes found.")
        sys.exit(1)

    print(f"Verifying {len(targets)} node(s):")
    for t in targets:
        print(f"  {t['name']} — {t['ip']}")
    print()

    results: dict[str, bool] = {}
    lock = threading.Lock()

    def worker(node):
        ok = verify_node(node["ip"], node["name"])
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

    sys.exit(0 if fail_count == 0 else 1)


if __name__ == "__main__":
    main()
