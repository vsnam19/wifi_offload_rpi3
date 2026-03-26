# WiFi Offload Manager — Project Summary

> Full design reference for AI agents and developers.
> For active task tracking, see `PLANNING.md`.

---

## 1. Project Goal

Build a **WiFi Offload Manager** daemon running on **Embedded Linux (Yocto Scarthgap 5.0)**
for an automotive TCU (Telematics Control Unit).

The daemon is a **Network Infrastructure Service**. It:
- Manages multi-path connectivity across WiFi and LTE interfaces
- Provides a registration API for consumer services (FOTA, Telemetry, B2B Fleet)
- Does **NOT** implement consumer business logic

**Prototype target:** Raspberry Pi 3B+ — WiFi (`wlan0`) + Ethernet (`eth0`) simulating LTE  
**Production target:** TCU with `wlan0` + `wwan0` (LTE B2C) + `wwan1` (LTE B2B)

---

## 2. Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│                    Consumer Layer                            │
│  FOTA Agent       Telemetry Agent       B2B Service         │
│  register(pid,    register(pid,         register(pid,        │
│  "multipath")     "multipath")          "lte_b2b")           │
│  → on_path_event()→ on_path_event()     → on_path_event()   │
└───────────────────────┬──────────────────────────────────────┘
                        │ Unix socket /var/run/netservice/control.sock
┌───────────────────────▼──────────────────────────────────────┐
│              WiFi Offload Manager (Daemon)                   │
│                                                              │
│  ┌─────────────┐  ┌────────────────┐  ┌──────────────────┐  │
│  │ Config      │  │ Path State FSM │  │ Consumer API     │  │
│  │ Loader      │  │ IDLE→SCANNING  │  │ Server           │  │
│  │ (JSON)      │  │ →CONNECTING    │  │ Path Registry    │  │
│  └─────────────┘  │ →PATH_UP       │  └──────────────────┘  │
│                   │ →PATH_DEGRADED │                         │
│  ┌─────────────┐  │ →PATH_DOWN     │  ┌──────────────────┐  │
│  │ Routing     │  └────────────────┘  │ MPTCP Manager    │  │
│  │ Policy Mgr  │                      │ (Netlink)        │  │
│  │ cgroup +    │  ┌────────────────┐  └──────────────────┘  │
│  │ iptables +  │  │ wpa_supplicant │                         │
│  │ ip rule     │  │ Monitor        │                         │
│  └─────────────┘  └────────────────┘                         │
└──────────────────────────────────────────────────────────────┘
                        │
┌───────────────────────▼──────────────────────────────────────┐
│                    Kernel Space                              │
│  cgroup net_cls │ iptables mangle │ ip rule │ ip route       │
│  MPTCP (≥5.6)   │ wlan0           │ eth0/wwan0 │ wwan1        │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. Physical Interfaces

| Interface | Role | Prototype mapping |
|---|---|---|
| `wlan0` | WiFi (dynamic) | `wlan0` (Pi 3B+ onboard) |
| `wwan0` | LTE B2C (public internet) | `eth0` (Ethernet simulates LTE) |
| `wwan1` | LTE B2B (dedicated OEM APN) | `veth0` (virtual, for isolation test) |

---

## 4. Path Classes

Path classes are **data-driven** — defined in `/etc/netservice/path-policies.json`.
Adding a new class requires a config update only, not a code rebuild.

```json
{
  "path_classes": [
    {
      "id":               "multipath",
      "classid":          "0x00100001",
      "cgroup_path":      "/sys/fs/cgroup/net_cls/multipath",
      "interfaces":       ["wlan0", "wwan0"],
      "mptcp_enabled":    true,
      "routing_table":    100,
      "mark":             "0x10",
      "strict_isolation": false
    },
    {
      "id":               "lte_b2c",
      "classid":          "0x00100002",
      "cgroup_path":      "/sys/fs/cgroup/net_cls/lte_b2c",
      "interfaces":       ["wwan0"],
      "mptcp_enabled":    false,
      "routing_table":    200,
      "mark":             "0x20",
      "strict_isolation": false
    },
    {
      "id":               "lte_b2b",
      "classid":          "0x00100003",
      "cgroup_path":      "/sys/fs/cgroup/net_cls/lte_b2b",
      "interfaces":       ["wwan1"],
      "mptcp_enabled":    false,
      "routing_table":    300,
      "mark":             "0x30",
      "strict_isolation": true
    }
  ]
}
```

---

## 5. Traffic Isolation — cgroup net_cls

**Why not SO\_MARK per-socket:**
Consumer processes use third-party libraries (libcurl, gRPC, mosquitto) that create
sockets internally. The consumer cannot call `setsockopt(SO_MARK)` on those sockets.

