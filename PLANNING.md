# WiFi Offload Manager — Implementation Planning

> Claude Code reads this file automatically.
> Update "Active Tasks" and checkbox status after each completed task.

---

## ⚡ Active Tasks

**Current phase:** Phase 5 — Consumer Registration API ✅
**Current task:** `P6-T1` — Integration & Hardening: end-to-end test on RPi

> Phase 0 ✅. Phase 1 ✅. P2-T1..T6 ✅. MptcpManager ✅. P3-T1..T5 ✅.
> P4-T1..T5 ✅ (PathStateFsm, 17 unit tests, deployed to RPi).
> P5-T1..T7 ✅ (2026-04-07 — ConsumerApiServer: Unix socket server, epoll loop,
> Register/Unregister/QueryCurrent/PathEvent, cgroup PID assignment,
> 8 unit tests + mock_consumer, deployed — [API] server started confirmed).
>
> **IPK deploy workflow** (no reflash needed for daemon changes):
> ```bash
> ./scripts/deploy.sh root@172.16.45.2
> ```

**Do this task only. Phase 6 tasks must be completed one at a time.**

---

## Mandatory Rules

### R1 — Read before coding
Every session must start by reading in order:
1. `CLAUDE.md` — tech stack, C++23 standards, naming conventions
2. `docs/PROJECT_SUMMARY.md` — full design reference
3. `PLANNING.md` (this file) — active task and constraints

### R2 — One task at a time
- Implement only the task marked as active in "Active Tasks" above
- Do not implement tasks from future phases
- Do not "improve" other modules while working on the assigned task

### R3 — Open Points = hard stop
If implementation requires a decision on an open point:
```cpp
// OPEN POINT OP-1: fallback chain ownership not decided
// Stub only — do NOT implement logic until OP-1 is resolved
```
Report to human before continuing.

### R4 — No over-engineering
- Implement what the task requires — nothing more
- If an abstraction seems needed beyond task scope → ask human first
- Simple and correct beats clever and complex

### R5 — Never break the build
- Each commit must compile cleanly
- If refactoring is needed → keep old code commented with date before deleting
- Run `kas shell kas.yml -c "bitbake wifi-offload-manager"` before marking task done

### R6 — Commit discipline
One logical change per commit:
```
feat(config): add JSON config loader with schema validation
fix(routing): handle EEXIST when adding duplicate ip rule
test(fsm): add unit tests for PATH_UP to PATH_DEGRADED transition
chore(yocto): add netservice.cfg kernel fragment
```

### R7 — Report blockers immediately
If stuck on kernel behavior, Netlink API, or wpa_ctrl event format:
- Do NOT assume
- Do NOT use a hacky workaround
- Describe the blocker to human with exact error or behavior observed

---

## Project Source Structure

