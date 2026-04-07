#!/usr/bin/env python3
"""
mptcp_server.py — MPTCP-capable HTTP file server for FOTA download testing.

Creates a 1.2 GB sparse firmware image, serves it on all three VLAN interfaces
(172.16.1.1, 172.16.2.1, 172.16.3.1) so the RPi MPTCP stack can use all three
paths simultaneously.

Requires kernel MPTCP support (net.mptcp.enabled=1) and the MPTCP path manager
to announce endpoints on the server side.

Usage:
    sudo python3 scripts/mptcp_server.py [--port PORT] [--size-gb SIZE]

The server MUST be started from the host machine with the VLAN interfaces up.
"""

import argparse
import hashlib
import http.server
import os
import socket
import struct
import subprocess
import sys
import threading
from pathlib import Path

FIRMWARE_PATH = Path("/tmp/firmware.img")
CHUNK         = 1 * 1024 * 1024   # 1 MB read chunks for serving

# ── MPTCP socket constant ─────────────────────────────────────────────────────
IPPROTO_MPTCP = 262    # Linux 5.6+
SOL_TCP       = 6

# ── Firmware image creation ───────────────────────────────────────────────────

def create_firmware_image(path: Path, size_gb: float) -> str:
    """Create a sparse firmware image with a deterministic pattern header."""
    size_bytes = int(size_gb * 1024 * 1024 * 1024)
    print(f"[SERVER] creating firmware image: {path}  size={size_bytes/1e9:.2f} GB", flush=True)

    with open(path, "wb") as f:
        # 4 KB header: magic + version + size
        header = b"FOTA_IMG_V1\x00" + struct.pack("<Q", size_bytes) + b"\x00" * (4096 - 20)
        f.write(header)
        # Sparse: seek to end and write a single byte to create a hole
        f.seek(size_bytes - 1)
        f.write(b"\xff")

    print(f"[SERVER] firmware image ready: {path.stat().st_size} bytes on disk "
          f"(sparse, actual={path.stat().st_blocks * 512} bytes allocated)", flush=True)

    # SHA-256 of first 4 KB + last byte (quick fingerprint, not full image)
    sha = hashlib.sha256()
    with open(path, "rb") as f:
        sha.update(f.read(4096))
        f.seek(-1, 2)
        sha.update(f.read(1))
    digest = sha.hexdigest()
    print(f"[SERVER] firmware fingerprint (sha256 head+tail): {digest}", flush=True)
    return digest


# ── MPTCP endpoint setup ──────────────────────────────────────────────────────

VLAN_IPS = ["172.16.1.1", "172.16.2.1", "172.16.3.1"]

def setup_mptcp_endpoints():
    """Add server-side MPTCP subflow endpoints for all VLAN IPs."""
    print("[SERVER] configuring MPTCP endpoints on host …", flush=True)

    # Flush existing endpoints to avoid duplicates
    subprocess.run(["ip", "mptcp", "endpoint", "flush"], check=False)

    for ip in VLAN_IPS:
        r = subprocess.run(
            ["ip", "mptcp", "endpoint", "add", ip, "signal", "id", str(VLAN_IPS.index(ip) + 1)],
            capture_output=True, text=True
        )
        if r.returncode == 0:
            print(f"[SERVER]   endpoint added: {ip} (signal)", flush=True)
        else:
            print(f"[SERVER]   WARNING: endpoint {ip}: {r.stderr.strip()}", flush=True)

    # Allow up to 3 additional subflows from the RPi
    subprocess.run(["ip", "mptcp", "limits", "set", "add_addr_accepted", "3", "subflows", "3"],
                   check=False)

    r = subprocess.run(["ip", "mptcp", "endpoint", "show"], capture_output=True, text=True)
    print(f"[SERVER] current MPTCP endpoints:\n{r.stdout}", flush=True)


# ── HTTP request handler ──────────────────────────────────────────────────────

class FotaHandler(http.server.BaseHTTPRequestHandler):
    firmware_path: Path = FIRMWARE_PATH
    log_lock = threading.Lock()

    def do_GET(self):
        if self.path not in ("/firmware.img", "/firmware.img.sha256"):
            self.send_error(404)
            return

        if self.path == "/firmware.img.sha256":
            body = (FotaHandler.sha256_digest + "\n").encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        size = self.firmware_path.stat().st_size
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.send_header("Content-Disposition", 'attachment; filename="firmware.img"')
        self.end_headers()

        sent = 0
        with open(self.firmware_path, "rb") as f:
            while True:
                chunk = f.read(CHUNK)
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    sent += len(chunk)
                except BrokenPipeError:
                    with self.log_lock:
                        print(f"\n[SERVER] client disconnected after {sent/1e6:.1f} MB "
                              f"(expected — path drop test)", flush=True)
                    return

        with self.log_lock:
            print(f"\n[SERVER] transfer complete: {sent/1e6:.1f} MB to "
                  f"{self.client_address[0]}", flush=True)

    def log_message(self, fmt, *args):
        with self.log_lock:
            print(f"[SERVER] {self.address_string()} - {fmt % args}", flush=True)


# ── MPTCP socket server ───────────────────────────────────────────────────────

class MptcpTCPServer(http.server.ThreadingHTTPServer):
    """Like ThreadingHTTPServer but binds on IPPROTO_MPTCP instead of TCP."""

    def server_bind(self):
        # Create an MPTCP socket; fall back to TCP if unavailable
        try:
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM, IPPROTO_MPTCP)
            print("[SERVER] listening on IPPROTO_MPTCP socket", flush=True)
        except OSError:
            print("[SERVER] WARNING: MPTCP socket unavailable, falling back to TCP", flush=True)
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind(self.server_address)
        self.server_address = self.socket.getsockname()


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port",    type=int,   default=8080)
    ap.add_argument("--size-gb", type=float, default=1.2,
                    help="Firmware image size in GB (default 1.2)")
    ap.add_argument("--bind",    default="0.0.0.0")
    ap.add_argument("--no-endpoints", action="store_true",
                    help="Skip MPTCP endpoint setup (already configured)")
    args = ap.parse_args()

    # Create firmware image if needed
    if not FIRMWARE_PATH.exists():
        digest = create_firmware_image(FIRMWARE_PATH, args.size_gb)
    else:
        print(f"[SERVER] reusing existing firmware image: {FIRMWARE_PATH} "
              f"({FIRMWARE_PATH.stat().st_size/1e9:.2f} GB)", flush=True)
        sha = hashlib.sha256()
        with open(FIRMWARE_PATH, "rb") as f:
            sha.update(f.read(4096))
            f.seek(-1, 2)
            sha.update(f.read(1))
        digest = sha.hexdigest()

    FotaHandler.sha256_digest = digest
    FotaHandler.firmware_path = FIRMWARE_PATH

    # Configure MPTCP endpoints
    if not args.no_endpoints:
        setup_mptcp_endpoints()

    # Start server
    server = MptcpTCPServer((args.bind, args.port), FotaHandler)
    print(f"[SERVER] FOTA HTTP server listening on {args.bind}:{args.port}", flush=True)
    print(f"[SERVER] firmware URL: http://<host_ip>:{args.port}/firmware.img", flush=True)
    print(f"[SERVER] SHA-256 URL:  http://<host_ip>:{args.port}/firmware.img.sha256", flush=True)
    print("[SERVER] Ctrl+C to stop\n", flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[SERVER] shutting down …", flush=True)
        server.shutdown()


if __name__ == "__main__":
    main()
