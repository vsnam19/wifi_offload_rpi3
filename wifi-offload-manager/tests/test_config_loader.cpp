// test_config_loader.cpp — unit tests for ConfigLoader
// Covers: valid config, missing required fields, invalid values,
//         cross-class duplicates, optional fields with defaults.

#include "config/config_loader.hpp"
#include "common/error.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

namespace netservice {
namespace {

// ── Helpers ───────────────────────────────────────────────────────

// Write JSON to a temp file and call loadFromFile; cleans up on destruction.
class TempConfig {
public:
    explicit TempConfig(std::string_view content) {
        path_ = std::filesystem::temp_directory_path()
                / ("test_config_" + std::to_string(
                    std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".json");
        std::ofstream f{path_};
        f << content;
    }
    ~TempConfig() { std::filesystem::remove(path_); }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

// Minimal valid 3-class config matching path-policies.json
constexpr std::string_view kValidConfig = R"({
  "path_classes": [
    {
      "id": "multipath",
      "classid": "0x00100001",
      "cgroup_path": "/sys/fs/cgroup/net_cls/multipath",
      "interfaces": ["wlan0", "wwan0"],
      "mptcp_enabled": true,
      "routing_table": 100,
      "mark": "0x10",
      "strict_isolation": false,
      "rssi": { "connect_min": -70, "warn": -75, "drop": -85 }
    },
    {
      "id": "lte_b2c",
      "classid": "0x00100002",
      "cgroup_path": "/sys/fs/cgroup/net_cls/lte_b2c",
      "interfaces": ["wwan0"],
      "mptcp_enabled": false,
      "routing_table": 200,
      "mark": "0x20",
      "strict_isolation": false
    },
    {
      "id": "lte_b2b",
      "classid": "0x00100003",
      "cgroup_path": "/sys/fs/cgroup/net_cls/lte_b2b",
      "interfaces": ["wwan1"],
      "mptcp_enabled": false,
      "routing_table": 300,
      "mark": "0x30",
      "strict_isolation": true
    }
  ]
})";

// ── Test: valid config ────────────────────────────────────────────

TEST(ConfigLoader, LoadsValidConfig) {
    TempConfig tmp{kValidConfig};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_TRUE(result.has_value());

    const auto& classes = result.value();
    ASSERT_EQ(classes.size(), 3u);

    // multipath
    EXPECT_EQ(classes[0].id, "multipath");
    EXPECT_EQ(classes[0].classid, 0x00100001u);
    EXPECT_EQ(classes[0].cgroupPath, "/sys/fs/cgroup/net_cls/multipath");
    EXPECT_EQ(classes[0].interfaces, (std::vector<std::string>{"wlan0", "wwan0"}));
    EXPECT_TRUE(classes[0].mptcpEnabled);
    EXPECT_EQ(classes[0].routingTable, 100u);
    EXPECT_EQ(classes[0].mark, 0x10u);
    EXPECT_FALSE(classes[0].strictIsolation);
    EXPECT_EQ(classes[0].rssi.connectMin, -70);
    EXPECT_EQ(classes[0].rssi.warn, -75);
    EXPECT_EQ(classes[0].rssi.drop, -85);

    // lte_b2b (strict)
    EXPECT_EQ(classes[2].id, "lte_b2b");
    EXPECT_TRUE(classes[2].strictIsolation);
    EXPECT_FALSE(classes[2].mptcpEnabled);
}

TEST(ConfigLoader, DefaultRssiThresholdsWhenAbsent) {
    // lte_b2c has no rssi block — defaults must be used
    TempConfig tmp{kValidConfig};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_TRUE(result.has_value());
    const auto& b2c = result.value()[1];
    EXPECT_EQ(b2c.rssi.connectMin, -70);
    EXPECT_EQ(b2c.rssi.warn, -75);
    EXPECT_EQ(b2c.rssi.drop, -85);
}

// ── Test: file errors ─────────────────────────────────────────────

TEST(ConfigLoader, FileNotFound) {
    auto result = ConfigLoader::loadFromFile("/nonexistent/path-policies.json");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::FileNotFound);
}

TEST(ConfigLoader, InvalidJson) {
    TempConfig tmp{"this is not json {"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::ParseError);
}

// ── Test: schema errors ───────────────────────────────────────────

TEST(ConfigLoader, MissingPathClassesKey) {
    TempConfig tmp{R"({"version": 1})"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::InvalidSchema);
}

TEST(ConfigLoader, EmptyPathClassesArray) {
    TempConfig tmp{R"({"path_classes": []})"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::InvalidSchema);
}

// ── Test: missing required fields ────────────────────────────────

TEST(ConfigLoader, MissingId) {
    TempConfig tmp{R"({
      "path_classes": [{
        "classid": "0x00100001",
        "cgroup_path": "/sys/fs/cgroup/net_cls/multipath",
        "interfaces": ["wlan0"],
        "mptcp_enabled": false,
        "routing_table": 100,
        "mark": "0x10"
      }]
    })"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::MissingField);
}

