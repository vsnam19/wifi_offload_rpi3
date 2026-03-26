// main.cpp — WiFi Offload Manager daemon entry point
//
// Phase 0 skeleton: opens syslog, installs signal handlers, and blocks
// until SIGTERM/SIGINT.  Each subsequent phase will plug its module in
// between the "startup" and the main event loop.

#include "common/logger.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <span>
#include <string_view>

// ── Default config path (overridden by --config <path>) ──────────
static constexpr std::string_view kDefaultConfigPath =
    "/etc/netservice/path-policies.json";

// ── Shutdown flag set by signal handler ──────────────────────────
static std::atomic<bool> gShutdown{false};

static void handleSignal(int sig) noexcept {
    // async-signal-safe: only write to atomic flag and syslog
    gShutdown.store(true, std::memory_order_relaxed);
    (void)sig;
}

// ── Argument parsing (minimal) ───────────────────────────────────
struct Args {
    std::string_view configPath{kDefaultConfigPath};
};

[[nodiscard]] static Args parseArgs(std::span<char*> argv) {
    Args args;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--config" && i + 1 < argv.size()) {
            args.configPath = argv[++i];
        }
    }
    return args;
}

// ── Entry point ───────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    logger::open("wifi-offload-manager");

    const auto args = parseArgs(std::span{argv, static_cast<std::size_t>(argc)});

    logger::info("[MAIN] wifi-offload-manager starting");
    logger::info("[MAIN] config path: {}", args.configPath);

    // Install signal handlers for clean shutdown
    struct sigaction sa{};
    sa.sa_handler = handleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);

    // Verify config file exists (full load deferred to Phase 1)
    if (!std::filesystem::exists(std::filesystem::path{args.configPath})) {
        logger::error("[MAIN] config file not found: {}", args.configPath);
        logger::close();
        return EXIT_FAILURE;
    }

    logger::info("[MAIN] startup complete — entering event loop");

    // ── Main event loop (placeholder — Phase 1+ will add epoll/poll) ──
    while (!gShutdown.load(std::memory_order_relaxed)) {
        // TODO Phase 1: drive config loader
        // TODO Phase 2: drive routing policy manager
        // TODO Phase 3: drive wpa_monitor event loop
        // TODO Phase 4: drive path state FSM
        // TODO Phase 5: drive consumer API server

        pause(); // sleep until any signal arrives
    }

    logger::info("[MAIN] SIGTERM received — shutting down");

    // TODO Phase 2: cleanup routing rules and cgroups
    // TODO Phase 3: disconnect wpa_ctrl

    logger::info("[MAIN] shutdown complete");
    logger::close();
    return EXIT_SUCCESS;
}
