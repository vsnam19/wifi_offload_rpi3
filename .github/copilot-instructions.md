# GitHub Copilot Instructions — WiFi Offload Manager

## Project Context

WiFi Offload Manager is a **C++23 Linux daemon** for automotive TCU.
It manages multi-path network connectivity (WiFi + LTE) using MPTCP and cgroup-based
traffic isolation. It is a **pure infrastructure service** — it does NOT implement
FOTA, Telemetry, or any other consumer business logic.

---

## Language & Standard

- **C++23** (GCC 13.2, Yocto Scarthgap 5.0)
- `-std=c++23`, no GNU extensions (`-std=gnu++23` is NOT used)
- **No exceptions** in daemon code — use `std::expected<T, E>` for all error paths
- **No raw new/delete** — RAII, `std::unique_ptr`, `std::shared_ptr`
- **No `system()` calls** for shell commands — use Netlink sockets or syscalls directly

---

## Naming Conventions

```cpp
// Files:        snake_case.hpp / snake_case.cpp
// Classes:      PascalCase
// Methods:      camelCase
// Constants:    kPascalCase (constexpr)
// Enums:        enum class PascalCase { PascalCaseValues }
// Private members: trailing_underscore_

class PathStateFsm {
public:
    void onWifiConnected(std::string_view iface);
    [[nodiscard]] PathState currentState() const noexcept;

private:
    PathState state_{PathState::Idle};
    int rssiWarnThreshold_{kDefaultRssiWarnThreshold};
};
```

---

## Error Handling

```cpp
// ALWAYS std::expected — NEVER throw
#include <expected>

[[nodiscard]] std::expected<void, RoutingError>
addCgroupRule(std::string_view cgroupPath, uint32_t classid);

// Propagate errors with monadic operations
return addCgroupRule(path, classid)
    .and_then([&]{ return addIptablesRule(mark); })
    .and_then([&]{ return addIpRule(mark, table); });
```

---

## Logging

```cpp
// Use syslog — NEVER std::cout or printf
#include "common/logger.hpp"

logger::info("[MODULE] action: detail={}", value);
logger::warn("[MODULE] unexpected condition: {}", reason);
logger::error("[MODULE] fatal: errno={} ({})", errno, strerror(errno));
```

---

## Key Types (from `src/common/types.hpp`)

```cpp
struct PathClassConfig {
    std::string id;
    uint32_t    classid;
    std::string cgroupPath;
    std::vector<std::string> interfaces;
    bool        mptcpEnabled;
    int         routingTable;
    uint32_t    mark;
    bool        strictIsolation;
};

enum class PathState    { Idle, Scanning, Connecting, PathUp, PathDegraded, PathDown };
enum class PathEvent    { WifiConnected, WifiDisconnected, SignalWarn, SignalDrop };
enum class CellularProfile { B2C, B2B };
```

---

## What NOT to suggest

- Do NOT suggest `NetworkManager` or `ConnMan` — daemon uses `wpa_ctrl` directly
- Do NOT suggest `system("iptables ...")` — use `libmnl` or raw Netlink
- Do NOT suggest hardcoded interface names like `"wlan0"` — read from config
- Do NOT suggest hardcoded marks (`0x10`) — read from `PathClassConfig`
- Do NOT suggest adding exception handling (`try/catch`) in daemon code
- Do NOT suggest consumer business logic (FOTA pause, Telemetry flush) — out of scope
- Do NOT suggest `boost` — keep dependencies minimal for embedded target

---

## Preferred Patterns

```cpp
// Range-based operations
auto multipath = std::ranges::find_if(classes_, [](const auto& c) {
    return c.mptcpEnabled;
});

// Structured bindings
for (const auto& [id, config] : pathClassMap_) { ... }

// std::format for messages
auto msg = std::format("[ROUTING] table {} rule added for mark 0x{:08x}", table, mark);

// Monadic error handling
return loadJson(path)
    .and_then(validateSchema)
    .and_then(parsePathClasses)
    .transform([](auto classes) { return DaemonConfig{std::move(classes)}; });
```

---

## Module Boundaries (do NOT cross)

| Module | Responsibility | Does NOT |
|---|---|---|
| `config/` | Load + validate JSON | Setup kernel rules |
| `routing/` | cgroup, iptables, ip rule | Know consumer PIDs |
| `mptcp/` | Netlink endpoint lifecycle | Know path class business rules |
| `wpa/` | wpa\_supplicant events | Make routing decisions |
| `fsm/` | Path state transitions | Call consumer callbacks |
| `api/` | Consumer register/notify | Know what consumer does with events |
