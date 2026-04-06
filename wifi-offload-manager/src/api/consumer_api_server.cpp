// consumer_api_server.cpp — P5-T1..T6: Consumer Registration API implementation

#include "api/consumer_api_server.hpp"
#include "common/logger.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <fstream>

// POSIX / Linux
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace netservice {

// ── Construction / Destruction ────────────────────────────────────────────────

ConsumerApiServer::ConsumerApiServer(
    const std::vector<PathClassConfig>& classes,
    std::string_view socketPath) noexcept
    : socketPath_{socketPath}
{
    classStates_.reserve(classes.size());
    for (const auto& cls : classes) {
        classStates_.push_back(ClassState{
            .classId    = cls.id,
            .cgroupPath = cls.cgroupPath,
            .state      = PathState::Idle,
            .activeIface = {},
            .rssi       = 0,
        });
    }
}

ConsumerApiServer::~ConsumerApiServer() {
    stop();
}

// ── start ─────────────────────────────────────────────────────────────────────

std::expected<void, ApiError> ConsumerApiServer::start() {
    // Create parent directory if needed.
    {
        const auto parentDir = socketPath_.substr(0, socketPath_.rfind('/'));
        if (!parentDir.empty()) {
            ::mkdir(parentDir.c_str(), 0755);  // ignore EEXIST
        }
    }

    // Remove stale socket file.
    ::unlink(socketPath_.c_str());

    // Create listen socket.
    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) {
        logger::error("[API] socket() failed: errno={} ({})", errno, std::strerror(errno));
        return std::unexpected(ApiError::SocketError);
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        logger::error("[API] bind({}) failed: errno={} ({})",
                      socketPath_, errno, std::strerror(errno));
        ::close(listenFd_); listenFd_ = -1;
        return std::unexpected(ApiError::SocketError);
    }
    ::chmod(socketPath_.c_str(), 0666);  // allow non-root consumers

    if (::listen(listenFd_, 16) < 0) {
        logger::error("[API] listen() failed: errno={} ({})", errno, std::strerror(errno));
        ::close(listenFd_); listenFd_ = -1;
        return std::unexpected(ApiError::SocketError);
    }

    // Create epoll + wakeupFd.
    epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0) {
        logger::error("[API] epoll_create1 failed: errno={} ({})", errno, std::strerror(errno));
        ::close(listenFd_); listenFd_ = -1;
        return std::unexpected(ApiError::SocketError);
    }

    wakeupFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeupFd_ < 0) {
        logger::error("[API] eventfd failed: errno={} ({})", errno, std::strerror(errno));
        ::close(epollFd_); epollFd_ = -1;
        ::close(listenFd_); listenFd_ = -1;
        return std::unexpected(ApiError::SocketError);
    }

    auto epollAdd = [this](int fd, uint32_t events) -> bool {
        struct epoll_event ev{};
        ev.events  = events;
        ev.data.fd = fd;
        return ::epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) == 0;
    };

    if (!epollAdd(listenFd_, EPOLLIN) || !epollAdd(wakeupFd_, EPOLLIN)) {
        logger::error("[API] epoll_ctl add failed: errno={} ({})", errno, std::strerror(errno));
        ::close(wakeupFd_); wakeupFd_ = -1;
        ::close(epollFd_);  epollFd_ = -1;
        ::close(listenFd_); listenFd_ = -1;
        return std::unexpected(ApiError::SocketError);
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread{[this] { runLoop(); }};

    logger::info("[API] server started: socket={}", socketPath_);
    return {};
}

// ── stop ──────────────────────────────────────────────────────────────────────

void ConsumerApiServer::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    // Wake up epoll loop.
    if (wakeupFd_ >= 0) {
        const uint64_t one = 1;
        [[maybe_unused]] const ssize_t ignored = ::write(wakeupFd_, &one, sizeof(one));
    }
    if (thread_.joinable()) thread_.join();

    // Close all client fds.
    {
        std::scoped_lock lock{registryMutex_};
        for (const auto& entry : registry_) {
            ::close(entry.fd);
        }
        registry_.clear();
    }

    if (wakeupFd_ >= 0) { ::close(wakeupFd_); wakeupFd_ = -1; }
    if (epollFd_  >= 0) { ::close(epollFd_);  epollFd_  = -1; }
    if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }

    ::unlink(socketPath_.c_str());
    logger::info("[API] server stopped");
}

