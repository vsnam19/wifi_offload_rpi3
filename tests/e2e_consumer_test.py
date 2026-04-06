#!/usr/bin/env python3
# e2e_consumer_test.py — P6-T1: End-to-end test for ConsumerApiServer
#
# Run on RPi while wifi-offload-manager is active:
#   python3 /tmp/e2e_consumer_test.py
#
# Tests:
#   1. Connect to /var/run/netservice/control.sock
#   2. Register for each known class → verify RegisterAck + valid handle
#   3. QueryCurrent for each class → verify QueryAck + state field
#   4. Unregister → verify UnregisterAck
#   5. Re-connect and close without unregistering → daemon must not crash

import socket
import struct
import sys
import os

SOCK_PATH = "/var/run/netservice/control.sock"

# ApiMsg layout (96 bytes, host byte order):
# uint32 type, uint32 error, uint64 handle,
# int32 pid, int32 rssi, uint32 state, uint32 pad,
# char classId[32], char iface[32]
MSG_FMT  = "=IIQiiII32s32s"
MSG_SIZE = struct.calcsize(MSG_FMT)
assert MSG_SIZE == 96, f"unexpected MSG_SIZE={MSG_SIZE}"

MSGTYPE_REGISTER       = 1
MSGTYPE_REGISTER_ACK   = 2
MSGTYPE_UNREGISTER     = 3
MSGTYPE_UNREGISTER_ACK = 4
MSGTYPE_QUERY_CURRENT  = 5
MSGTYPE_QUERY_ACK      = 6
MSGTYPE_PATH_EVENT     = 7

PATH_STATE_NAMES = {0: "Idle", 1: "Scanning", 2: "Connecting",
                    3: "PathUp", 4: "PathDegraded", 5: "PathDown"}

KNOWN_CLASSES = ["multipath", "lte_b2c", "lte_b2b"]

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

failures = 0


def pack_msg(type_, error=0, handle=0, pid=0, rssi=0, state=0, pad=0,
             class_id=b"", iface=b""):
    return struct.pack(MSG_FMT, type_, error, handle, pid, rssi, state, pad,
                       class_id.ljust(32, b'\x00')[:32],
                       iface.ljust(32, b'\x00')[:32])


def unpack_msg(data):
    t, err, handle, pid, rssi, state, pad, class_raw, iface_raw = struct.unpack(MSG_FMT, data)
    class_id = class_raw.rstrip(b'\x00').decode()
    iface    = iface_raw.rstrip(b'\x00').decode()
    return dict(type=t, error=err, handle=handle, pid=pid, rssi=rssi,
                state=state, class_id=class_id, iface=iface)


def send_recv(sock, msg_bytes):
    sock.sendall(msg_bytes)
    data = b""
    while len(data) < MSG_SIZE:
        chunk = sock.recv(MSG_SIZE - len(data))
        if not chunk:
            raise ConnectionError("server closed connection")
        data += chunk
    return unpack_msg(data)


def check(label, cond, actual=""):
    global failures
    if cond:
        print(f"  [{PASS}] {label}")
    else:
        print(f"  [{FAIL}] {label}  actual={actual}")
        failures += 1


def connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK_PATH)
    return s


# ── Test 1: socket accessible ─────────────────────────────────────────────────
print("\n=== Test 1: daemon socket accessible ===")
check("socket path exists", os.path.exists(SOCK_PATH),
      f"path={SOCK_PATH}")
try:
    s = connect()
    s.close()
    check("connect() succeeds", True)
except Exception as ex:
    check("connect() succeeds", False, str(ex))
    sys.exit(f"FATAL: cannot reach daemon. Is wifi-offload-manager running? {ex}")

# ── Test 2: Register for each known class ────────────────────────────────────
print("\n=== Test 2: Register for all path classes ===")
handles = {}
for cls in KNOWN_CLASSES:
    s = connect()
    req = pack_msg(MSGTYPE_REGISTER, pid=os.getpid(),
                   class_id=cls.encode())
    ack = send_recv(s, req)
    check(f"Register {cls}: type=RegisterAck",
          ack["type"] == MSGTYPE_REGISTER_ACK, str(ack["type"]))
    check(f"Register {cls}: error=0", ack["error"] == 0, str(ack["error"]))
    check(f"Register {cls}: handle>0", ack["handle"] > 0, str(ack["handle"]))
    check(f"Register {cls}: class_id matches",
          ack["class_id"] == cls, ack["class_id"])
    handles[cls] = (s, ack["handle"])

# ── Test 3: QueryCurrent for each class ──────────────────────────────────────
print("\n=== Test 3: QueryCurrent for all path classes ===")
for cls in KNOWN_CLASSES:
    s2 = connect()
    req = pack_msg(MSGTYPE_QUERY_CURRENT, class_id=cls.encode())
    ack = send_recv(s2, req)
    check(f"QueryCurrent {cls}: type=QueryAck",
          ack["type"] == MSGTYPE_QUERY_ACK, str(ack["type"]))
    check(f"QueryCurrent {cls}: error=0", ack["error"] == 0, str(ack["error"]))
    state_name = PATH_STATE_NAMES.get(ack["state"], f"?({ack['state']})")
    print(f"         state={state_name} iface='{ack['iface']}' rssi={ack['rssi']}dBm")
    s2.close()

# ── Test 4: Unregister ───────────────────────────────────────────────────────
print("\n=== Test 4: Unregister all ===")
for cls in KNOWN_CLASSES:
    s, h = handles[cls]
    req = pack_msg(MSGTYPE_UNREGISTER, handle=h)
    ack = send_recv(s, req)
    check(f"Unregister {cls}: type=UnregisterAck",
          ack["type"] == MSGTYPE_UNREGISTER_ACK, str(ack["type"]))
    check(f"Unregister {cls}: error=0", ack["error"] == 0, str(ack["error"]))
    check(f"Unregister {cls}: handle echoed", ack["handle"] == h, str(ack["handle"]))
    s.close()

# ── Test 5: Register unknown class returns error ──────────────────────────────
print("\n=== Test 5: Register unknown class → UnknownClassId error ===")
s = connect()
req = pack_msg(MSGTYPE_REGISTER, pid=os.getpid(), class_id=b"no_such_class")
ack = send_recv(s, req)
check("Unknown class: type=RegisterAck", ack["type"] == MSGTYPE_REGISTER_ACK, str(ack["type"]))
check("Unknown class: error!=0", ack["error"] != 0, str(ack["error"]))
s.close()

# ── Test 6: Abrupt disconnect — daemon must still run ────────────────────────
print("\n=== Test 6: Abrupt disconnect (no Unregister) ===")
for _ in range(3):
    s = connect()
    pack_msg(MSGTYPE_REGISTER, pid=os.getpid(), class_id=b"multipath")
    s.send(pack_msg(MSGTYPE_REGISTER, pid=os.getpid(), class_id=b"multipath"))
    s.close()  # close before reading ack — daemon must handle EPOLLHUP gracefully

import time
time.sleep(0.3)

# Verify daemon is still responsive after abrupt disconnects.
s = connect()
req = pack_msg(MSGTYPE_QUERY_CURRENT, class_id=b"multipath")
ack = send_recv(s, req)
check("Daemon still responds after abrupt disconnect",
      ack["type"] == MSGTYPE_QUERY_ACK, str(ack["type"]))
s.close()

# ── Summary ──────────────────────────────────────────────────────────────────
print("")
if failures == 0:
    print(f"\033[32m✓ All tests passed.\033[0m")
else:
    print(f"\033[31m✗ {failures} test(s) FAILED.\033[0m")
    sys.exit(1)
