# WiFi Offload Manager — Claude Code Instructions

> Claude Code reads this file automatically at session start.
> Read this file completely before writing any code.

---

## Project Overview

**WiFi Offload Manager** — a Linux daemon for automotive TCU (Telematics Control Unit).
It is a **Network Service** that manages multi-path connectivity (WiFi + LTE) using MPTCP
and cgroup-based traffic isolation. It does NOT implement FOTA or Telemetry business logic.

**Full design:** `docs/PROJECT_SUMMARY.md`
**Current tasks:** `PLANNING.md` → Section "Active Tasks"

---

## Tech Stack

| Component | Technology |
|---|---|
| Language | **C++23** (GCC 13.2 via Yocto Scarthgap) |
| Build | CMake 3.26+, Ninja |
| OS / Target | Embedded Linux, Raspberry Pi 3B+ (prototype) |
| Kernel features | MPTCP (≥5.6), cgroup net\_cls, Netlink, iptables |
| WiFi control | wpa\_supplicant wpa\_ctrl API |
| IPC | Unix domain socket |
| Init system | systemd |
| JSON | nlohmann/json |
| Netlink | libmnl |
| Yocto | Scarthgap 5.0, kas |

---

## C++23 Standards

```cmake
# CMakeLists.txt
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

```cmake
# Compiler flags
target_compile_options(wifi-offload-manager PRIVATE
    -Wall -Wextra -Werror -Wpedantic
    -Wno-unused-parameter   # wpa_ctrl callback signatures
)
```

**Use C++23 features actively:**
- `std::expected<T, E>` for error handling (no exceptions in daemon code)
- `std::string_view` for read-only string parameters
- `std::span` for buffer views
- `std::format` for log messages
- Structured bindings, `if constexpr`, concepts where appropriate
- `[[nodiscard]]` on all functions returning error codes

**Avoid:**
- Exceptions (embedded daemon — use `std::expected` instead)
- `new`/`delete` (use smart pointers or RAII)
- `std::cout` (use syslog)
- Global mutable state without mutex protection

---

## Naming Conventions

```cpp
// Files: snake_case
path_state_fsm.hpp
routing_policy_manager.cpp

// Classes: PascalCase
class PathStateFsm;
class RoutingPolicyManager;

// Methods: camelCase
void addMptcpEndpoint(std::string_view iface);
std::expected<void, Error> loadConfig(std::filesystem::path configPath);

// Constants: kPascalCase
constexpr int kDefaultRssiWarnThreshold = -75;

// Enum classes: PascalCase values
enum class PathState { Idle, Scanning, Connecting, PathUp, PathDegraded, PathDown };

// Private members: trailing underscore
class Foo {
    int value_;
    std::string name_;
};
```

---

## Project Source Structure

```
wifi-offload-manager/          ← Daemon source repo (C++23)
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── config/
│   │   ├── config_loader.hpp
│   │   └── config_loader.cpp
│   ├── routing/
│   │   ├── routing_policy_manager.hpp
│   │   └── routing_policy_manager.cpp
│   ├── mptcp/
│   │   ├── mptcp_manager.hpp
│   │   └── mptcp_manager.cpp
│   ├── wpa/
│   │   ├── wpa_monitor.hpp
│   │   └── wpa_monitor.cpp
│   ├── fsm/
│   │   ├── path_state_fsm.hpp
│   │   └── path_state_fsm.cpp
│   ├── api/
│   │   ├── consumer_api_server.hpp
│   │   └── consumer_api_server.cpp
│   └── common/
│       ├── error.hpp           ← std::expected error types
│       ├── logger.hpp          ← syslog wrapper
│       └── types.hpp           ← shared type definitions
├── tests/
│   ├── CMakeLists.txt
│   ├── test_config_loader.cpp
│   ├── test_path_state_fsm.cpp
│   └── test_routing_policy.cpp
├── config/
│   └── path-policies.json      ← Default config (deployed to /etc/netservice/)
└── systemd/
    └── wifi-offload-manager.service
```

---

## Error Handling Pattern

```cpp
// ALWAYS use std::expected — never throw in daemon code
#include <expected>

enum class ConfigError { FileNotFound, ParseError, InvalidSchema, MissingField };
enum class RoutingError { CgroupCreateFailed, IptablesError, NetlinkError };

// Function signature
[[nodiscard]] std::expected<PathClassConfig, ConfigError>
loadPathClass(const nlohmann::json& json);

// Caller
auto result = loadPathClass(json);
if (!result) {
    logger::error("[CONFIG] Failed to load path class: {}", errorToString(result.error()));
    return std::unexpected(result.error());
}
auto& config = result.value();
```

---

## Logging Pattern

```cpp
// src/common/logger.hpp
#include <syslog.h>
#include <format>

namespace logger {
    template<typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) {
        syslog(LOG_INFO, "%s", std::format(fmt, std::forward<Args>(args)...).c_str());
    }
    // error(), warn(), debug() same pattern
}

// Usage
logger::info("[ROUTING] cgroup created: {} (classid=0x{:08x})", path, classid);
logger::error("[MPTCP] endpoint add failed: iface={} errno={}", iface, errno);
```

---

## Rules (MUST follow)

1. **Read `PLANNING.md` first** — check "Active Tasks" before implementing anything
2. **One task at a time** — do not implement tasks from future phases
3. **OPEN POINT = STOP** — if code touches an open point, add comment and report:
   ```cpp
   // OPEN POINT OP-1: fallback chain ownership not decided
   // Do NOT implement fallback logic until OP-1 is resolved
   ```
4. **No hardcoded interface names** — always read from config
5. **No hardcoded marks or table numbers** — always read from config
6. **Do NOT use `system()` for iptables/ip** — use Netlink (libmnl) or sockets
7. **Do NOT implement consumer business logic** — e.g., FOTA pause/resume is NOT here
8. **Keep wpa\_ctrl.hpp unmodified** — it is copied from upstream wpa\_supplicant

---

## Key Design Constraints

- Path classes are **data-driven** (JSON config), not hardcoded enums
- Traffic isolation uses **cgroup net\_cls** (process-level), not SO\_MARK (socket-level)
- MPTCP endpoints managed only for `multipath` class
- B2B class (`lte_b2b`) must never mix with WiFi or B2C — enforced by iptables DROP rules
- Consumer API: Network Service does NOT know what consumer is doing with the path event

---

## Open Points (DO NOT implement until resolved)

| ID | Description | Affected Module |
|---|---|---|
| OP-1 | Fallback chain WiFi→B2C→B2B: who decides trigger? | `api/`, `fsm/` |
| OP-2 | B2B in FOTA chain: last resort or explicit? | `config/`, `api/` |
| OP-3 | Consumer API IPC message framing format | `api/` |
| OP-4 | JSON config signature verification | `config/` |