TEST(ConfigLoader, MissingClassid) {
    TempConfig tmp{R"({
      "path_classes": [{
        "id": "x",
        "cgroup_path": "/sys/fs/cgroup/net_cls/x",
        "interfaces": ["wlan0"],
        "mptcp_enabled": false,
        "routing_table": 100,
        "mark": "0x10"
      }]
    })"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::MissingField);
}

TEST(ConfigLoader, MissingMark) {
    TempConfig tmp{R"({
      "path_classes": [{
        "id": "x",
        "classid": "0x00100001",
        "cgroup_path": "/sys/fs/cgroup/net_cls/x",
        "interfaces": ["wlan0"],
        "mptcp_enabled": false,
        "routing_table": 100
      }]
    })"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::MissingField);
}

// ── Test: invalid values ──────────────────────────────────────────

TEST(ConfigLoader, ZeroClassid) {
    TempConfig tmp{R"({
      "path_classes": [{
        "id": "x", "classid": "0x0",
        "cgroup_path": "/a", "interfaces": ["wlan0"],
        "mptcp_enabled": false, "routing_table": 100, "mark": "0x10"
      }]
    })"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::InvalidValue);
}

TEST(ConfigLoader, ZeroMark) {
    TempConfig tmp{R"({
      "path_classes": [{
        "id": "x", "classid": "0x00100001",
        "cgroup_path": "/a", "interfaces": ["wlan0"],
        "mptcp_enabled": false, "routing_table": 100, "mark": "0x0"
      }]
    })"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::InvalidValue);
}

TEST(ConfigLoader, RoutingTableOutOfRange) {
    TempConfig tmp{R"({
      "path_classes": [{
        "id": "x", "classid": "0x00100001",
        "cgroup_path": "/a", "interfaces": ["wlan0"],
        "mptcp_enabled": false, "routing_table": 0, "mark": "0x10"
      }]
    })"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::InvalidValue);
}

TEST(ConfigLoader, NonHexMarkString) {
    TempConfig tmp{R"({
      "path_classes": [{
        "id": "x", "classid": "0x00100001",
        "cgroup_path": "/a", "interfaces": ["wlan0"],
        "mptcp_enabled": false, "routing_table": 100, "mark": "not_hex"
      }]
    })"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::InvalidValue);
}

TEST(ConfigLoader, RssiWarnNotHigherThanDrop) {
    TempConfig tmp{R"({
      "path_classes": [{
        "id": "x", "classid": "0x00100001",
        "cgroup_path": "/a", "interfaces": ["wlan0"],
        "mptcp_enabled": false, "routing_table": 100, "mark": "0x10",
        "rssi": { "warn": -90, "drop": -85 }
      }]
    })"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::InvalidValue);
}

// ── Test: cross-class duplicate checks ───────────────────────────

TEST(ConfigLoader, DuplicateId) {
    TempConfig tmp{R"({
      "path_classes": [
        {"id":"x","classid":"0x00100001","cgroup_path":"/a","interfaces":["wlan0"],
         "mptcp_enabled":false,"routing_table":100,"mark":"0x10"},
        {"id":"x","classid":"0x00100002","cgroup_path":"/b","interfaces":["wwan0"],
         "mptcp_enabled":false,"routing_table":200,"mark":"0x20"}
      ]
    })"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::InvalidValue);
}

TEST(ConfigLoader, DuplicateMark) {
    TempConfig tmp{R"({
      "path_classes": [
        {"id":"a","classid":"0x00100001","cgroup_path":"/a","interfaces":["wlan0"],
         "mptcp_enabled":false,"routing_table":100,"mark":"0x10"},
        {"id":"b","classid":"0x00100002","cgroup_path":"/b","interfaces":["wwan0"],
         "mptcp_enabled":false,"routing_table":200,"mark":"0x10"}
      ]
    })"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::InvalidValue);
}

TEST(ConfigLoader, DuplicateRoutingTable) {
    TempConfig tmp{R"({
      "path_classes": [
        {"id":"a","classid":"0x00100001","cgroup_path":"/a","interfaces":["wlan0"],
         "mptcp_enabled":false,"routing_table":100,"mark":"0x10"},
        {"id":"b","classid":"0x00100002","cgroup_path":"/b","interfaces":["wwan0"],
         "mptcp_enabled":false,"routing_table":100,"mark":"0x20"}
      ]
    })"};
    auto result = ConfigLoader::loadFromFile(tmp.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ConfigError::InvalidValue);
}

} // namespace
} // namespace netservice
