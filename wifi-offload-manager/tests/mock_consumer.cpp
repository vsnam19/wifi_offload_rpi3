// mock_consumer.cpp — Standalone mock consumer for integration testing
//
// Usage (run on target RPi while daemon is running):
//   mock_consumer [--socket <path>] [--class <classId>] [--events <N>]
//
// Registers with the daemon, prints PathEvents until N events received or
// 30 seconds elapses, then unregisters and exits.

#include "api/consumer_api_server.hpp"

#include <cstring>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace netservice;

// ── helpers ───────────────────────────────────────────────────────────────────

static std::string_view pathStateName(uint32_t s) {
    switch (static_cast<PathState>(s)) {
        case PathState::Idle:         return "Idle";
        case PathState::Scanning:     return "Scanning";
        case PathState::Connecting:   return "Connecting";
        case PathState::PathUp:       return "PathUp";
        case PathState::PathDegraded: return "PathDegraded";
        case PathState::PathDown:     return "PathDown";
    }
    return "Unknown";
}

static bool sendMsg(int fd, const ApiMsg& msg) {
    const char* p = reinterpret_cast<const char*>(&msg);
    std::size_t rem = sizeof(ApiMsg);
    while (rem > 0) {
        const ssize_t n = ::send(fd, p, rem, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n; rem -= static_cast<std::size_t>(n);
    }
    return true;
}

static bool recvMsg(int fd, ApiMsg& msg) {
    char* p = reinterpret_cast<char*>(&msg);
    std::size_t rem = sizeof(ApiMsg);
    while (rem > 0) {
        const ssize_t n = ::recv(fd, p, rem, MSG_WAITALL);
        if (n <= 0) return false;
        p += n; rem -= static_cast<std::size_t>(n);
    }
    return true;
}

// ── argument parsing ──────────────────────────────────────────────────────────

struct Args {
    std::string socketPath{std::string{ConsumerApiServer::kDefaultSocketPath}};
    std::string classId{"multipath"};
    int         maxEvents{10};
};

static Args parseArgs(std::span<char*> argv) {
    Args args;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        std::string_view a{argv[i]};
        if (a == "--socket" && i + 1 < argv.size()) {
            args.socketPath = argv[++i];
        } else if (a == "--class" && i + 1 < argv.size()) {
            args.classId = argv[++i];
        } else if (a == "--events" && i + 1 < argv.size()) {
            args.maxEvents = std::stoi(std::string{argv[++i]});
        }
    }
    return args;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const auto args = parseArgs(std::span{argv, static_cast<std::size_t>(argc)});

    std::cout << std::format("[mock_consumer] socket={} class={} maxEvents={}\n",
                             args.socketPath, args.classId, args.maxEvents);

    // Connect.
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        std::cerr << "[mock_consumer] socket() failed\n";
        return 1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, args.socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << std::format("[mock_consumer] connect({}) failed: {}\n",
                                 args.socketPath, std::strerror(errno));
        ::close(fd);
        return 1;
    }
    std::cout << "[mock_consumer] connected\n";

    // Register.
    ApiMsg reg{};
    reg.type = static_cast<uint32_t>(MsgType::Register);
    reg.pid  = static_cast<int32_t>(::getpid());
    std::strncpy(reg.classId, args.classId.c_str(), sizeof(reg.classId) - 1);

    if (!sendMsg(fd, reg)) {
        std::cerr << "[mock_consumer] send Register failed\n";
        ::close(fd);
        return 1;
    }

    ApiMsg regAck{};
    if (!recvMsg(fd, regAck) || regAck.error != 0) {
        std::cerr << std::format("[mock_consumer] Register failed: error={}\n", regAck.error);
        ::close(fd);
        return 1;
    }
    const uint64_t handle = regAck.handle;
    std::cout << std::format("[mock_consumer] registered: handle={} class={} "
                             "initial_state={}\n",
                             handle, args.classId,
                             pathStateName(regAck.state));

    // Set 30 s receive timeout.
    struct timeval tv{30, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Receive path events.
    int received = 0;
    while (received < args.maxEvents) {
        ApiMsg event{};
        if (!recvMsg(fd, event)) {
            std::cout << "[mock_consumer] connection closed or timeout\n";
            break;
        }
        if (event.type == static_cast<uint32_t>(MsgType::PathEvent)) {
            std::cout << std::format(
                "[mock_consumer] PathEvent: class={} state={} iface={} rssi={}dBm\n",
                event.classId,
                pathStateName(event.state),
                event.iface,
                event.rssi);
            ++received;
        }
    }
    std::cout << std::format("[mock_consumer] {} events received\n", received);

    // Unregister.
    ApiMsg unreg{};
    unreg.type   = static_cast<uint32_t>(MsgType::Unregister);
    unreg.handle = handle;
    sendMsg(fd, unreg);   // best-effort; server will clean up on close anyway

    ApiMsg unregAck{};
    recvMsg(fd, unregAck);

    ::close(fd);
    std::cout << "[mock_consumer] done\n";
    return 0;
}