```
wifi-offload-project/                   ← Root repository
│
├── CLAUDE.md                           ← Claude Code auto-reads (agent instructions)
├── PLANNING.md                         ← Claude Code auto-reads (this file)
├── README.md
│
├── .github/
│   └── copilot-instructions.md         ← GitHub Copilot auto-reads
│
├── docs/
│   ├── PROJECT_SUMMARY.md              ← Full design reference
│   └── OPEN_POINTS.md                  ← Open points tracker (optional separate file)
│
├── kas/                                ← kas build configurations
│   ├── kas.yml                         ← Main kas config (entry point)
│   ├── machine/
│   │   ├── rpi3.yml                    ← Pi 3B+ overrides
│   │   └── rpi4.yml                    ← Pi 4 overrides (future)
│   └── features/
│       ├── debug.yml                   ← Add debug tools to image
│       └── lte.yml                     ← Add LTE modem support
│
├── meta-netservice/                    ← Yocto custom layer
│   ├── conf/
│   │   └── layer.conf
│   ├── recipes-netservice/
│   │   └── wifi-offload-manager/
│   │       ├── wifi-offload-manager.bb
│   │       └── files/
│   │           ├── wifi-offload-manager.service
│   │           └── path-policies.json
│   ├── recipes-kernel/
│   │   └── linux/
│   │       └── linux-raspberrypi/
│   │           └── netservice.cfg
│   └── recipes-core/
│       └── images/
│           └── netservice-image.bb
│
└── wifi-offload-manager/               ← Daemon source (C++23)
    ├── CMakeLists.txt
    ├── src/
    │   ├── main.cpp
    │   ├── common/
    │   │   ├── error.hpp               ← std::expected error types
    │   │   ├── logger.hpp              ← syslog wrapper using std::format
    │   │   └── types.hpp               ← PathClassConfig, PathState, PathEvent, etc.
    │   ├── config/
    │   │   ├── config_loader.hpp
    │   │   └── config_loader.cpp       ← Load + validate path-policies.json
    │   ├── routing/
    │   │   ├── routing_policy_manager.hpp
    │   │   └── routing_policy_manager.cpp  ← cgroup + iptables + ip rule setup
    │   ├── mptcp/
    │   │   ├── mptcp_manager.hpp
    │   │   └── mptcp_manager.cpp       ← Netlink MPTCP endpoint lifecycle
    │   ├── wpa/
    │   │   ├── wpa_ctrl.hpp            ← COPIED from wpa_supplicant, DO NOT MODIFY
    │   │   ├── wpa_monitor.hpp
    │   │   └── wpa_monitor.cpp         ← wpa_supplicant event loop + RSSI parsing
    │   ├── fsm/
    │   │   ├── path_state_fsm.hpp
    │   │   └── path_state_fsm.cpp      ← Path State FSM (IDLE→...→PATH_DOWN)
    │   └── api/
    │       ├── consumer_api_server.hpp
    │       └── consumer_api_server.cpp ← Unix socket server, registration, broadcast
    ├── tests/
    │   ├── CMakeLists.txt
    │   ├── test_config_loader.cpp
    │   ├── test_path_state_fsm.cpp
    │   └── test_routing_policy.cpp
    ├── config/
    │   └── path-policies.json          ← Default config → /etc/netservice/
    └── systemd/
        └── wifi-offload-manager.service
```

---

## Implementation Phases

### Phase 0 — Yocto Foundation
**Goal:** Build compiles, image boots on Pi 3B+, daemon skeleton starts  
**Status:** ✅ Complete  
**Dependency:** None

| Task | Description | Status |
|---|---|---|
| P0-T1 | Create full project directory structure | ✅ |
| P0-T2 | Write `kas.yml` + machine overlay `kas/machine/rpi3.yml` | ✅ |
| P0-T3 | Write `meta-netservice/conf/layer.conf` | ✅ |
| P0-T4 | Write `netservice.cfg` kernel fragment + `.bbappend` | ✅ |
| P0-T5 | Write `netservice-image.bb` image recipe | ✅ |
| P0-T6 | Write `wifi-offload-manager.bb` recipe (skeleton binary) | ✅ |
| P0-T7 | Write `wifi-offload-manager.service` systemd unit | ✅ |
| P0-T8 | Write daemon skeleton `main.cpp` (starts, logs, exits cleanly) | ✅ |
| P0-T9 | Write `CMakeLists.txt` with C++23 flags | ✅ |
| P0-T10 | Verify: `kas build` succeeds, image boots, daemon starts | ✅ |
**Definition of Done:**
```bash
ssh root@<pi-ip>
systemctl status wifi-offload-manager  # active (running)
uname -r                               # >= 5.6
sysctl net.mptcp.enabled              # = 1
```

---

### Phase 1 — Config Loader
**Goal:** Load and validate `path-policies.json`, expose config to other modules  
**Status:** ✅ Complete
**Dependency:** Phase 0 complete

| Task | Description | Status |
|---|---|---|
| P1-T1 | Define `PathClassConfig` struct in `common/types.hpp` | ✅ |
| P1-T2 | Implement JSON parser using nlohmann/json | ✅ |
| P1-T3 | Schema validation: required fields, value ranges, type checks | ✅ |
| P1-T4 | Unit tests: valid config, missing field, invalid mark value | ✅ |
| P1-T5 | Integrate into `main.cpp` — load config at startup | ✅ |

