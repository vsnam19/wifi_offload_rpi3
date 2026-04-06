// consumer_api_server.hpp — P5-T1..T6: Consumer Registration API
//
// Unix domain socket server at /var/run/netservice/control.sock.
// Consumers register by PID + class ID and receive PathEvent notifications.
// On register, the consumer PID is placed into the correct cgroup net_cls
// directory so traffic isolation applies automatically.
//
// OPEN POINT OP-3: message framing uses a fixed-size struct.
//                  Replace with TLV or protobuf when OP-3 is resolved.

#pragma once

#include "common/error.hpp"
#include "common/types.hpp"

#include <atomic>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace netservice {

// ── Wire protocol ────────────────────────────────────────────────────────────
// OPEN POINT OP-3: fixed-size struct framing.

enum class MsgType : uint32_t {
    Register      = 1,   // client → server
    RegisterAck   = 2,   // server → client
    Unregister    = 3,   // client → server
    UnregisterAck = 4,   // server → client
    QueryCurrent  = 5,   // client → server
    QueryAck      = 6,   // server → client
    PathEvent     = 7,   // server → client (async notification)
};

// Fixed 96-byte message.  All integers are host byte order (local socket only).
// OPEN POINT OP-3: format subject to change.
struct alignas(8) ApiMsg {
    uint32_t type;        // MsgType cast to uint32_t
    uint32_t error;       // 0 = success; ApiError value on failure
    uint64_t handle;      // Registration handle (client stores, sends on Unregister)
    int32_t  pid;         // Consumer PID (Register only)
    int32_t  rssi;        // RSSI in dBm (PathEvent / QueryAck); 0 otherwise
    uint32_t state;       // PathState cast to uint32_t (PathEvent / QueryAck)
    uint32_t pad;         // reserved, always 0
    char     classId[32]; // null-terminated class ID string
    char     iface[32];   // null-terminated interface name (PathEvent / QueryAck)
};
static_assert(sizeof(ApiMsg) == 96, "ApiMsg must be exactly 96 bytes");

// ── ConsumerApiServer ────────────────────────────────────────────────────────

class ConsumerApiServer {
public:
    static constexpr std::string_view kDefaultSocketPath =
        "/var/run/netservice/control.sock";

    // `classes` must outlive this object (or be copied — they're copied below).
    explicit ConsumerApiServer(
        const std::vector<PathClassConfig>& classes,
        std::string_view socketPath = kDefaultSocketPath) noexcept;

    ~ConsumerApiServer();

    // Create socket, start accept/event loop on a background thread.
    [[nodiscard]] std::expected<void, ApiError> start();

    // Signal the background thread to exit and join it.
    void stop() noexcept;

    // Thread-safe.  Called by PathStateFsm::Callbacks::onStateChanged.
    // Writes PathEvent messages to all consumers registered for `classId`.
    void broadcastPathEvent(std::string_view classId,
                            PathState        newState,
                            std::string_view iface,
                            int              rssiDbm) noexcept;

    // Query current state for a class (used by QueryCurrent handler).
    // Thread-safe.
    [[nodiscard]] PathState queryCurrentState(std::string_view classId) const noexcept;

    // Update cached state (called from FSM callback in main.cpp).
    void updateCurrentState(std::string_view classId, PathState state) noexcept;

private:
    // ── Per-consumer entry ────────────────────────────────────────
    struct ConsumerEntry {
        int         fd;
        int32_t     pid;
        uint64_t    handle;
        std::string classId;
    };

    // ── Per-class cached state (for QueryCurrent) ─────────────────
    struct ClassState {
        std::string classId;
        std::string cgroupPath;
        PathState   state{PathState::Idle};
        std::string activeIface;
        int         rssi{0};
    };

    // ── Fields ────────────────────────────────────────────────────
    std::vector<ClassState> classStates_;  // indexed by position, not locked (written pre-start)
    std::string             socketPath_;
    int                     listenFd_{-1};
    int                     epollFd_{-1};
    int                     wakeupFd_{-1};  // eventfd written by stop()
    std::thread             thread_;
    std::atomic<bool>       running_{false};

    mutable std::mutex          registryMutex_;
    std::vector<ConsumerEntry>  registry_;
    uint64_t                    nextHandle_{1};

    // ── Background loop ───────────────────────────────────────────
    void runLoop();
    void handleAccept();
    void handleClient(int fd);
    void removeClient(int fd) noexcept;

    // ── Protocol handlers ─────────────────────────────────────────
    void handleRegister(int fd, const ApiMsg& msg);
    void handleUnregister(int fd, const ApiMsg& msg);
    void handleQueryCurrent(int fd, const ApiMsg& msg);

    // ── cgroup helpers (P5-T4 / P5-T6) ───────────────────────────
    void assignCgroupPid(std::string_view classId, int32_t pid) noexcept;
    void removeCgroupPid(std::string_view classId, int32_t pid) noexcept;

    // ── Helpers ───────────────────────────────────────────────────
    // Returns nullptr if classId not found.
    [[nodiscard]] ClassState* findClass(std::string_view classId) noexcept;
    [[nodiscard]] const ClassState* findClass(std::string_view classId) const noexcept;

    // sendMsg: best-effort, no return value (used for single-client responses).
    static void sendMsg(int fd, const ApiMsg& msg) noexcept;
    // trySendMsg: returns false on EPIPE / error (used inside broadcastPathEvent).
    [[nodiscard]] static bool trySendMsg(int fd, const ApiMsg& msg) noexcept;
    static bool recvMsg(int fd, ApiMsg& msg) noexcept;
};

} // namespace netservice
