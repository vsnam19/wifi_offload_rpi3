// config_loader.cpp — JSON config loader implementation
// Scope: load + validate only.  No cgroup/iptables setup here (Phase 2).
// No hot-reload (Phase 1 loads once at startup).

#include "config/config_loader.hpp"
#include "common/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace netservice {

using json = nlohmann::json;

// ── Public entry point ────────────────────────────────────────────

std::expected<std::vector<PathClassConfig>, ConfigError>
ConfigLoader::loadFromFile(const std::filesystem::path& configPath) {
    // Open file
    std::ifstream file{configPath};
    if (!file.is_open()) {
        logger::error("[CONFIG] file not found: {}", configPath.string());
        return std::unexpected(ConfigError::FileNotFound);
    }

    // Parse JSON
    json root;
    try {
        root = json::parse(file);
    } catch (const json::parse_error& ex) {
        logger::error("[CONFIG] JSON parse error: {}", ex.what());
        return std::unexpected(ConfigError::ParseError);
    }

    // Require top-level "path_classes" array
    if (!root.contains("path_classes") || !root["path_classes"].is_array()) {
        logger::error("[CONFIG] missing or non-array 'path_classes' key");
        return std::unexpected(ConfigError::InvalidSchema);
    }

    const auto& arr = root["path_classes"];
    if (arr.empty()) {
        logger::error("[CONFIG] 'path_classes' array is empty");
        return std::unexpected(ConfigError::InvalidSchema);
    }

    // Parse each path class
    std::vector<PathClassConfig> classes;
    classes.reserve(arr.size());

    for (const auto& entry : arr) {
        auto result = parsePathClass(entry);
        if (!result) {
            return std::unexpected(result.error());
        }
        classes.push_back(std::move(result.value()));
    }

    // Cross-class invariant checks
    if (auto check = validateClasses(classes); !check) {
        return std::unexpected(check.error());
    }

    logger::info("[CONFIG] loaded {} path class(es): {}",
        classes.size(),
        [&]() {
            std::string ids;
            for (std::size_t i = 0; i < classes.size(); ++i) {
                if (i > 0) ids += ", ";
                ids += classes[i].id;
            }
            return ids;
        }());

    return classes;
}

// ── Private helpers ───────────────────────────────────────────────

std::expected<PathClassConfig, ConfigError>
ConfigLoader::parsePathClass(const json& j) {
    PathClassConfig cfg;

    // ── id (required, non-empty string) ───────────────────────────
    if (!j.contains("id") || !j["id"].is_string()) {
        logger::error("[CONFIG] path class missing required string 'id'");
        return std::unexpected(ConfigError::MissingField);
    }
    cfg.id = j["id"].get<std::string>();
    if (cfg.id.empty()) {
        logger::error("[CONFIG] path class 'id' must not be empty");
        return std::unexpected(ConfigError::InvalidValue);
    }

    // ── classid (required, hex string, non-zero) ──────────────────
    auto classidResult = parseHexField(j, "classid");
    if (!classidResult) return std::unexpected(classidResult.error());
    if (classidResult.value() == 0) {
        logger::error("[CONFIG] {}: classid must not be zero", cfg.id);
        return std::unexpected(ConfigError::InvalidValue);
    }
    cfg.classid = classidResult.value();

    // ── cgroup_path (required, non-empty string) ──────────────────
    if (!j.contains("cgroup_path") || !j["cgroup_path"].is_string()) {
        logger::error("[CONFIG] {}: missing required string 'cgroup_path'", cfg.id);
        return std::unexpected(ConfigError::MissingField);
    }
    cfg.cgroupPath = j["cgroup_path"].get<std::string>();
    if (cfg.cgroupPath.empty()) {
        logger::error("[CONFIG] {}: 'cgroup_path' must not be empty", cfg.id);
        return std::unexpected(ConfigError::InvalidValue);
    }

    // ── interfaces (required, non-empty array of non-empty strings)
    if (!j.contains("interfaces") || !j["interfaces"].is_array()) {
        logger::error("[CONFIG] {}: missing required array 'interfaces'", cfg.id);
        return std::unexpected(ConfigError::MissingField);
    }
    const auto& ifaces = j["interfaces"];
    if (ifaces.empty()) {
        logger::error("[CONFIG] {}: 'interfaces' must not be empty", cfg.id);
        return std::unexpected(ConfigError::InvalidValue);
    }
    for (const auto& iface : ifaces) {
        if (!iface.is_string() || iface.get<std::string>().empty()) {
            logger::error("[CONFIG] {}: each interface must be a non-empty string", cfg.id);
            return std::unexpected(ConfigError::InvalidValue);
        }
        cfg.interfaces.push_back(iface.get<std::string>());
    }

    // ── mptcp_enabled (required, bool) ────────────────────────────
    if (!j.contains("mptcp_enabled") || !j["mptcp_enabled"].is_boolean()) {
        logger::error("[CONFIG] {}: missing required bool 'mptcp_enabled'", cfg.id);
        return std::unexpected(ConfigError::MissingField);
    }
    cfg.mptcpEnabled = j["mptcp_enabled"].get<bool>();

    // ── routing_table (required, integer 1..65534) ────────────────
    if (!j.contains("routing_table") || !j["routing_table"].is_number_integer()) {
        logger::error("[CONFIG] {}: missing required integer 'routing_table'", cfg.id);
        return std::unexpected(ConfigError::MissingField);
    }
    const int rt = j["routing_table"].get<int>();
    if (rt < 1 || rt > 65534) {
        logger::error("[CONFIG] {}: routing_table {} out of range [1, 65534]", cfg.id, rt);
        return std::unexpected(ConfigError::InvalidValue);
    }
    cfg.routingTable = static_cast<uint32_t>(rt);

    // ── mark (required, hex string, non-zero) ─────────────────────
    auto markResult = parseHexField(j, "mark");
    if (!markResult) return std::unexpected(markResult.error());
    if (markResult.value() == 0) {
        logger::error("[CONFIG] {}: mark must not be zero", cfg.id);
        return std::unexpected(ConfigError::InvalidValue);
    }
    cfg.mark = markResult.value();

    // ── strict_isolation (optional, default false) ────────────────
    if (j.contains("strict_isolation")) {
        if (!j["strict_isolation"].is_boolean()) {
            logger::error("[CONFIG] {}: 'strict_isolation' must be a bool", cfg.id);
            return std::unexpected(ConfigError::InvalidValue);
        }
        cfg.strictIsolation = j["strict_isolation"].get<bool>();
    }

    // ── rssi (optional sub-object) ────────────────────────────────
    if (j.contains("rssi")) {
        if (!j["rssi"].is_object()) {
            logger::error("[CONFIG] {}: 'rssi' must be an object", cfg.id);
            return std::unexpected(ConfigError::InvalidValue);
        }
        cfg.rssi = parseRssiThresholds(j["rssi"]);

        // Sanity: warn must be higher than drop (less negative)
        if (cfg.rssi.warn <= cfg.rssi.drop) {
            logger::error("[CONFIG] {}: rssi.warn ({}) must be > rssi.drop ({})",
                cfg.id, cfg.rssi.warn, cfg.rssi.drop);
            return std::unexpected(ConfigError::InvalidValue);
        }
    }

    return cfg;
}