// ── runLoop ───────────────────────────────────────────────────────────────────

void ConsumerApiServer::runLoop() {
    constexpr int kMaxEvents = 32;
    struct epoll_event events[kMaxEvents];

    while (running_.load(std::memory_order_acquire)) {
        const int n = ::epoll_wait(epollFd_, events, kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            logger::error("[API] epoll_wait failed: errno={} ({})", errno, std::strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;

            if (fd == wakeupFd_) {
                // stop() was called
                return;
            } else if (fd == listenFd_) {
                handleAccept();
            } else {
                if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                    removeClient(fd);
                } else if (events[i].events & EPOLLIN) {
                    handleClient(fd);
                }
            }
        }
    }
}

// ── handleAccept ─────────────────────────────────────────────────────────────

void ConsumerApiServer::handleAccept() {
    for (;;) {
        const int clientFd = ::accept4(listenFd_, nullptr, nullptr,
                                       SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (clientFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            logger::warn("[API] accept4 failed: errno={} ({})", errno, std::strerror(errno));
            break;
        }

        struct epoll_event ev{};
        ev.events  = EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP;
        ev.data.fd = clientFd;
        if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, clientFd, &ev) < 0) {
            logger::warn("[API] epoll_ctl add client failed: errno={} ({})",
                         errno, std::strerror(errno));
            ::close(clientFd);
        } else {
            logger::info("[API] client connected fd={}", clientFd);
        }
    }
}

// ── handleClient ─────────────────────────────────────────────────────────────

void ConsumerApiServer::handleClient(int fd) {
    ApiMsg msg{};
    if (!recvMsg(fd, msg)) {
        removeClient(fd);
        return;
    }

    switch (static_cast<MsgType>(msg.type)) {
        case MsgType::Register:
            handleRegister(fd, msg);
            break;
        case MsgType::Unregister:
            handleUnregister(fd, msg);
            break;
        case MsgType::QueryCurrent:
            handleQueryCurrent(fd, msg);
            break;
        default:
            logger::warn("[API] unknown message type={} from fd={}", msg.type, fd);
            ApiMsg ack{};
            ack.type  = static_cast<uint32_t>(MsgType::RegisterAck);
            ack.error = static_cast<uint32_t>(ApiError::InvalidMessage);
            sendMsg(fd, ack);
            break;
    }
}

// ── removeClient ─────────────────────────────────────────────────────────────

void ConsumerApiServer::removeClient(int fd) noexcept {
    ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);

    std::scoped_lock lock{registryMutex_};
    auto it = std::find_if(registry_.begin(), registry_.end(),
                           [fd](const ConsumerEntry& e) { return e.fd == fd; });
    if (it != registry_.end()) {
        logger::info("[API] consumer unregistered on disconnect: pid={} class={} fd={}",
                     it->pid, it->classId, fd);
        removeCgroupPid(it->classId, it->pid);
        registry_.erase(it);
    }
}

// ── handleRegister ────────────────────────────────────────────────────────────

void ConsumerApiServer::handleRegister(int fd, const ApiMsg& msg) {
    const std::string classId{msg.classId,
        strnlen(msg.classId, sizeof(msg.classId))};

    ApiMsg ack{};
    ack.type = static_cast<uint32_t>(MsgType::RegisterAck);

    // Validate class ID.
    if (findClass(classId) == nullptr) {
        logger::warn("[API] Register: unknown classId='{}' from fd={}", classId, fd);
        ack.error = static_cast<uint32_t>(ApiError::UnknownClassId);
        sendMsg(fd, ack);
        return;
    }

    // Create registry entry.
    ConsumerEntry entry{
        .fd      = fd,
        .pid     = msg.pid,
        .handle  = 0,
        .classId = classId,
    };

    uint64_t handle{};
    {
        std::scoped_lock lock{registryMutex_};
        // Remove any stale entry for this fd (re-register).
        registry_.erase(
            std::remove_if(registry_.begin(), registry_.end(),
                           [fd](const ConsumerEntry& e) { return e.fd == fd; }),
            registry_.end());
        handle = nextHandle_++;
        entry.handle = handle;
        registry_.push_back(entry);
    }

    // P5-T4: assign PID to cgroup.
    assignCgroupPid(classId, msg.pid);

    ack.handle = handle;
    std::strncpy(ack.classId, classId.c_str(), sizeof(ack.classId) - 1);

    // Send back current path state as part of RegisterAck.
    if (const auto* cls = findClass(classId)) {
        ack.state = static_cast<uint32_t>(cls->state);
        std::strncpy(ack.iface, cls->activeIface.c_str(), sizeof(ack.iface) - 1);
        ack.rssi  = cls->rssi;
    }

    sendMsg(fd, ack);
    logger::info("[API] consumer registered: pid={} class={} handle={} fd={}",
                 msg.pid, classId, handle, fd);
}

