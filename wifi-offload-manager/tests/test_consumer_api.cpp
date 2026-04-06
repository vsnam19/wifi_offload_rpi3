// test_consumer_api.cpp — Phase 5 unit tests for ConsumerApiServer
//
// Tests run in-process: start the server on a temp socket, connect a raw
// Unix client socket, and exercise the wire protocol directly.

#include <gtest/gtest.h>

#include "api/consumer_api_server.hpp"
#include "common/types.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace netservice;

// ── Helpers ───────────────────────────────────────────────────────────────────

static constexpr std::string_view kTestSocket = "/tmp/test_consumer_api.sock";

static std::vector<PathClassConfig> makeClasses() {
    return {
        PathClassConfig{
            .id           = "multipath",
            .classid      = 0x00010001u,
            .cgroupPath   = "/tmp/cgroup_test",
            .interfaces   = {"wlan0"},
            .mptcpEnabled = true,
            .routingTable = 200,
            .mark         = 0x10,
            .strictIsolation = false,
            .rssi         = {},
        },
        PathClassConfig{
            .id           = "lte_b2c",
            .classid      = 0x00010002u,
            .cgroupPath   = "/tmp/cgroup_b2c",
            .interfaces   = {"usb0"},
            .mptcpEnabled = false,
            .routingTable = 201,
            .mark         = 0x20,
            .strictIsolation = false,
            .rssi         = {},
        },
    };
}

// Build and connect a client socket to the test server.
static int connectClient(std::string_view path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.data(), sizeof(addr.sun_path) - 1);

    for (int i = 0; i < 50; ++i) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            return fd;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::close(fd);
    return -1;
}

static bool sendRaw(int fd, const ApiMsg& msg) {
    const char* p = reinterpret_cast<const char*>(&msg);
    std::size_t rem = sizeof(ApiMsg);
    while (rem > 0) {
        ssize_t n = ::send(fd, p, rem, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n; rem -= static_cast<std::size_t>(n);
    }
    return true;
}

static bool recvRaw(int fd, ApiMsg& msg) {
    char* p = reinterpret_cast<char*>(&msg);
    std::size_t rem = sizeof(ApiMsg);
    while (rem > 0) {
        ssize_t n = ::recv(fd, p, rem, MSG_WAITALL);
        if (n <= 0) return false;
        p += n; rem -= static_cast<std::size_t>(n);
    }
    return true;
}

// ── Fixture ───────────────────────────────────────────────────────────────────

class ConsumerApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        ::unlink(kTestSocket.data());
        server_ = std::make_unique<ConsumerApiServer>(makeClasses(), kTestSocket);
        ASSERT_TRUE(server_->start().has_value());
        // Give the epoll thread a moment to start listening.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    void TearDown() override {
        server_->stop();
    }

    std::unique_ptr<ConsumerApiServer> server_;
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(ConsumerApiTest, RegisterAndReceiveAck) {
    int fd = connectClient(kTestSocket);
    ASSERT_GE(fd, 0) << "could not connect to server socket";

    // Send Register message.
    ApiMsg req{};
    req.type = static_cast<uint32_t>(MsgType::Register);
    req.pid  = static_cast<int32_t>(::getpid());
    std::strncpy(req.classId, "multipath", sizeof(req.classId) - 1);

    ASSERT_TRUE(sendRaw(fd, req));

    // Receive RegisterAck.
    ApiMsg ack{};
    ASSERT_TRUE(recvRaw(fd, ack));
    EXPECT_EQ(ack.type,    static_cast<uint32_t>(MsgType::RegisterAck));
    EXPECT_EQ(ack.error,   0u);
    EXPECT_GT(ack.handle,  0u);
    EXPECT_STREQ(ack.classId, "multipath");

    ::close(fd);
}

TEST_F(ConsumerApiTest, RegisterUnknownClassReturnsError) {
    int fd = connectClient(kTestSocket);
    ASSERT_GE(fd, 0);

    ApiMsg req{};
    req.type = static_cast<uint32_t>(MsgType::Register);
    req.pid  = static_cast<int32_t>(::getpid());
    std::strncpy(req.classId, "no_such_class", sizeof(req.classId) - 1);

    ASSERT_TRUE(sendRaw(fd, req));

    ApiMsg ack{};
    ASSERT_TRUE(recvRaw(fd, ack));
    EXPECT_EQ(ack.type,  static_cast<uint32_t>(MsgType::RegisterAck));
    EXPECT_EQ(ack.error, static_cast<uint32_t>(ApiError::UnknownClassId));

    ::close(fd);
}

