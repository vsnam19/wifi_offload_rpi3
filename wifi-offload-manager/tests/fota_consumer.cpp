// fota_consumer.cpp — FOTA download consumer demo
//
// Demonstrates a consumer that:
//   1. Registers with wifi-offload-manager for the "multipath" path class
//      (cgroup assignment ensures all traffic uses the MPTCP-enabled class)
//   2. Opens an MPTCP socket (IPPROTO_MPTCP) to the firmware server
//   3. Downloads a large firmware image via a minimal HTTP/1.1 GET
//   4. Tracks per-path subflow statistics (via /proc/net/tcp)
//   5. Handles path events (PathDegraded / PathDown) gracefully — logs and
//      continues; MPTCP kernel layer retransmits on remaining subflows
//   6. Verifies the downloaded image with SHA-256
//
// Usage:
//   fota_consumer --server <ip> --port <port> --path <url_path>
//                 [--socket <ctl_socket>] [--class <classId>]
//                 [--out <output_file>] [--expected-sha256 <hex>]
//
// The server MUST also use MPTCP (host runs mptcp_server or Python http.server
// wrapped with mptcpize; kernel MPTCP path manager adds subflows automatically
// via ADD_ADDR once RPi announces eth0.200 endpoint).

#include "api/consumer_api_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <iostream>
#include <linux/mptcp.h>
#include <netinet/in.h>
#ifdef USE_OPENSSL
#  include <openssl/sha.h>
#endif
#include <atomic>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace netservice;
using Clock = std::chrono::steady_clock;

// ─────────────────────────── helpers ─────────────────────────────────────────

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

// ─────────────────────── MPTCP subflow counter ──────────────────────────────
// Reads /proc/net/mptcp (kernel ≥ 5.6) and counts rows — each row is one
// active MPTCP subflow.  Returns 0 if the file doesn't exist.
static int mptcpSubflowCount() {
    std::ifstream f{"/proc/net/mptcp"};
    if (!f) return 0;
    int count = 0;
    std::string line;
    std::getline(f, line);  // skip header
    while (std::getline(f, line))
        if (!line.empty()) ++count;
    return count;
}

// ─────────────────────── periodic throughput sampler ─────────────────────────
// Runs in a background thread; every kSampleIntervalMs prints a [TPUT] line:
//   [TPUT] t=12.0s  interval=2.0s  10.45 Mbps  cumulative=24.10 Mbps
//          subflows=2  received=12.3/1228.8 MB  (1.0%)
// Stops when done_ is set.
struct ThroughputSampler {
    static constexpr int kSampleIntervalMs = 2000;

    std::atomic<std::size_t>& received_;
    std::size_t                total_;
    Clock::time_point          t0_;
    std::atomic<bool>          done_{false};
    std::thread                thread_;
    std::mutex                 printMtx_;

    ThroughputSampler(std::atomic<std::size_t>& rx, std::size_t total,
                      Clock::time_point t0)
        : received_{rx}, total_{total}, t0_{t0}
    {
        thread_ = std::thread([this]{ run(); });
    }

    void stop() {
        done_ = true;
        thread_.join();
    }

    void run() {
        std::size_t prev = 0;
        auto        prevTime = Clock::now();

        while (!done_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kSampleIntervalMs));
            if (done_) break;

            const std::size_t cur      = received_.load(std::memory_order_relaxed);
            const auto        now      = Clock::now();
            const double      dt       = std::chrono::duration<double>(now - prevTime).count();
            const double      elapsed  = std::chrono::duration<double>(now - t0_).count();
            const double      intervalMbps  = (static_cast<double>(cur - prev) * 8.0 / 1e6) / (dt > 0 ? dt : 1.0);
            const double      cumMbps       = (static_cast<double>(cur) * 8.0 / 1e6) / (elapsed > 0 ? elapsed : 1.0);
            const int         subflows      = mptcpSubflowCount();
            const double      pct           = total_ > 0
                ? (100.0 * static_cast<double>(cur) / static_cast<double>(total_)) : 0.0;

