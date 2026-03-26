#pragma once

// config_loader.hpp — load and validate path-policies.json
// Usage:
//   auto result = ConfigLoader::loadFromFile("/etc/netservice/path-policies.json");
//   if (!result) { /* handle ConfigError */ }
//   for (const auto& cls : result.value()) { ... }

#include "common/error.hpp"
#include "common/types.hpp"

#include <nlohmann/json.hpp>

#include <expected>
#include <filesystem>
#include <vector>

namespace netservice {

class ConfigLoader {
public:
    // Load and fully validate the config file.
    // Returns all path classes on success, or the first validation error.
    [[nodiscard]] static std::expected<std::vector<PathClassConfig>, ConfigError>
    loadFromFile(const std::filesystem::path& configPath);

private:
    // Parse a single path_class JSON object.
    [[nodiscard]] static std::expected<PathClassConfig, ConfigError>
    parsePathClass(const nlohmann::json& j);

    // Parse a hex-encoded string field (e.g. "0x00100001") to uint32_t.
    [[nodiscard]] static std::expected<uint32_t, ConfigError>
    parseHexField(const nlohmann::json& j, std::string_view key);

    // Parse an optional RSSI thresholds sub-object.
    [[nodiscard]] static RssiThresholds
    parseRssiThresholds(const nlohmann::json& j);

    // Cross-class invariant checks (duplicate ids, duplicate marks, etc.)
    [[nodiscard]] static std::expected<void, ConfigError>
    validateClasses(const std::vector<PathClassConfig>& classes);
};

} // namespace netservice