TEST_F(ConsumerApiTest, RegisterThenUnregister) {
    int fd = connectClient(kTestSocket);
    ASSERT_GE(fd, 0);

    // Register.
    ApiMsg req{};
    req.type = static_cast<uint32_t>(MsgType::Register);
    req.pid  = static_cast<int32_t>(::getpid());
    std::strncpy(req.classId, "lte_b2c", sizeof(req.classId) - 1);
    ASSERT_TRUE(sendRaw(fd, req));

    ApiMsg regAck{};
    ASSERT_TRUE(recvRaw(fd, regAck));
    ASSERT_EQ(regAck.error, 0u);
    const uint64_t handle = regAck.handle;

    // Unregister.
    ApiMsg unreg{};
    unreg.type   = static_cast<uint32_t>(MsgType::Unregister);
    unreg.handle = handle;
    ASSERT_TRUE(sendRaw(fd, unreg));

    ApiMsg unregAck{};
    ASSERT_TRUE(recvRaw(fd, unregAck));
    EXPECT_EQ(unregAck.type,   static_cast<uint32_t>(MsgType::UnregisterAck));
    EXPECT_EQ(unregAck.error,  0u);
    EXPECT_EQ(unregAck.handle, handle);

    ::close(fd);
}

TEST_F(ConsumerApiTest, QueryCurrentStateReturnsIdle) {
    int fd = connectClient(kTestSocket);
    ASSERT_GE(fd, 0);

    // Initially Idle (no PathEvent sent).
    ApiMsg req{};
    req.type = static_cast<uint32_t>(MsgType::QueryCurrent);
    std::strncpy(req.classId, "multipath", sizeof(req.classId) - 1);
    ASSERT_TRUE(sendRaw(fd, req));

    ApiMsg ack{};
    ASSERT_TRUE(recvRaw(fd, ack));
    EXPECT_EQ(ack.type,  static_cast<uint32_t>(MsgType::QueryAck));
    EXPECT_EQ(ack.error, 0u);
    EXPECT_EQ(ack.state, static_cast<uint32_t>(PathState::Idle));

    ::close(fd);
}

TEST_F(ConsumerApiTest, BroadcastPathEventDelivered) {
    int fd = connectClient(kTestSocket);
    ASSERT_GE(fd, 0);

    // Register.
    ApiMsg req{};
    req.type = static_cast<uint32_t>(MsgType::Register);
    req.pid  = static_cast<int32_t>(::getpid());
    std::strncpy(req.classId, "multipath", sizeof(req.classId) - 1);
    ASSERT_TRUE(sendRaw(fd, req));

    ApiMsg regAck{};
    ASSERT_TRUE(recvRaw(fd, regAck));
    ASSERT_EQ(regAck.error, 0u);

    // Trigger a broadcast from another thread.
    std::thread t{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        server_->broadcastPathEvent("multipath", PathState::PathUp, "wlan0", -55);
    }};

    // Receive PathEvent.
    ApiMsg event{};
    ASSERT_TRUE(recvRaw(fd, event));
    EXPECT_EQ(event.type,  static_cast<uint32_t>(MsgType::PathEvent));
    EXPECT_EQ(event.state, static_cast<uint32_t>(PathState::PathUp));
    EXPECT_EQ(event.rssi,  -55);
    EXPECT_STREQ(event.classId, "multipath");
    EXPECT_STREQ(event.iface,   "wlan0");

    t.join();
    ::close(fd);
}

TEST_F(ConsumerApiTest, BroadcastNotDeliveredToOtherClass) {
    // Connect and register for lte_b2c.
    int fd = connectClient(kTestSocket);
    ASSERT_GE(fd, 0);

    ApiMsg req{};
    req.type = static_cast<uint32_t>(MsgType::Register);
    req.pid  = static_cast<int32_t>(::getpid());
    std::strncpy(req.classId, "lte_b2c", sizeof(req.classId) - 1);
    ASSERT_TRUE(sendRaw(fd, req));

    ApiMsg regAck{};
    ASSERT_TRUE(recvRaw(fd, regAck));
    ASSERT_EQ(regAck.error, 0u);

    // Broadcast for "multipath" — consumer registered only for "lte_b2c" — nothing received.
    server_->broadcastPathEvent("multipath", PathState::PathUp, "wlan0", -55);

    // Set a short receive timeout and expect timeout (no data).
    struct timeval tv{};
    tv.tv_sec  = 0;
    tv.tv_usec = 100'000; // 100 ms
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ApiMsg event{};
    const ssize_t n = ::recv(fd, &event, sizeof(event), MSG_WAITALL);
    EXPECT_LE(n, 0) << "should not have received a PathEvent for a different class";

    ::close(fd);
}

TEST_F(ConsumerApiTest, ApiMsgSize) {
    static_assert(sizeof(ApiMsg) == 96);
}

TEST_F(ConsumerApiTest, UpdateAndQueryCurrentState) {
    server_->updateCurrentState("lte_b2c", PathState::PathDegraded);
    EXPECT_EQ(server_->queryCurrentState("lte_b2c"), PathState::PathDegraded);
    EXPECT_EQ(server_->queryCurrentState("multipath"), PathState::Idle);
}
