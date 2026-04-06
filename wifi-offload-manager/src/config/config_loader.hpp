#pragma once

// config_loader.hpp — load and validate path-policies.json
// Usage:
//   auto result = ConfigLoader::loadFromFile("/etc/netservice/path-policies.json");
//   if (!result) { /* handle ConfigError */ }
//   for (const auto& cls : result.value()) { ... }
//
// OPEN POINT OP-2: B2B in FOTA chain: should B2B be a last-resort fallback or
//   an explicitly selected profile?  The config currently treats all classes
//   equally.  Do NOT add FOTA-specific selection logic here until OP-2 is
//   resolved by the system integrator.
//
// OPEN POINT OP-4: JSON config signature verification.  The config file is
//   currently loaded and parsed without any integrity check.  Until OP-4 is
//   resolved (scheme to be decided: e.g. detached Ed25519 signature, HMAC),
//   do NOT add signature verification here.  Mark any future implementation
//   with a reference to OP-4.

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