std::expected<uint32_t, ConfigError>
ConfigLoader::parseHexField(const json& j, std::string_view key) {
    if (!j.contains(key)) {
        logger::error("[CONFIG] missing required hex field '{}'", key);
        return std::unexpected(ConfigError::MissingField);
    }
    const auto& val = j[key];
    if (!val.is_string()) {
        logger::error("[CONFIG] field '{}' must be a hex string (e.g. \"0x10\")", key);
        return std::unexpected(ConfigError::InvalidValue);
    }
    const std::string s = val.get<std::string>();
    try {
        // std::stoul with base=0 handles "0x..." prefix automatically
        const unsigned long parsed = std::stoul(s, nullptr, 0);
        if (parsed > std::numeric_limits<uint32_t>::max()) {
            logger::error("[CONFIG] field '{}' value '{}' overflows uint32", key, s);
            return std::unexpected(ConfigError::InvalidValue);
        }
        return static_cast<uint32_t>(parsed);
    } catch (const std::invalid_argument&) {
        logger::error("[CONFIG] field '{}' value '{}' is not a valid hex string", key, s);
        return std::unexpected(ConfigError::InvalidValue);
    } catch (const std::out_of_range&) {
        logger::error("[CONFIG] field '{}' value '{}' overflows", key, s);
        return std::unexpected(ConfigError::InvalidValue);
    }
}

RssiThresholds
ConfigLoader::parseRssiThresholds(const json& j) {
    RssiThresholds rssi; // initialised with defaults from types.hpp

    if (j.contains("connect_min") && j["connect_min"].is_number_integer()) {
        rssi.connectMin = j["connect_min"].get<int>();
    }
    if (j.contains("warn") && j["warn"].is_number_integer()) {
        rssi.warn = j["warn"].get<int>();
    }
    if (j.contains("drop") && j["drop"].is_number_integer()) {
        rssi.drop = j["drop"].get<int>();
    }
    return rssi;
}

std::expected<void, ConfigError>
ConfigLoader::validateClasses(const std::vector<PathClassConfig>& classes) {
    std::set<std::string>  seenIds;
    std::set<uint32_t>     seenClassids;
    std::set<uint32_t>     seenMarks;
    std::set<uint32_t>     seenTables;

    for (const auto& cls : classes) {
        if (!seenIds.insert(cls.id).second) {
            logger::error("[CONFIG] duplicate path class id '{}'", cls.id);
            return std::unexpected(ConfigError::InvalidValue);
        }
        if (!seenClassids.insert(cls.classid).second) {
            logger::error("[CONFIG] duplicate classid 0x{:08x} in class '{}'",
                cls.classid, cls.id);
            return std::unexpected(ConfigError::InvalidValue);
        }
        if (!seenMarks.insert(cls.mark).second) {
            logger::error("[CONFIG] duplicate mark 0x{:x} in class '{}'",
                cls.mark, cls.id);
            return std::unexpected(ConfigError::InvalidValue);
        }
        if (!seenTables.insert(cls.routingTable).second) {
            logger::error("[CONFIG] duplicate routing_table {} in class '{}'",
                cls.routingTable, cls.id);
            return std::unexpected(ConfigError::InvalidValue);
        }
    }
    return {};
}

} // namespace netservice
