# E2E Test Results — Phase 6 (Consumer API)

**Date:** 2026-04-08  
**Target:** Raspberry Pi 3B+ (`raspberrypi3`)  
**Kernel:** `Linux raspberrypi3 6.6.63-v7 #1 SMP armv7l`  
**Daemon binary:** `/usr/sbin/wifi-offload-manager` (built 2026-04-08, Release)  
**Test binary:** `/usr/sbin/mock_consumer` (built 2026-04-08)  
**Config:** `/etc/netservice/path-policies.json` (3 path classes: multipath, lte_b2c, lte_b2b)

---

## Test Environment

| Component | Detail |
|---|---|
| SoC | BCM2837 (Cortex-A53 ARMv7), 1 GB RAM |
| OS | Yocto Scarthgap 5.0 (Poky) |
| Kernel | 6.6.63-v7 (`linux-raspberrypi`) |
| Network | 3× VLAN subinterfaces: `eth0.100/200/300` |
| Socket | `/var/run/netservice/control.sock` (Unix domain) |
| cgroup | `/sys/fs/cgroup/net_cls,net_prio/{multipath,lte_b2c,lte_b2b}` |

### Network Topology

```
Host enp2s0.100 172.16.1.1/24  ←→  RPi eth0.100 172.16.1.2/24  (multipath)
Host enp2s0.200 172.16.2.1/24  ←→  RPi eth0.200 172.16.2.2/24  (lte_b2c)
Host enp2s0.300 172.16.3.1/24  ←→  RPi eth0.300 172.16.3.2/24  (lte_b2b)
```

### Pre-test: Daemon Startup (clean)

```
wifi-offload-manager[9570]: [INF] [MAIN] wifi-offload-manager starting
wifi-offload-manager[9570]: [INF] [MAIN] config path: /etc/netservice/path-policies.json
wifi-offload-manager[9570]: [INF] [CONFIG] loaded 3 path class(es): multipath, lte_b2c, lte_b2b
wifi-offload-manager[9570]: [INF] [ROUTING] creating cgroup net_cls hierarchy (3 class(es))
wifi-offload-manager[9570]: [INF] [ROUTING] cgroup created: id=multipath real_path=/sys/fs/cgroup/net_cls,net_prio/multipath classid=0x00100001
wifi-offload-manager[9570]: [INF] [ROUTING] cgroup created: id=lte_b2c real_path=/sys/fs/cgroup/net_cls,net_prio/lte_b2c classid=0x00100002
wifi-offload-manager[9570]: [INF] [ROUTING] cgroup created: id=lte_b2b real_path=/sys/fs/cgroup/net_cls,net_prio/lte_b2b classid=0x00100003
wifi-offload-manager[9570]: [INF] [ROUTING] cgroup hierarchy ready
wifi-offload-manager[9570]: [INF] [ROUTING] setting up iptables mangle MARK rules (3 class(es))
wifi-offload-manager[9570]: [INF] [API] server started: socket=/var/run/netservice/control.sock
wifi-offload-manager[9570]: [INF] [MAIN] startup complete — entering event loop
```

---

## Test Results

### T1 — Register `multipath` class, query current state

**Command:**
```
mock_consumer --class multipath --events 0 --socket /var/run/netservice/control.sock
```
**Output:**
```
[mock_consumer] socket=/var/run/netservice/control.sock class=multipath maxEvents=0
[mock_consumer] connected
[mock_consumer] registered: handle=1 class=multipath initial_state=Idle
[mock_consumer] 0 events received
[mock_consumer] done
```
**Result:** ✅ PASS — registered with handle=1, initial state=`Idle`, clean exit (0)

---

### T2 — Register `lte_b2c` class, query current state

**Command:**
```
mock_consumer --class lte_b2c --events 0 --socket /var/run/netservice/control.sock
```
**Output:**
```
[mock_consumer] socket=/var/run/netservice/control.sock class=lte_b2c maxEvents=0
[mock_consumer] connected
[mock_consumer] registered: handle=2 class=lte_b2c initial_state=Idle
[mock_consumer] 0 events received
[mock_consumer] done
```
**Result:** ✅ PASS — registered with handle=2, initial state=`Idle`, clean exit (0)