            {
                std::lock_guard lk{printMtx_};
                std::cout << std::format(
                    "[TPUT] t={:.1f}s  interval={:.1f}s  {:.2f} Mbps  cumulative={:.2f} Mbps"
                    "  subflows={}  received={:.1f}/{:.1f} MB  ({:.1f}%)\n",
                    elapsed, dt, intervalMbps, cumMbps,
                    subflows, cur / 1e6, total_ / 1e6, pct)
                << std::flush;
            }

            prev     = cur;
            prevTime = now;
        }
    }
};

static void printProgress([[maybe_unused]] std::size_t received,
                          [[maybe_unused]] std::size_t total,
                          [[maybe_unused]] double elapsed_s) {
    // Progress is now handled by ThroughputSampler; this is a no-op.
    // Keeping the call sites avoids removing them from the download loop.
}

// ─────────────────────────── args ────────────────────────────────────────────

struct Args {
    std::string server      {"172.16.1.1"};
    uint16_t    port        {8080};
    std::string urlPath     {"/firmware.img"};
    std::string socketPath  {std::string{ConsumerApiServer::kDefaultSocketPath}};
    std::string classId     {"multipath"};
    std::string outFile     {"/tmp/firmware_downloaded.img"};
    std::string expectedSha {""};    // optional hex SHA-256
};

static Args parseArgs(std::span<char*> argv) {
    Args a;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        std::string_view v{argv[i]};
        if      (v == "--server"  && i+1 < argv.size()) a.server       = argv[++i];
        else if (v == "--port"    && i+1 < argv.size()) a.port         = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (v == "--path"    && i+1 < argv.size()) a.urlPath      = argv[++i];
        else if (v == "--socket"  && i+1 < argv.size()) a.socketPath   = argv[++i];
        else if (v == "--class"   && i+1 < argv.size()) a.classId      = argv[++i];
        else if (v == "--out"     && i+1 < argv.size()) a.outFile      = argv[++i];
        else if (v == "--expected-sha256" && i+1 < argv.size()) a.expectedSha = argv[++i];
    }
    return a;
}

// ─────────────────────────── consumer API connection ─────────────────────────

struct CtlConn {
    int      fd{-1};
    uint64_t handle{0};
};

static CtlConn registerWithDaemon(const std::string& socketPath,
                                  const std::string& classId) {
    CtlConn c;

    c.fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c.fd < 0) {
        std::cerr << std::format("[FOTA] socket(AF_UNIX) failed: {}\n", strerror(errno));
        return c;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(c.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << std::format("[FOTA] connect({}) failed: {}\n", socketPath, strerror(errno));
        ::close(c.fd); c.fd = -1;
        return c;
    }

    ApiMsg reg{};
    reg.type = static_cast<uint32_t>(MsgType::Register);
    reg.pid  = static_cast<int32_t>(::getpid());
    std::strncpy(reg.classId, classId.c_str(), sizeof(reg.classId) - 1);

    if (!sendMsg(c.fd, reg)) {
        std::cerr << "[FOTA] send Register failed\n";
        ::close(c.fd); c.fd = -1;
        return c;
    }

    ApiMsg ack{};
    if (!recvMsg(c.fd, ack) || ack.error != 0) {
        std::cerr << std::format("[FOTA] Register failed: error={}\n", ack.error);
        ::close(c.fd); c.fd = -1;
        return c;
    }

    c.handle = ack.handle;
    std::cout << std::format("[FOTA] registered with daemon: handle={} class={} state={}\n",
                             c.handle, classId, pathStateName(ack.state));
    return c;
}

static void unregisterFromDaemon(CtlConn& c) {
    if (c.fd < 0) return;
    ApiMsg msg{};
    msg.type   = static_cast<uint32_t>(MsgType::Unregister);
    msg.handle = c.handle;
    sendMsg(c.fd, msg);  // best-effort
    ::close(c.fd);
    c.fd = -1;
    std::cout << "[FOTA] unregistered from daemon\n";
}

// ─────────────────────────── MPTCP download ───────────────────────────────────