**Solution: cgroup net\_cls**
The kernel marks ALL packets from a process based on cgroup membership.
No changes required in consumer code or libraries.

```
Consumer process (pid 1234) → cgroup /net_cls/multipath
  └── libcurl socket()  ← created internally, no consumer control
        └── kernel marks packet = classid 0x00100001 (automatic)
              └── iptables -t mangle: classid → fwmark 0x10
                    └── ip rule: fwmark 0x10 → table 100
```

**Required kernel config:**
```
CONFIG_CGROUP_NET_CLASSID=y
CONFIG_NETFILTER_XT_MATCH_CGROUP=y
CONFIG_NETFILTER_XT_TARGET_MARK=y
CONFIG_IP_MULTIPLE_TABLES=y
```

---

## 6. Routing Architecture

```bash
# ── iptables mangle: classid → fwmark ────────────────────────
iptables -t mangle -A OUTPUT -m cgroup --cgroup 0x00100001 -j MARK --set-mark 0x10
iptables -t mangle -A OUTPUT -m cgroup --cgroup 0x00100002 -j MARK --set-mark 0x20
iptables -t mangle -A OUTPUT -m cgroup --cgroup 0x00100003 -j MARK --set-mark 0x30

# ── ip rule: fwmark → routing table ──────────────────────────
ip rule add fwmark 0x10 table 100   # multipath
ip rule add fwmark 0x20 table 200   # lte_b2c
ip rule add fwmark 0x30 table 300   # lte_b2b

# ── Table 100: dynamic (updated on WiFi up/down) ─────────────
ip route add default via <wwan0_gw> dev wwan0 table 100   # always present
ip route add default via <wlan0_gw> dev wlan0 table 100   # added on WiFi up

# ── Table 200: static ─────────────────────────────────────────
ip route add default via <wwan0_gw> dev wwan0 table 200

# ── Table 300: static (B2B APN) ───────────────────────────────
ip route add default via <wwan1_gw> dev wwan1 table 300

# ── Safety net (strict_isolation) ────────────────────────────
iptables -I OUTPUT -o wlan0 -m mark --mark 0x30 -j DROP   # B2B → no WiFi
iptables -I OUTPUT -o wwan0 -m mark --mark 0x30 -j DROP   # B2B → no B2C
iptables -I OUTPUT -o wwan1 -m mark --mark 0x10 -j DROP   # multipath → no B2B
iptables -I OUTPUT -o wwan1 -m mark --mark 0x20 -j DROP   # lte_b2c → no B2B
```

---

## 7. MPTCP

- Mainline Linux kernel since 5.6 — enable only, no patches required
- **Only applies to `multipath` class** — B2C and B2B use standard TCP
- Network Service manages **endpoint lifecycle** via Netlink
- Consumer opens `IPPROTO_MPTCP` socket — kernel handles subflow scheduling automatically

```cpp
// Consumer opens MPTCP socket — cgroup handles routing automatically
int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_MPTCP);
```

**Endpoint lifecycle:**
```bash
# WiFi connected → add subflow
ip mptcp endpoint add <wlan0_ip> dev wlan0 subflow
ip route add default via <gw> dev wlan0 table 100

# WiFi disconnected → remove subflow
ip mptcp endpoint delete id <endpoint_id>
ip route del default dev wlan0 table 100
```

---

## 8. wpa_supplicant Controller

- Direct control via `wpa_ctrl` Unix socket — no ConnMan or NetworkManager
- Trusted SSID list: `/etc/netservice/trusted-ssids.conf` (read-only, provisioned at EOL/Dealer)
- Events handled:
  - `CTRL-EVENT-CONNECTED` → trigger FSM: CONNECTING → PATH\_UP
  - `CTRL-EVENT-DISCONNECTED` → trigger FSM: → PATH\_DOWN
  - `CTRL-EVENT-SIGNAL-CHANGE rssi=X` → trigger FSM: PATH\_UP ↔ PATH\_DEGRADED

---

## 9. Path State FSM

```
IDLE ──[scan timer]──▶ SCANNING ──[trusted SSID found]──▶ CONNECTING
                                                                │
                                                    [auth ok]   │
                                                                ▼
                                                           PATH_UP
                                                          ↑       │
                                              [rssi ok]   │       │ [rssi < warn]
                                                          │       ▼
                                                     PATH_DEGRADED
                                                                │
                                              [rssi < drop]     │
                                                                ▼
PATH_DOWN ◀──[disconnect]──────────────────────────────────────┘
```

**RSSI Thresholds (configurable):**

| Threshold | Default | Transition |
|---|---|---|
| `rssi_connect_min` | -70 dBm | CONNECTING → PATH\_UP |
| `rssi_warn` | -75 dBm | PATH\_UP → PATH\_DEGRADED |
| `rssi_drop` | -85 dBm | PATH\_DEGRADED → PATH\_DOWN |