// ── handleUnregister ─────────────────────────────────────────────────────────

void ConsumerApiServer::handleUnregister(int fd, const ApiMsg& msg) {
    ApiMsg ack{};
    ack.type   = static_cast<uint32_t>(MsgType::UnregisterAck);
    ack.handle = msg.handle;

    std::scoped_lock lock{registryMutex_};
    auto it = std::find_if(registry_.begin(), registry_.end(),
                           [&msg](const ConsumerEntry& e) {
                               return e.handle == msg.handle;
                           });
    if (it == registry_.end()) {
        ack.error = static_cast<uint32_t>(ApiError::InvalidMessage);
        sendMsg(fd, ack);
        return;
    }

    removeCgroupPid(it->classId, it->pid);
    registry_.erase(it);
    sendMsg(fd, ack);
    logger::info("[API] consumer unregistered: handle={} fd={}", msg.handle, fd);
}

// ── handleQueryCurrent ────────────────────────────────────────────────────────

void ConsumerApiServer::handleQueryCurrent(int fd, const ApiMsg& msg) {
    const std::string classId{msg.classId,
        strnlen(msg.classId, sizeof(msg.classId))};

    ApiMsg ack{};
    ack.type = static_cast<uint32_t>(MsgType::QueryAck);

    const auto* cls = findClass(classId);
    if (!cls) {
        ack.error = static_cast<uint32_t>(ApiError::UnknownClassId);
        sendMsg(fd, ack);
        return;
    }

    ack.state = static_cast<uint32_t>(cls->state);
    ack.rssi  = cls->rssi;
    std::strncpy(ack.classId, classId.c_str(), sizeof(ack.classId) - 1);
    std::strncpy(ack.iface, cls->activeIface.c_str(), sizeof(ack.iface) - 1);
    sendMsg(fd, ack);
}

// ── broadcastPathEvent ────────────────────────────────────────────────────────

void ConsumerApiServer::broadcastPathEvent(std::string_view classId,
                                           PathState        newState,
                                           std::string_view iface,
                                           int              rssiDbm) noexcept
{
    // Update cached state.
    updateCurrentState(classId, newState);
    if (auto* cls = findClass(classId)) {
        cls->activeIface = std::string{iface};
        cls->rssi        = rssiDbm;
    }

    ApiMsg msg{};
    msg.type  = static_cast<uint32_t>(MsgType::PathEvent);
    msg.state = static_cast<uint32_t>(newState);
    msg.rssi  = rssiDbm;
    std::strncpy(msg.classId, classId.data(),   sizeof(msg.classId) - 1);
    std::strncpy(msg.iface,   iface.data(),     sizeof(msg.iface)   - 1);

    std::scoped_lock lock{registryMutex_};
    std::vector<int> toRemove;
    for (const auto& entry : registry_) {
        if (entry.classId != classId) continue;
        if (!trySendMsg(entry.fd, msg)) {
            toRemove.push_back(entry.fd);
        }
    }
    // Remove broken connections found during broadcast.
    for (int fd : toRemove) {
        // removeClient() acquires the mutex — can't call it here; defer.
        // Post-broadcast cleanup: close fd and erase. The epoll edge will also
        // fire but removeClient is idempotent (checks fd in registry).
        auto it = std::find_if(registry_.begin(), registry_.end(),
                               [fd](const ConsumerEntry& e) { return e.fd == fd; });
        if (it != registry_.end()) {
            logger::warn("[API] broadcast: removing dead consumer pid={} fd={}", it->pid, fd);
            removeCgroupPid(it->classId, it->pid);
            registry_.erase(it);
        }
        ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
    }
}

