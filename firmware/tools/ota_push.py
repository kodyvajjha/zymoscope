#!/usr/bin/env python3
"""Push a firmware build to a Zymoscope ESP32 over the air.

Serves build/zymoscope.bin from this machine over HTTP, tells the device
to fetch it via MQTT ({"ota_url": ...} on zymoscope/<id>/cmd), then follows
progress on zymoscope/<id>/status until the device comes back online
running the new version (or rolls back / fails).

Requires only the Python stdlib plus the mosquitto clients
(mosquitto_pub / mosquitto_sub).

Usage:
    idf.py build
    python tools/ota_push.py                     # auto-detect the device
    python tools/ota_push.py --device a1b2c3d4e5f6 --broker 10.0.0.102
"""

from __future__ import annotations

import argparse
import functools
import http.server
import json
import queue
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

FW_DIR = Path(__file__).resolve().parent.parent
DEFAULT_BIN = FW_DIR / "build" / "zymoscope.bin"
APP_DESC_MAGIC = 0xABCD5432


def read_app_version(path: Path) -> tuple[str, str]:
    """Return (version, project_name) from an ESP-IDF app image."""
    d = path.read_bytes()[:0x100]
    if len(d) < 0x70 or struct.unpack_from("<I", d, 0x20)[0] != APP_DESC_MAGIC:
        sys.exit(f"{path} doesn't look like an ESP-IDF app image")
    ver = d[0x30:0x50].split(b"\0")[0].decode()
    proj = d[0x50:0x70].split(b"\0")[0].decode()
    return ver, proj


