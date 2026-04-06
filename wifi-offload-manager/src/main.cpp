// main.cpp — WiFi Offload Manager daemon entry point

#include "common/logger.hpp"
#include "config/config_loader.hpp"
#include "routing/routing_policy_manager.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string_view>

// ── Default config path (overridden by --config <path>) ──────────
static constexpr std::string_view kDefaultConfigPath =
    "/etc/netservice/path-policies.json";

// ── Shutdown flag set by signal handler ──────────────────────────
static std::atomic<bool> gShutdown{false};

static void handleSignal(int sig) noexcept {
    gShutdown.store(true, std::memory_order_relaxed);
    (void)sig;
}

// ── Argument parsing ─────────────────────────────────────────────
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

    // ── Phase 1: load config ──────────────────────────────────────
    auto configResult = netservice::ConfigLoader::loadFromFile(
        std::filesystem::path{args.configPath});
    if (!configResult) {
        logger::error("[MAIN] failed to load config: {}",
            netservice::toString(configResult.error()));
        logger::close();
        return EXIT_FAILURE;
    }
    [[maybe_unused]] const auto& pathClasses = configResult.value();

    // ── Phase 2: setup cgroup net_cls hierarchy ───────────────────
    netservice::RoutingPolicyManager routingMgr{pathClasses};
    if (auto result = routingMgr.createCgroupHierarchy(); !result) {
        logger::error("[MAIN] routing setup failed: {}",
            netservice::toString(result.error()));
        logger::close();
        return EXIT_FAILURE;
    }

    // ── Phase 2-T2: add iptables mangle mark rules ────────────────
    if (auto result = routingMgr.addIptablesMarkRules(); !result) {
        logger::error("[MAIN] iptables setup failed: {}",
            netservice::toString(result.error()));
        routingMgr.cleanup();
        logger::close();
        return EXIT_FAILURE;
    }

    // TODO Phase 2-T3: addIpRules
    // TODO Phase 2-T4: addDropRules for strict_isolation
    // TODO Phase 3: pass pathClasses to WpaMonitor
    // TODO Phase 4: pass pathClasses to PathStateFsm
    // TODO Phase 5: pass pathClasses to ConsumerApiServer

    logger::info("[MAIN] startup complete — entering event loop");

    // ── Main event loop (Phase 3+ will replace pause() with epoll) ──
    while (!gShutdown.load(std::memory_order_relaxed)) {
        pause();
    }

    logger::info("[MAIN] SIGTERM received — shutting down");

    // Phase 2: remove cgroup dirs
    routingMgr.cleanup();

    logger::info("[MAIN] shutdown complete");
    logger::close();
    return EXIT_SUCCESS;
}