// ── queryCurrentState / updateCurrentState ───────────────────────────────────

PathState ConsumerApiServer::queryCurrentState(std::string_view classId) const noexcept {
    const auto* cls = findClass(classId);
    return cls ? cls->state : PathState::Idle;
}

void ConsumerApiServer::updateCurrentState(std::string_view classId,
                                           PathState state) noexcept {
    if (auto* cls = findClass(classId)) {
        cls->state = state;
    }
}

// ── cgroup helpers ────────────────────────────────────────────────────────────

void ConsumerApiServer::assignCgroupPid(std::string_view classId,
                                        int32_t pid) noexcept
{
    const auto* cls = findClass(classId);
    if (!cls) return;

    // cgroup v1: write to 'tasks' file
    const std::string tasksPath = cls->cgroupPath + "/tasks";
    std::ofstream f{tasksPath, std::ios::app};
    if (!f.is_open()) {
        logger::warn("[API] assignCgroupPid: cannot open {}: errno={} ({})",
                     tasksPath, errno, std::strerror(errno));
        return;
    }
    f << pid << "\n";
    logger::info("[API] assigned pid={} to cgroup {}", pid, cls->cgroupPath);
}

void ConsumerApiServer::removeCgroupPid(std::string_view classId,
                                        int32_t pid) noexcept
{
    // On cgroup v1, a task is removed from a cgroup by writing its PID to
    // the *root* net_cls tasks file (echo pid > /sys/fs/cgroup/net_cls/tasks).
    // If the process is already dead, this is a no-op.
    const auto* cls = findClass(classId);
    if (!cls) return;

    constexpr std::string_view kCgroupRoot = "/sys/fs/cgroup/net_cls/tasks";
    std::ofstream f{std::string{kCgroupRoot}, std::ios::app};
    if (!f.is_open()) {
        // Not a fatal error — process may have already exited.
        logger::warn("[API] removeCgroupPid: cannot open {}", kCgroupRoot);
        return;
    }
    f << pid << "\n";
    logger::info("[API] moved pid={} back to cgroup root (was {})", pid, cls->cgroupPath);
}

// ── helpers ───────────────────────────────────────────────────────────────────

ConsumerApiServer::ClassState*
ConsumerApiServer::findClass(std::string_view classId) noexcept {
    for (auto& cs : classStates_) {
        if (cs.classId == classId) return &cs;
    }
    return nullptr;
}

const ConsumerApiServer::ClassState*
ConsumerApiServer::findClass(std::string_view classId) const noexcept {
    for (const auto& cs : classStates_) {
        if (cs.classId == classId) return &cs;
    }
    return nullptr;
}

void ConsumerApiServer::sendMsg(int fd, const ApiMsg& msg) noexcept {
    const auto* p   = reinterpret_cast<const char*>(&msg);
    std::size_t rem = sizeof(ApiMsg);
    while (rem > 0) {
        const ssize_t n = ::send(fd, p, rem, MSG_NOSIGNAL);
        if (n <= 0) return;
        p   += n;
        rem -= static_cast<std::size_t>(n);
    }
}

// trySendMsg: same as sendMsg but returns false on error (used in broadcast).
bool ConsumerApiServer::trySendMsg(int fd, const ApiMsg& msg) noexcept {
    const auto* p   = reinterpret_cast<const char*>(&msg);
    std::size_t rem = sizeof(ApiMsg);
    while (rem > 0) {
        const ssize_t n = ::send(fd, p, rem, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p   += static_cast<std::size_t>(n);
        rem -= static_cast<std::size_t>(n);
    }
    return true;
}

bool ConsumerApiServer::recvMsg(int fd, ApiMsg& msg) noexcept {
    auto* p         = reinterpret_cast<char*>(&msg);
    std::size_t rem = sizeof(ApiMsg);
    while (rem > 0) {
        const ssize_t n = ::recv(fd, p, rem, MSG_WAITALL);
        if (n <= 0) return false;
        p   += n;
        rem -= static_cast<std::size_t>(n);
    }
    return true;
}

} // namespace netservice