**Scope boundary:**
- Load and validate only — do NOT setup cgroup/iptables
- Load once at startup — no hot-reload in this phase

**Definition of Done:**
```bash
journalctl -u wifi-offload-manager | grep "\[CONFIG\]"
# [CONFIG] Loaded 3 path classes: multipath, lte_b2c, lte_b2b
```

---

### Phase 2 — Routing Policy Manager
**Goal:** Setup cgroup, iptables, ip rule according to loaded config  
**Status:** ✅ Complete  
**Dependency:** Phase 1 complete

| Task | Description | Status |
|---|---|---|
| P2-T1 | Create cgroup directories + write classid | ✅ |
| P2-T2 | Add iptables mangle rules: classid → fwmark | ✅ |
| P2-T3 | Add ip rules: fwmark → routing table (via Netlink/libmnl) | ✅ |
| P2-T4 | Add safety net DROP rules for `strict_isolation` classes | ✅ |
| P2-T5 | Cleanup on daemon exit (remove rules, delete cgroups) | ✅ |
| P2-T6 | Verify: test process packet goes to correct routing table | ✅ |

**Scope boundary:**
- Do NOT add routes to routing tables (Phase 4)
- Do NOT assign PIDs to cgroups (Phase 5)

**Definition of Done:**
```bash
ip rule show | grep -E "0x10|0x20|0x30"     # 3 rules exist
iptables -t mangle -L OUTPUT -n | grep MARK  # 3 MARK rules
ls /sys/fs/cgroup/net_cls/                   # multipath/ lte_b2c/ lte_b2b/
```

---

### Phase 3 — wpa_supplicant Monitor
**Goal:** Receive and parse WiFi events from wpa_supplicant  
**Status:** ✅ Complete (2026-04-07)
**Dependency:** Phase 0 complete (independent of Phase 1, 2)

| Task | Description | Status |
|---|---|---|
| P3-T1 | Copy `wpa_ctrl.h/.c` from wpa_supplicant upstream into `src/wpa/` | ✅ |
| P3-T2 | Implement wpa_ctrl connect / attach / event loop | ✅ |
| P3-T3 | Parse events: CONNECTED, DISCONNECTED, SIGNAL-CHANGE | ✅ |
| P3-T4 | Extract RSSI integer from `CTRL-EVENT-SIGNAL-CHANGE rssi=X avg_rssi=Y` | ✅ |
| P3-T5 | Define callback interface → feeds events into FSM (Phase 4) | ✅ |

**Scope boundary:**
- Monitor events only — do NOT manage wpa_supplicant lifecycle
- Do NOT implement scan/connect in this phase (Phase 3b, future)
- `wpa_ctrl.hpp` must remain unmodified

**Definition of Done:**
```bash
journalctl -u wifi-offload-manager -f
# [WPA] CTRL-EVENT-CONNECTED ssid=TEST_NET bssid=xx:xx:xx:xx:xx:xx
# [WPA] CTRL-EVENT-SIGNAL-CHANGE rssi=-65 avg_rssi=-67
```

---

### Phase 4 — Path State FSM
**Goal:** FSM coordinates path state, updates routing table, manages MPTCP endpoints  
**Status:** ⏸ Blocked on Phase 2 + Phase 3  
**Dependency:** Phase 2 + Phase 3 complete

| Task | Description | Status |
|---|---|---|
| P4-T1 | Implement FSM states and transitions | ⬜ |
| P4-T2 | RSSI threshold checks (rssi_warn, rssi_drop — read from config or defaults) | ⬜ |
| P4-T3 | On PATH_UP: add wlan0 route to table 100 + add MPTCP endpoint | ⬜ |
| P4-T4 | On PATH_DOWN: remove wlan0 route from table 100 + remove MPTCP endpoint | ⬜ |
| P4-T5 | Unit tests for all FSM transitions | ⬜ |
| P4-T6 | Verify MPTCP on Pi: WiFi + Ethernet dual subflow active | ✅ (early — MptcpManager via Generic Netlink, tested 2026-04-07) |

**Scope boundary:**
- Do NOT notify consumers in this phase (Phase 5)
- MPTCP endpoint only for classes with `mptcp_enabled: true`