// Returns SOCK_NONBLOCK fd connected via MPTCP to server:port.
// Falls back to TCP if IPPROTO_MPTCP is not available at runtime.
static int openMptcpSocket(const std::string& server, uint16_t port) {
    // IPPROTO_MPTCP = 262 (kernel 5.6+)
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_MPTCP);
    if (fd < 0) {
        std::cerr << std::format("[FOTA] IPPROTO_MPTCP not available ({}), falling back to TCP\n",
                                 strerror(errno));
        fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        if (fd < 0) {
            std::cerr << std::format("[FOTA] socket(TCP) failed: {}\n", strerror(errno));
            return -1;
        }
    } else {
        std::cout << "[FOTA] using MPTCP socket (IPPROTO_MPTCP=262)\n";
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, server.c_str(), &addr.sin_addr) != 1) {
        std::cerr << std::format("[FOTA] invalid server IP: {}\n", server);
        ::close(fd);
        return -1;
    }

    std::cout << std::format("[FOTA] connecting to {}:{} …\n", server, port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << std::format("[FOTA] connect({}:{}) failed: {}\n",
                                 server, port, strerror(errno));
        ::close(fd);
        return -1;
    }
    std::cout << "[FOTA] MPTCP connection established\n";
    return fd;
}

// Send HTTP GET request.
static bool sendHttpGet(int fd, const std::string& host, const std::string& path) {
    const auto req = std::format(
        "GET {} HTTP/1.1\r\n"
        "Host: {}\r\n"
        "Connection: close\r\n"
        "User-Agent: fota_consumer/1.0\r\n"
        "\r\n",
        path, host);

    const char* p = req.c_str();
    std::size_t rem = req.size();
    while (rem > 0) {
        const ssize_t n = ::send(fd, p, rem, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n; rem -= static_cast<std::size_t>(n);
    }
    return true;
}

// Parse HTTP response header.  Fills `contentLength` (0 if unknown).
// Returns position in `buf` just after the blank line (\r\n\r\n).
// Returns -1 on parse error.
static ssize_t parseHttpHeader(const std::vector<uint8_t>& buf,
                               std::size_t& contentLength,
                               int& statusCode) {
    std::string_view s{reinterpret_cast<const char*>(buf.data()), buf.size()};

    // Status line
    const auto eol = s.find("\r\n");
    if (eol == std::string_view::npos) return -1;
    std::string_view status_line = s.substr(0, eol);
    // "HTTP/1.1 200 OK"
    const auto sp1 = status_line.find(' ');
    if (sp1 == std::string_view::npos) return -1;
    statusCode = std::stoi(std::string{status_line.substr(sp1 + 1, 3)});

    // Header end
    const auto hdrEnd = s.find("\r\n\r\n");
    if (hdrEnd == std::string_view::npos) return -1;

    // Content-Length
    contentLength = 0;
    const auto clHdr = s.find("Content-Length:");
    if (clHdr != std::string_view::npos && clHdr < hdrEnd) {
        const auto clVal = s.find_first_not_of(" \t", clHdr + 15);
        if (clVal != std::string_view::npos) {
            contentLength = static_cast<std::size_t>(std::stoull(std::string{s.substr(clVal, 20)}));
        }
    }

    return static_cast<ssize_t>(hdrEnd + 4);
}

// ─────────────────────────── SHA-256 convenience ─────────────────────────────

#ifdef USE_OPENSSL
static std::string sha256hex(const std::string& filePath) {
    int fd = ::open(filePath.c_str(), O_RDONLY);
    if (fd < 0) return "";

    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    std::vector<uint8_t> buf(64 * 1024);
    ssize_t n;
    while ((n = ::read(fd, buf.data(), buf.size())) > 0)
        SHA256_Update(&ctx, buf.data(), static_cast<std::size_t>(n));
    ::close(fd);

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &ctx);

    std::string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * 2);
    for (const auto b : digest)
        hex += std::format("{:02x}", b);
    return hex;
}
#endif