---

### T3 — Register `lte_b2b` class, query current state

**Command:**
```
mock_consumer --class lte_b2b --events 0 --socket /var/run/netservice/control.sock
```
**Output:**
```
[mock_consumer] socket=/var/run/netservice/control.sock class=lte_b2b maxEvents=0
[mock_consumer] connected
[mock_consumer] registered: handle=3 class=lte_b2b initial_state=Idle
[mock_consumer] 0 events received
[mock_consumer] done
```
**Result:** ✅ PASS — registered with handle=3, initial state=`Idle`, clean exit (0)

---

### T4 — Unknown class rejected with error

**Command:**
```
mock_consumer --class invalid_class --events 0 --socket /var/run/netservice/control.sock
```
**Output:**
```
[mock_consumer] socket=/var/run/netservice/control.sock class=invalid_class maxEvents=0
[mock_consumer] connected
[mock_consumer] Register failed: error=2
```
**Daemon log:**
```
wifi-offload-manager[9570]: [WRN] [API] Register: unknown classId='invalid_class' from fd=7
```
**Result:** ✅ PASS — daemon returned error code 2, consumer exit code 1 (non-zero), no crash

---

### T5 — Concurrent registration (3 consumers simultaneously)

**Command:**
```bash
mock_consumer --class multipath --events 0 ... &
mock_consumer --class lte_b2c  --events 0 ... &
mock_consumer --class lte_b2b  --events 0 ... &
wait
```
**Output:**
```
[mock_consumer] registered: handle=4 class=lte_b2c initial_state=Idle  → exit=0
[mock_consumer] registered: handle=5 class=lte_b2b initial_state=Idle  → exit=0
[mock_consumer] registered: handle=6 class=multipath initial_state=Idle → exit=0
```
**Daemon log (excerpt):**
```
wifi-offload-manager[9570]: [INF] [API] assigned pid=9626 to cgroup /sys/fs/cgroup/net_cls/lte_b2c
wifi-offload-manager[9570]: [INF] [API] consumer registered: pid=9626 class=lte_b2c handle=4 fd=7
wifi-offload-manager[9570]: [INF] [API] assigned pid=9627 to cgroup /sys/fs/cgroup/net_cls/lte_b2b
wifi-offload-manager[9570]: [INF] [API] consumer registered: pid=9627 class=lte_b2b handle=5 fd=8
wifi-offload-manager[9570]: [INF] [API] assigned pid=9625 to cgroup /sys/fs/cgroup/net_cls/multipath
wifi-offload-manager[9570]: [INF] [API] consumer registered: pid=9625 class=multipath handle=6 fd=9
wifi-offload-manager[9570]: [INF] [API] moved pid=9626 back to cgroup root (was /sys/fs/cgroup/net_cls/lte_b2c)
wifi-offload-manager[9570]: [INF] [API] consumer unregistered: handle=4 fd=7
wifi-offload-manager[9570]: [INF] [API] moved pid=9627 back to cgroup root (was /sys/fs/cgroup/net_cls/lte_b2b)
wifi-offload-manager[9570]: [INF] [API] consumer unregistered: handle=5 fd=8
wifi-offload-manager[9570]: [INF] [API] moved pid=9625 back to cgroup root (was /sys/fs/cgroup/net_cls/multipath)
wifi-offload-manager[9570]: [INF] [API] consumer unregistered: handle=6 fd=9
```
**Result:** ✅ PASS — all 3 consumers registered concurrently with unique handles, all cgroup assignments and cleanup correct

---

### T6 — Daemon survives abrupt consumer disconnect (`SIGKILL`)