def local_ip_towards(host: str) -> str:
    """IP of the interface this machine uses to reach *host*."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, 1883))
        return s.getsockname()[0]
    finally:
        s.close()


def mqtt_sub(broker: str, port: int, topic: str, out: queue.Queue,
             timeout: float | None = None) -> subprocess.Popen:
    """Run mosquitto_sub in the background, feeding (topic, payload) to *out*."""
    cmd = ["mosquitto_sub", "-h", broker, "-p", str(port), "-t", topic, "-v"]
    if timeout:
        cmd += ["-W", str(int(timeout))]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, bufsize=1,
                            stderr=subprocess.DEVNULL if timeout else None)

    def pump() -> None:
        for line in proc.stdout:  # type: ignore[union-attr]
            topic_, _, payload = line.rstrip("\n").partition(" ")
            out.put((topic_, payload))
        out.put((None, None))

    threading.Thread(target=pump, daemon=True).start()
    return proc


def detect_device(broker: str, port: int) -> str:
    """Find device ids from retained status, falling back to live telemetry."""
    q: queue.Queue = queue.Queue()
    print("Looking for devices (retained status, then up to 40 s of telemetry)...")
    found: set[str] = set()
    for topic, wait in (("zymoscope/+/status", 2), ("zymoscope/+/telemetry", 40)):
        proc = mqtt_sub(broker, port, topic, q, timeout=wait)
        deadline = time.time() + wait + 1
        while time.time() < deadline:
            try:
                t, payload = q.get(timeout=0.5)
            except queue.Empty:
                continue
            if t is None:
                break
            dev = t.split("/")[1]
            if "/status" in t and '"offline"' in (payload or ""):
                continue
            found.add(dev)
            if "/telemetry" in t:
                break
        proc.terminate()
        if found:
            break
    if not found:
        sys.exit("No online Zymoscope device found — is it powered and on Wi-Fi?")
    if len(found) > 1:
        sys.exit(f"Several devices found ({', '.join(sorted(found))}); pass --device")
    return found.pop()


class _BinHandler(http.server.BaseHTTPRequestHandler):
    def __init__(self, *args, image: bytes, served: threading.Event, **kw):
        self.image = image
        self.served = served
        super().__init__(*args, **kw)

    def do_GET(self) -> None:  # noqa: N802
        if self.path != "/zymoscope.bin":
            self.send_error(404)
            return
        print(f"  [http] {self.client_address[0]} downloading "
              f"{len(self.image)} bytes")
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(self.image)))
        self.end_headers()
        try:
            self.wfile.write(self.image)
            self.served.set()
        except (BrokenPipeError, ConnectionResetError):
            print("  [http] download interrupted")

    def log_message(self, *args) -> None:  # silence default access log
        pass


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--broker", default="10.0.0.102", help="MQTT broker host")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--device", help="device id (MAC hex); auto-detected if omitted")
    ap.add_argument("--bin", type=Path, default=DEFAULT_BIN, help="firmware image")
    ap.add_argument("--http-port", type=int, default=8070,
                    help="port to serve the image on (must be reachable by the ESP32)")
    ap.add_argument("--timeout", type=int, default=420,
                    help="seconds to wait for the whole update")
    args = ap.parse_args()

    if not args.bin.exists():
        sys.exit(f"{args.bin} not found — run `idf.py build` first")
    new_ver, project = read_app_version(args.bin)
    if project != "zymoscope":
        sys.exit(f"Image project is '{project}', not 'zymoscope'")

    device = args.device or detect_device(args.broker, args.mqtt_port)
    ip = local_ip_towards(args.broker)
    url = f"http://{ip}:{args.http_port}/zymoscope.bin"
    image = args.bin.read_bytes()
    print(f"Device:   {device}")
    print(f"Image:    {args.bin} ({len(image)} bytes, version {new_ver})")
    print(f"Serving:  {url}")

    served = threading.Event()
    handler = functools.partial(_BinHandler, image=image, served=served)
    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", args.http_port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    q: queue.Queue = queue.Queue()
    sub = mqtt_sub(args.broker, args.mqtt_port, f"zymoscope/{device}/status", q)
    # Wait for the retained "online" status so we know which slot is running
    # now; that's how a rollback is told apart from success when the version
    # string doesn't change (e.g. two "-dirty" builds).
    old_ver = old_part = None
    wait_until = time.time() + 5
    while time.time() < wait_until and old_part is None:
        try:
            _, payload = q.get(timeout=0.25)
        except queue.Empty:
            continue
        try:
            msg = json.loads(payload)
        except (TypeError, json.JSONDecodeError):
            continue
        if msg.get("event") == "online":
            old_ver, old_part = msg.get("fw"), msg.get("partition")
            print(f"Current:  {old_ver} on {old_part} "
                  f"({msg.get('ota_state')})")
    if old_part is None:
        print("Current:  unknown (no retained status; device may be running "
              "pre-OTA firmware)")

    subprocess.run(
        ["mosquitto_pub", "-h", args.broker, "-p", str(args.mqtt_port), "-q", "1",
         "-t", f"zymoscope/{device}/cmd", "-m", json.dumps({"ota_url": url})],
        check=True,
    )
    print("OTA command sent; waiting for the device...")

    rc = 1
    rebooting = False
    acked = warned = False
    sent_at = time.time()
    deadline = sent_at + args.timeout
    try:
        while time.time() < deadline:
            try:
                _, payload = q.get(timeout=1)
            except queue.Empty:
                if not acked and not warned and time.time() > sent_at + 20:
                    print("No response in 20 s. Is the device online and running "
                          "OTA-capable firmware? (The first OTA build must be "
                          "flashed over USB.)")
                    warned = True
                continue
            if payload is None:
                print("mosquitto_sub exited unexpectedly")
                break
            try:
                msg = json.loads(payload)
            except json.JSONDecodeError:
                continue
            ev, state = msg.get("event"), msg.get("state")
            if ev == "ota":
                acked = True
                detail = f" {msg['detail']}" if msg.get("detail") else ""
                print(f"  [device] {state} {msg.get('progress', '')}%{detail}")
                if state in ("failed", "rejected"):
                    break
                if state == "rebooting":
                    rebooting = True
            elif ev == "offline" and rebooting:
                print("  [device] offline (rebooting)")
            elif ev == "online" and rebooting:
                fw, part = msg.get("fw"), msg.get("partition")
                print(f"  [device] online: {fw} on {part} "
                      f"({msg.get('ota_state')})")
                if msg.get("ota_state") == "pending_verify" and part != old_part:
                    print("  New image booted; waiting ~2 min for it to verify "
                          "(rolls back automatically if it misbehaves)...")
                    continue
                # A rollback comes back on the old slot; a good update on the
                # other one (versions alone can match, e.g. two "-dirty" builds).
                switched = old_part is None or part != old_part
                if fw == new_ver and switched and msg.get("ota_state") == "valid":
                    print("Update successful.")
                    rc = 0
                else:
                    print("Device came back on its previous image — the update "
                          "rolled back. Check the serial log for why it didn't verify.")
                break
        else:
            print("Timed out waiting for the device. If it booted the new image "
                  "but can't reach MQTT, it will roll back on its own within 5 min.")
    finally:
        sub.terminate()
        httpd.shutdown()
    return rc


if __name__ == "__main__":
    sys.exit(main())