**Definition of Done:**
```bash
ip mptcp endpoint show    # wlan0 endpoint present when WiFi up
ip route show table 100   # wlan0 route present when WiFi up, gone when down
```

---

### Phase 5 — Consumer Registration API
**Goal:** Unix socket server, consumer register/unregister, broadcast path events  
**Status:** ⏸ Blocked on Phase 2 + Phase 4  
**Dependency:** Phase 2 + Phase 4 complete

| Task | Description | Status |
|---|---|---|
| P5-T1 | Unix socket server at `/var/run/netservice/control.sock` | ⬜ |
| P5-T2 | Message protocol: register, unregister, query_current | ⬜ |
| P5-T3 | Path Registry: handle → {pid, class\_id, callback\_fd} | ⬜ |
| P5-T4 | cgroup PID assignment when consumer registers | ⬜ |
| P5-T5 | Broadcast path events to registered consumers | ⬜ |
| P5-T6 | cgroup PID removal on unregister or process exit | ⬜ |
| P5-T7 | Integration test with mock consumer (simple C++ program) | ⬜ |

**Scope boundary:**
- ⚠️ **OP-3 OPEN:** Message framing format not decided
  - Use simple fixed-size struct for now, mark with `// OPEN POINT OP-3`
- Do NOT implement fallback chain logic (OP-1 and OP-2 not decided)

**Definition of Done:**
```bash
# Mock consumer registers successfully
# WiFi up/down → mock consumer receives and logs PathEvent
```

---

### Phase 6 — Integration & Hardening
**Goal:** End-to-end test, error recovery, production-ready daemon  
**Status:** ✅ Complete  
**Dependency:** All phases complete

| Task | Description | Status |
|---|---|---|
| P6-T1 | End-to-end test: mock_consumer (C++ ARM binary) + live daemon on RPi | ✅ |
| P6-T2 | Error recovery: WpaMonitor reconnect loop w/ exponential backoff (1s..30s) | ✅ |
| P6-T3 | systemd watchdog: `Type=notify`, `WatchdogSec=30s`, `sd_notify("READY=1")` | ✅ |
| P6-T4 | Config reload via SIGHUP (logs changes; routing not re-applied) | ✅ |
| P6-T5 | AddressSanitizer: 42 tests pass clean, zero leaks detected | ✅ |
| P6-T6 | Open Points OP-1..OP-4 documented as stubs in code + PLANNING updated | ✅ |

---

## Open Points Tracker

| ID | Issue | Affects | Status |
|---|---|---|---|
| OP-1 | Fallback chain WiFi→B2C→B2B: who decides trigger and fallback? | `api/`, `fsm/` | 🔴 Deferred — stub in `path_state_fsm.hpp` |
| OP-2 | B2B in FOTA: last resort or explicit? Critical vs normal campaign? | `config/`, `api/` | 🔴 Deferred — stub in `config_loader.hpp` |
| OP-3 | Consumer API IPC message framing: TLV, fixed struct, or protobuf? | `api/` | 🟡 Implemented as fixed 96-byte struct; stub in `consumer_api_server.hpp` |
| OP-4 | JSON config signature verification scheme | `config/` | 🔴 Deferred — stub in `config_loader.hpp` |

---

## Quick Reference

```bash
# Full build
kas build kas/kas.yml

# Rebuild daemon only
kas shell kas/kas.yml -c "bitbake wifi-offload-manager"

# Rebuild kernel
kas shell kas/kas.yml -c "bitbake linux-raspberrypi"

# Check kernel config
kas shell kas/kas.yml -c "bitbake linux-raspberrypi -c kernel_configcheck"

# Fast iteration (devtool)
kas shell kas/kas.yml -c "devtool modify wifi-offload-manager"
kas shell kas/kas.yml -c "devtool deploy-target wifi-offload-manager root@<pi-ip>"

# Verify on Pi
ssh root@<pi-ip>
uname -r && sysctl net.mptcp.enabled
systemctl status wifi-offload-manager
journalctl -u wifi-offload-manager -f
ip mptcp endpoint show
ip rule show
```