**Command:**
```bash
mock_consumer --class multipath --events 999 ... &
sleep 1
kill -9 $MC_PID
sleep 1
systemctl is-active wifi-offload-manager
```
**Daemon log:**
```
wifi-offload-manager[9570]: [INF] [API] consumer unregistered on disconnect: pid=9628 class=multipath fd=7
wifi-offload-manager[9570]: [INF] [API] moved pid=9628 back to cgroup root (was /sys/fs/cgroup/net_cls/multipath)
```
**Result:** ✅ PASS — daemon detected peer disconnect, cleaned up cgroup assignment, remained `active`

---

### T7 — Clean unregister (graceful disconnect)

**Command:**
```
mock_consumer --class multipath --events 0 --socket /var/run/netservice/control.sock
```
**Output:**
```
[mock_consumer] registered: handle=8 class=multipath initial_state=Idle
[mock_consumer] 0 events received
[mock_consumer] done
```
**Daemon log:**
```
wifi-offload-manager[9570]: [INF] [API] assigned pid=9631 to cgroup /sys/fs/cgroup/net_cls/multipath
wifi-offload-manager[9570]: [INF] [API] consumer registered: pid=9631 class=multipath handle=8 fd=7
wifi-offload-manager[9570]: [INF] [API] moved pid=9631 back to cgroup root (was /sys/fs/cgroup/net_cls/multipath)
wifi-offload-manager[9570]: [INF] [API] consumer unregistered: handle=8 fd=7
```
**Result:** ✅ PASS — cgroup assigned on register, restored to root on clean disconnect

---

## Summary

| # | Test Case | Result |
|---|---|---|
| T1 | Register `multipath`, query state | ✅ PASS |
| T2 | Register `lte_b2c`, query state | ✅ PASS |
| T3 | Register `lte_b2b`, query state | ✅ PASS |
| T4 | Unknown class → error returned | ✅ PASS |
| T5 | 3 concurrent consumers | ✅ PASS |
| T6 | Daemon survives `SIGKILL` disconnect | ✅ PASS |
| T7 | Clean unregister (graceful disconnect) | ✅ PASS |

**7/7 PASS**

---

## Kernel State Verification

### cgroup Net-Cls Hierarchy

```
/sys/fs/cgroup/net_cls,net_prio/
├── multipath   classid=0x00100001
├── lte_b2c     classid=0x00100002
└── lte_b2b     classid=0x00100003
```

### IP Policy Rules

```
0:      from all lookup local
32765:  from all fwmark 0x10 lookup 100   ← multipath
32765:  from all fwmark 0x20 lookup 200   ← lte_b2c
32765:  from all fwmark 0x30 lookup 300   ← lte_b2b
32766:  from all lookup main
32767:  from all lookup default
```

### iptables mangle — MARK chain

```
Chain NETSERVICE-MARK (1 references)
  MARK  cgroup 1048577 (multipath)  → MARK set 0x10
  MARK  cgroup 1048578 (lte_b2c)   → MARK set 0x20
  MARK  cgroup 1048579 (lte_b2b)   → MARK set 0x30
```

### iptables filter — Isolation chain (strict_isolation=true for lte_b2b)

```
Chain NETSERVICE-ISO (1 references)
  DROP  out=eth0.100  mark=0x30   ← lte_b2b cannot use multipath iface
  DROP  out=eth0.200  mark=0x30   ← lte_b2b cannot use lte_b2c iface
  DROP  out=eth0.300  mark=0x10   ← multipath cannot use lte_b2b iface
  DROP  out=eth0.300  mark=0x20   ← lte_b2c cannot use lte_b2b iface
```

---

## Known Observations

- **WPA monitor retries**: `[WPA] ctrl_open failed: iface=eth0.X ... retry in Ns` — expected in test bench (no wpa_supplicant running on VLAN subinterfaces); daemon continues normally
- **iptables modules not persistent across reboot**: Fixed by writing `/etc/modules-load.d/netservice.conf`; also updated `setup-test-vlans.sh` to `modprobe` them on startup
- **RPi clock skew**: Journal timestamps show `May 30` (RPi RTC not set); does not affect test validity