---

## 10. Consumer Registration API

```cpp
enum class PathEvent { PathUp, PathDegraded, PathDown };

struct PathInfo {
    PathEvent   event;
    std::string iface;
    int         rssiDbm;
    uint32_t    estBandwidthKbps;
};

using PathCallback = std::function<void(const PathInfo&)>;

// Register: assigns consumer process to cgroup for given class
// Service does NOT store consumer identity — only pid, class, callback
PathHandle registerPath(pid_t pid, std::string_view classId, PathCallback callback);

// Unregister: removes pid from cgroup, releases handle
void unregisterPath(PathHandle handle);

// Query current state without waiting for event
std::expected<PathInfo, ApiError> getCurrentPath(std::string_view classId);
```

**IPC transport:** Unix domain socket at `/var/run/netservice/control.sock`

---

## 11. Cellular Profile (Two-Dimension Model)

Path decisions have two independent dimensions:

| Dimension | Description |
|---|---|
| **Path Priority Type** | HOW: WiFi vs Cellular vs MPTCP (7 types) |
| **Cellular Profile** | WHICH cellular: B2B or B2C |

**7 Path Priority Types:**

| Type | Description |
|---|---|
| 1 | Cellular Only |
| 2 | WiFi Only |
| 3 | Cellular Priority (WiFi fallback) |
| 4 | WiFi Priority (Cellular fallback) |
| 5 | Cellular Priority + MPTCP |
| 6 | WiFi Priority + MPTCP |
| 7 | Simultaneous MPTCP |

**B2B Valid Combinations:** Type 1 and Type 3 only.
B2B + WiFi/MPTCP types = **invalid** → daemon must reject at registration.

---

## 12. Telematics Service Matrix

| Service | Cellular Profile | Primary Type | Fallback Type |
|---|---|---|---|
| eCall / Emergency | B2B | 1 | — |
| B2B Fleet Telematics | B2B | 1 | 3 |
| Remote Diagnostics (UDS/DoIP) | B2B | 1 | 3 |
| Remote Vehicle Service | B2C | 3 | 5 |
| Real-time Telemetry | B2C | 3 | 5 |
| Bulk Telemetry Upload | B2C | 5 | 6 |
| FOTA / OTA Update | B2C | 6 | 3 |
| Navigation Map Update | B2C | 6 | 4 |
| Software / Config Update | B2C | 6 | 3 |
| Infotainment Streaming | B2C | 7 | — |
| VoWiFi / VoIP | B2C | 2 | 4 |

---

## 13. Open Points

These are **unresolved design decisions**. Do NOT implement code that depends on them
until they are resolved. Mark affected code with `// OPEN POINT OP-N`.

| ID | Issue | Impact |
|---|---|---|
| OP-1 | Fallback chain WiFi→B2C→B2B: who decides trigger condition and fallback logic? Network Service or FOTA Agent? | `api/`, `fsm/` |
| OP-2 | B2B in FOTA chain: last resort only, or can be explicitly requested? Critical vs normal campaign distinction? | `config/`, `api/` |
| OP-3 | Consumer API IPC message framing: TLV, fixed struct, or protobuf? | `api/` |
| OP-4 | JSON config integrity verification: signature scheme? | `config/` |

---

## 14. Yocto Setup

| Item | Value |
|---|---|
| Release | Scarthgap 5.0 (LTS) |
| GCC | 13.2 |
| C++ Standard | C++23 |
| Kernel | 6.6 LTS |
| Build tool | kas |
| Custom layer | `meta-netservice` |
| Target machine | `raspberrypi3` |
| Image | `netservice-image` |

---

## 15. Kernel Config Fragment

```
# netservice.cfg
CONFIG_MPTCP=y
CONFIG_INET_MPTCP_DIAG=y
CONFIG_CGROUP_NET_CLASSID=y
CONFIG_CGROUP_NET_PRIO=y
CONFIG_NETFILTER_XT_MATCH_CGROUP=y
CONFIG_NETFILTER_XT_TARGET_MARK=y
CONFIG_NETFILTER_XT_MATCH_MARK=y
CONFIG_IP_MULTIPLE_TABLES=y
CONFIG_IP_ROUTE_MULTIPATH=y
CONFIG_IP_ADVANCED_ROUTER=y
CONFIG_MAC80211=y
CONFIG_CFG80211=y
CONFIG_USB_SERIAL=y
CONFIG_USB_SERIAL_OPTION=y
CONFIG_USB_NET_QMI_WWAN=y
CONFIG_USB_WDM=y
CONFIG_USB_NET_CDCETHER=y
CONFIG_USB_NET_RNDIS_HOST=y
CONFIG_USB_NET_CDC_NCM=y
```
