// main.cpp — WiFi Offload Manager daemon entry point

#include "common/logger.hpp"
#include "config/config_loader.hpp"
#include "routing/routing_policy_manager.hpp"
#include "mptcp/mptcp_manager.hpp"
#include "wpa/wpa_monitor.hpp"
#include "fsm/path_state_fsm.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string_view>
#include <thread>

// ── Default paths ─────────────────────────────────────────────────
static constexpr std::string_view kDefaultConfigPath =
    "/etc/netservice/path-policies.json";
static constexpr std::string_view kDefaultWpaCtrl  = "/var/run/wpa_supplicant/wlan0";
static constexpr std::string_view kDefaultWpaIface = "wlan0";

// ── Shutdown flag set by signal handler ──────────────────────────
static std::atomic<bool> gShutdown{false};

static void handleSignal(int sig) noexcept {
    gShutdown.store(true, std::memory_order_relaxed);
    (void)sig;
}

// ── Argument parsing ─────────────────────────────────────────────
struct Args {
    std::string_view configPath{kDefaultConfigPath};
    std::string_view wpaCtrl{kDefaultWpaCtrl};
    std::string_view wpaIface{kDefaultWpaIface};
};

[[nodiscard]] static Args parseArgs(std::span<char*> argv) {
    Args args;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--config" && i + 1 < argv.size()) {
            args.configPath = argv[++i];
        } else if (arg == "--wpa-ctrl" && i + 1 < argv.size()) {
            args.wpaCtrl = argv[++i];
        } else if (arg == "--wpa-iface" && i + 1 < argv.size()) {
            args.wpaIface = argv[++i];
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

    // ── Phase 4: Path State FSM + wpa_supplicant monitor ─────────
    //
    // Find the PathClassConfig whose interface list includes the monitored
    // WiFi interface (--wpa-iface, default: wlan0).  Only one FSM is
    // instantiated per WiFi interface.
    const netservice::PathClassConfig* wifiClass = nullptr;
    for (const auto& cls : pathClasses) {
        for (const auto& iface : cls.interfaces) {
            if (iface == args.wpaIface) {
                wifiClass = &cls;
                break;
            }
        }
        if (wifiClass) break;
    }

    std::unique_ptr<netservice::WpaMonitor>  wpaMonitor;
    std::unique_ptr<netservice::PathStateFsm> fsm;
    std::thread wpaThread;

    if (wifiClass) {
        // Wire FSM callback → route + MPTCP side-effects (P4-T3 / P4-T4).
        fsm = std::make_unique<netservice::PathStateFsm>(
            *wifiClass,
            [&routingMgr, &mptcpMgr, wifiClass]
            (netservice::PathState newState, std::string_view iface) {
                if (newState == netservice::PathState::PathUp) {
                    // P4-T3: add WiFi route + MPTCP endpoint
                    if (auto r = routingMgr.addWifiRoute(iface, wifiClass->routingTable); !r) {
                        logger::error("[MAIN] addWifiRoute failed: {}",
                                      netservice::toString(r.error()));
                    }
                    if (wifiClass->mptcpEnabled) {
                        if (auto r = mptcpMgr.addEndpointForIface(iface); !r) {
                            logger::error("[MAIN] addEndpointForIface failed: {}",
                                          netservice::toString(r.error()));
                        }
                    }
                } else if (newState == netservice::PathState::PathDown) {
                    // P4-T4: remove WiFi route + MPTCP endpoint
                    routingMgr.removeWifiRoute(iface, wifiClass->routingTable);
                    if (wifiClass->mptcpEnabled) {
                        mptcpMgr.removeEndpointForIface(iface);
                    }
                }
            });

        wpaMonitor = std::make_unique<netservice::WpaMonitor>(
            std::string{args.wpaCtrl}, std::string{args.wpaIface});

        wpaMonitor->setEventCallback(
            [&fsm](const netservice::WpaEvent& ev) {
                fsm->onWpaEvent(ev);
            });

        // WpaMonitor::start() blocks the calling thread → run in a thread.
        wpaThread = std::thread([&wpaMonitor]() {
            if (auto r = wpaMonitor->start(); !r) {
                logger::warn("[MAIN] WpaMonitor start failed: {}",
                             netservice::toString(r.error()));
            }
        });

        logger::info("[MAIN] FSM started: class={} wpa-ctrl={}",
                     wifiClass->id, args.wpaCtrl);
    } else {
        logger::warn("[MAIN] no path class for wpa-iface={} — FSM disabled",
                     args.wpaIface);
    }

    // TODO Phase 5: pass pathClasses to ConsumerApiServer

    logger::info("[MAIN] startup complete — entering event loop");

    // ── Main event loop ───────────────────────────────────────────
    while (!gShutdown.load(std::memory_order_relaxed)) {
        pause();
    }

    logger::info("[MAIN] SIGTERM received — shutting down");

    // Stop WpaMonitor and wait for its thread to exit.
    if (wpaMonitor) {
        wpaMonitor->stop();
    }
    if (wpaThread.joinable()) {
        wpaThread.join();
    }

    // Phase 4: flush MPTCP endpoints
    mptcpMgr.removeEndpoints();

    // Phase 2: remove cgroup dirs
    routingMgr.cleanup();

    logger::info("[MAIN] shutdown complete");
    logger::close();
    return EXIT_SUCCESS;
}
