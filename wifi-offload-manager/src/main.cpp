// main.cpp — WiFi Offload Manager daemon entry point

#include "common/logger.hpp"
#include "config/config_loader.hpp"
#include "routing/routing_policy_manager.hpp"
#include "mptcp/mptcp_manager.hpp"
#include "fsm/path_state_fsm.hpp"
#include "wpa/wpa_monitor.hpp"
#include "api/consumer_api_server.hpp"

#ifdef HAVE_LIBSYSTEMD
#  include <systemd/sd-daemon.h>
#  define SD_NOTIFY(s) sd_notify(0, (s))
#else
#  define SD_NOTIFY(s) ((void)0)  // watchdog disabled — no libsystemd
#endif

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

// ── Config reload flag set by SIGHUP handler ─────────────────────
// P6-T4: SIGHUP causes a graceful config file reload in the main loop.
static std::atomic<bool> gReloadConfig{false};

static void handleSignal(int sig) noexcept {
    if (sig == SIGHUP) {
        gReloadConfig.store(true, std::memory_order_relaxed);
    } else {
        gShutdown.store(true, std::memory_order_relaxed);
    }
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

    // P6-T4: SIGHUP for config reload; do NOT use SA_RESTART so pause() is
    // interrupted immediately when SIGHUP is delivered.
    struct sigaction saSighup{};
    saSighup.sa_handler = handleSignal;
    sigemptyset(&saSighup.sa_mask);
    saSighup.sa_flags = 0;  // no SA_RESTART
    sigaction(SIGHUP, &saSighup, nullptr);

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

    // P6-T3: notify systemd that the daemon is ready (Type=notify in .service).
    // Also start a watchdog ping thread that resets the watchdog every 10s
    // (WatchdogSec=30s in the service file — two missed pings trigger restart).
    SD_NOTIFY("READY=1");

    std::thread wdThread{[]() {
        // Ping every 10 s; WatchdogSec=30s means 3 missed pings before restart.
        while (!gShutdown.load(std::memory_order_relaxed)) {
            SD_NOTIFY("WATCHDOG=1");
            // Sleep in 100 ms slices so shutdown is responsive.
            for (int i = 0; i < 100 && !gShutdown.load(std::memory_order_relaxed); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        }
    }};

    // ── Main event loop ──────────────────────────────────────────
    // pause() blocks until any signal is delivered.  SIGTERM/SIGINT/SIGHUP all
    // interrupt it.  The SA_RESTART flag is NOT set for SIGHUP so it reliably
    // interrupts pause() even on Linux.
    while (!gShutdown.load(std::memory_order_relaxed)) {
        pause();

        // P6-T4: SIGHUP — reload config from disk, log any changes.
        // Routing/iptables/MPTCP infra is NOT re-applied (requires daemon restart).
        // OPEN POINT OP-1: fallback chain trigger is unresolved — do not re-apply routing on reload.
        if (gReloadConfig.exchange(false, std::memory_order_relaxed)) {
            logger::info("[MAIN] SIGHUP received — reloading config: {}", args.configPath);
            auto newCfg = netservice::ConfigLoader::loadFromFile(
                std::filesystem::path{args.configPath});
            if (newCfg) {
                const auto& newClasses = newCfg.value();
                logger::info("[MAIN] config reloaded: {} path classes", newClasses.size());
                if (newClasses.size() != pathClasses.size()) {
                    logger::warn("[MAIN] class count changed ({} -> {}) — restart daemon to apply",
                                 pathClasses.size(), newClasses.size());
                }
            } else {
                logger::error("[MAIN] SIGHUP: config reload failed: {}",
                    netservice::toString(newCfg.error()));
            }
        }
    }

    logger::info("[MAIN] SIGTERM received — shutting down");

    // Stop watchdog thread (P6-T3)
    if (wdThread.joinable()) wdThread.join();

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
