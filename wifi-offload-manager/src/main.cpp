// main.cpp — WiFi Offload Manager daemon entry point

#include "common/logger.hpp"
#include "config/config_loader.hpp"
#include "routing/routing_policy_manager.hpp"
#include "mptcp/mptcp_manager.hpp"
#include "fsm/path_state_fsm.hpp"
#include "wpa/wpa_monitor.hpp"
#include "api/consumer_api_server.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

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

    // ── Phase 2-T3: add ip fwmark rules ──────────────────────────
    if (auto result = routingMgr.addIpRules(); !result) {
        logger::error("[MAIN] ip rules setup failed: {}",
            netservice::toString(result.error()));
        routingMgr.cleanup();
        logger::close();
        return EXIT_FAILURE;
    }

    // ── Phase 2-T4: add strict_isolation DROP rules ────────────────
    if (auto result = routingMgr.addDropRules(); !result) {
        logger::error("[MAIN] DROP rules setup failed: {}",
            netservice::toString(result.error()));
        routingMgr.cleanup();
        logger::close();
        return EXIT_FAILURE;
    }

    // ── Phase 4 (early): MPTCP endpoint registration ─────────────
    netservice::MptcpManager mptcpMgr{pathClasses};
    if (auto result = mptcpMgr.addEndpoints(); !result) {
        logger::error("[MAIN] MPTCP endpoint setup failed: {}",
            netservice::toString(result.error()));
        routingMgr.cleanup();
        logger::close();
        return EXIT_FAILURE;
    }

    // ── Phase 5: Consumer API Server (construct early so FSM callbacks can capture it) ──
    auto apiServerPtr = std::make_unique<netservice::ConsumerApiServer>(pathClasses);

    // ── Phase 4: Path State FSM + WpaMonitor ─────────────────────
    // Create one FSM per path class that has a WiFi interface.
    // For now we only monitor the "multipath" class (index 0, interface[0]).
    // A future phase can instantiate one FSM per class.
    std::vector<std::unique_ptr<netservice::PathStateFsm>> fsms;
    std::vector<std::unique_ptr<netservice::WpaMonitor>>   monitors;
    std::vector<std::thread>                               monitorThreads;

    for (const auto& cls : pathClasses) {
        if (cls.interfaces.empty()) continue;

        const std::string wifiIface = cls.interfaces.front();
        const std::string ctrlPath  =
            std::format("/var/run/wpa_supplicant/{}", wifiIface);
        // Only attach WpaMonitor to classes that actually use wpa_supplicant.
        // LTE-only classes (no wpa_supplicant socket) are skipped silently.
        // (A future config flag can make this explicit.)

        netservice::PathStateFsm::Callbacks cbs;

        cbs.onPathUp = [&routingMgr, &mptcpMgr, cls](
                            std::string_view iface, uint32_t table) {
            logger::info("[MAIN] PATH_UP iface={} table={}", iface, table);
            const uint32_t gw =
                netservice::RoutingPolicyManager::queryGatewayForIface(iface);
            if (gw != 0) {
                if (auto r = routingMgr.addDefaultRoute(iface, gw, table); !r) {
                    logger::error("[MAIN] addDefaultRoute failed for iface={}", iface);
                }
            } else {
                logger::warn("[MAIN] no gateway found for iface={}, route not added", iface);
            }
            if (cls.mptcpEnabled) {
                if (auto r = mptcpMgr.addEndpoints(); !r) {
                    logger::warn("[MAIN] MPTCP re-add failed for class={}", cls.id);
                }
            }
        };

        cbs.onPathDown = [&routingMgr](
                              std::string_view iface, uint32_t table) {
            logger::info("[MAIN] PATH_DOWN iface={} table={}", iface, table);
            if (auto r = routingMgr.removeDefaultRoute(iface, table); !r) {
                logger::warn("[MAIN] removeDefaultRoute failed for iface={}", iface);
            }
        };

        cbs.onStateChanged = [&apiServer = *apiServerPtr, id = cls.id](
                                  netservice::PathState /*from*/,
                                  netservice::PathState to,
                                  std::string_view iface,
                                  int rssi) {
            apiServer.broadcastPathEvent(id, to, iface, rssi);
            apiServer.updateCurrentState(id, to);
        };

        auto fsm = std::make_unique<netservice::PathStateFsm>(cls, std::move(cbs));

        auto monitor = std::make_unique<netservice::WpaMonitor>(ctrlPath, wifiIface);
        monitor->setEventCallback(
            [&fsm = *fsm](const netservice::WpaEvent& ev) {
                fsm.onWpaEvent(ev);
            });

        // Start monitor in a detached thread; stop() is called on shutdown.
        monitorThreads.emplace_back([mon = monitor.get()]() {
            if (auto r = mon->start(); !r) {
                logger::warn("[MAIN] WpaMonitor exited: errno via stop()");
            }
        });

        fsms.push_back(std::move(fsm));
        monitors.push_back(std::move(monitor));
    }

    // ── Phase 5: start Consumer API Server ─────────────────────
    if (auto r = apiServerPtr->start(); !r) {
        logger::error("[MAIN] ConsumerApiServer start failed: {}",
            netservice::toString(r.error()));
        routingMgr.cleanup();
        logger::close();
        return EXIT_FAILURE;
    }

    logger::info("[MAIN] startup complete — entering event loop");

    // ── Main event loop ───────────────────────────────────────────
    while (!gShutdown.load(std::memory_order_relaxed)) {
        pause();
    }

    logger::info("[MAIN] SIGTERM received — shutting down");

    // Stop all WpaMonitor threads
    for (auto& mon : monitors) {
        mon->stop();
    }
    for (auto& t : monitorThreads) {
        if (t.joinable()) t.join();
    }

    // Phase 5: stop Consumer API server
    apiServerPtr->stop();

    // Phase 4: flush MPTCP endpoints
    mptcpMgr.removeEndpoints();

    // Phase 2: remove cgroup dirs
    routingMgr.cleanup();

    logger::info("[MAIN] shutdown complete");
    logger::close();
    return EXIT_SUCCESS;
}