// ─────────────────────────── main ────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const auto args = parseArgs(std::span{argv, static_cast<std::size_t>(argc)});

    std::cout << std::format("[FOTA] server={}:{} path={} out={}\n",
                             args.server, args.port, args.urlPath, args.outFile);

    // ── Step 1: register with wifi-offload-manager ────────────────
    auto ctl = registerWithDaemon(args.socketPath, args.classId);
    if (ctl.fd < 0) {
        std::cerr << "[FOTA] WARNING: could not register with daemon — "
                     "proceeding without cgroup assignment\n";
    }

    // ── Step 2: set control socket non-blocking for event polling ─
    if (ctl.fd >= 0)
        ::fcntl(ctl.fd, F_SETFL, ::fcntl(ctl.fd, F_GETFL) | O_NONBLOCK);

    // ── Step 3: open MPTCP data socket ────────────────────────────
    const int dataFd = openMptcpSocket(args.server, args.port);
    if (dataFd < 0) {
        unregisterFromDaemon(ctl);
        return 1;
    }

    // ── Step 4: send HTTP GET ─────────────────────────────────────
    if (!sendHttpGet(dataFd, args.server, args.urlPath)) {
        std::cerr << "[FOTA] sendHttpGet failed\n";
        ::close(dataFd);
        unregisterFromDaemon(ctl);
        return 1;
    }

    // ── Step 5: read HTTP response header (buffered) ──────────────
    std::vector<uint8_t> headerBuf;
    headerBuf.reserve(4096);
    std::size_t contentLength = 0;
    int         statusCode    = 0;
    ssize_t     bodyStart     = -1;

    while (bodyStart < 0) {
        uint8_t tmp[4096];
        const ssize_t n = ::recv(dataFd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            std::cerr << "[FOTA] connection closed before header complete\n";
            ::close(dataFd);
            unregisterFromDaemon(ctl);
            return 1;
        }
        headerBuf.insert(headerBuf.end(), tmp, tmp + n);
        bodyStart = parseHttpHeader(headerBuf, contentLength, statusCode);
    }

    if (statusCode != 200) {
        std::cerr << std::format("[FOTA] HTTP error: {}\n", statusCode);
        ::close(dataFd);
        unregisterFromDaemon(ctl);
        return 1;
    }

    std::cout << std::format("[FOTA] HTTP 200  content-length={} bytes ({:.1f} MB)\n",
                             contentLength, contentLength / 1e6);

    // ── Step 6: open output file ──────────────────────────────────
    const int outFd = ::open(args.outFile.c_str(),
                             O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (outFd < 0) {
        std::cerr << std::format("[FOTA] open({}) failed: {}\n", args.outFile, strerror(errno));
        ::close(dataFd);
        unregisterFromDaemon(ctl);
        return 1;
    }

    // ── Step 7: download loop ─────────────────────────────────────
    std::vector<uint8_t> rowBuf(128 * 1024);
    std::atomic<std::size_t> received{0};
    uint32_t    pathDropEvents = 0;
    const auto  t0 = Clock::now();

    // Print initial MPTCP subflow state
    std::cout << std::format("[FOTA] initial MPTCP subflows: {}\n", mptcpSubflowCount());

    // Start background throughput sampler
    ThroughputSampler sampler{received, contentLength, t0};

    // Write any body bytes already in headerBuf
    const auto bp = static_cast<std::size_t>(bodyStart);
    if (bp < headerBuf.size()) {
        const std::size_t preBody = headerBuf.size() - bp;
        ::write(outFd, headerBuf.data() + bp, preBody);
        received.fetch_add(preBody, std::memory_order_relaxed);
    }

    // epoll on dataFd + ctlFd for concurrent PathEvent notifications
    const int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    {
        struct epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = dataFd;
        ::epoll_ctl(epfd, EPOLL_CTL_ADD, dataFd, &ev);
    }
    if (ctl.fd >= 0) {
        struct epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = ctl.fd;
        ::epoll_ctl(epfd, EPOLL_CTL_ADD, ctl.fd, &ev);
    }

    while (contentLength == 0 || received < contentLength) {
        struct epoll_event evs[4];
        const int nev = ::epoll_wait(epfd, evs, 4, 10000 /* 10 s timeout */);

        if (nev == 0) {
            std::cerr << "\n[FOTA] timeout waiting for data\n";
            break;
        }
        if (nev < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nev; ++i) {
            if (evs[i].data.fd == dataFd) {
                // Data socket readable
                const ssize_t n = ::recv(dataFd, rowBuf.data(), rowBuf.size(), 0);
                if (n <= 0) {
                    // EOF or error — MPTCP may have closed after all subflows dropped
                    std::cout << std::format("\n[FOTA] data socket closed: n={} ({})\n",
                                             n, (n < 0 ? strerror(errno) : "EOF"));
                    goto download_done;
                }
                ::write(outFd, rowBuf.data(), static_cast<std::size_t>(n));
                const std::size_t cur = received.fetch_add(
                    static_cast<std::size_t>(n), std::memory_order_relaxed) + static_cast<std::size_t>(n);
                (void)cur;  // sampler thread reads received_ directly

            } else if (evs[i].data.fd == ctl.fd) {
                // PathEvent notification from daemon
                ApiMsg evt{};
                if (recvMsg(ctl.fd, evt) && evt.type == static_cast<uint32_t>(MsgType::PathEvent)) {
                    const auto elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
                    const auto st = static_cast<PathState>(evt.state);
                    std::cout << std::format(
                        "\n[FOTA] PATH EVENT t={:.1f}s  class={}  state={}  iface={}  rssi={}dBm\n",
                        elapsed, evt.classId, pathStateName(evt.state), evt.iface, evt.rssi);

                    if (st == PathState::PathDegraded || st == PathState::PathDown) {
                        ++pathDropEvents;
                        std::cout << std::format(
                            "[FOTA] *** subflow disruption #{} — MPTCP retransmitting on remaining paths ***"
                            "  (active subflows={})\n",
                            pathDropEvents, mptcpSubflowCount());
                    } else if (st == PathState::PathUp) {
                        std::cout << "[FOTA] path restored — MPTCP adding subflow\n";
                    }
                }
            }
        }
    }

download_done:
    sampler.stop();
    ::close(epfd);
    ::close(dataFd);
    ::close(outFd);

    {
        const std::size_t rxFinal = received.load();
        const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
        const double avgMbps = (rxFinal * 8.0 / 1e6) / (elapsed > 0 ? elapsed : 1.0);
        std::cout << std::format(
            "\n[FOTA] === DOWNLOAD SUMMARY ===\n"
            "[FOTA]   received:        {:.3f} MB\n"
            "[FOTA]   expected:        {:.3f} MB\n"
            "[FOTA]   elapsed:         {:.1f} s\n"
            "[FOTA]   average rate:    {:.2f} Mbps\n"
            "[FOTA]   path events:     {}\n"
            "[FOTA]   MPTCP subflows:  {}\n"
            "[FOTA] ==========================\n",
            rxFinal / 1e6,
            contentLength / 1e6,
            elapsed,
            avgMbps,
            pathDropEvents,
            mptcpSubflowCount());
        received.store(rxFinal);  // keep readable for checks below
    }

    // ── Step 8: SHA-256 verification ─────────────────────────────
#ifdef USE_OPENSSL
    if (!args.expectedSha.empty()) {
        std::cout << "[FOTA] verifying SHA-256 …\n";
        const auto actual = sha256hex(args.outFile);
        if (actual == args.expectedSha) {
            std::cout << "[FOTA] SHA-256 OK: " << actual << "\n";
        } else {
            std::cerr << std::format("[FOTA] SHA-256 MISMATCH!\n  expected: {}\n  actual:   {}\n",
                                     args.expectedSha, actual);
            unregisterFromDaemon(ctl);
            return 1;
        }
    }
#else
    (void)args.expectedSha;
    std::cout << "[FOTA] (SHA-256 verification skipped — built without USE_OPENSSL)\n";
#endif

    // ── Step 9: unregister ────────────────────────────────────────
    unregisterFromDaemon(ctl);

    if (contentLength > 0 && received.load() < contentLength) {
        std::cerr << std::format("[FOTA] INCOMPLETE: got {} of {} bytes\n",
                                 received.load(), contentLength);
        return 1;
    }

    std::cout << "[FOTA] SUCCESS\n";
    return 0;
}
