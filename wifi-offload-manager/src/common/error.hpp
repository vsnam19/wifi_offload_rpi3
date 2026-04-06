#pragma once

// error.hpp — std::expected error types used across the daemon
// No exceptions in daemon code — every fallible operation returns
// std::expected<T, SomeError>.

#include <expected>
#include <string_view>

namespace netservice {

// ── Config errors ─────────────────────────────────────────────────
enum class ConfigError {
    FileNotFound,
    ParseError,
    InvalidSchema,
    MissingField,
    InvalidValue,
};

[[nodiscard]] constexpr std::string_view toString(ConfigError e) noexcept {
    switch (e) {
        case ConfigError::FileNotFound:  return "FileNotFound";
        case ConfigError::ParseError:    return "ParseError";
        case ConfigError::InvalidSchema: return "InvalidSchema";
        case ConfigError::MissingField:  return "MissingField";
        case ConfigError::InvalidValue:  return "InvalidValue";
    }
    return "Unknown";
}

// ── Routing errors ────────────────────────────────────────────────
enum class RoutingError {
    CgroupCreateFailed,
    CgroupWriteFailed,
    IptablesError,
    NetlinkError,
    AlreadyExists,
    NotFound,
};

[[nodiscard]] constexpr std::string_view toString(RoutingError e) noexcept {
    switch (e) {
        case RoutingError::CgroupCreateFailed: return "CgroupCreateFailed";
        case RoutingError::CgroupWriteFailed:  return "CgroupWriteFailed";
        case RoutingError::IptablesError:      return "IptablesError";
        case RoutingError::NetlinkError:       return "NetlinkError";
        case RoutingError::AlreadyExists:      return "AlreadyExists";
        case RoutingError::NotFound:           return "NotFound";
    }
    return "Unknown";
}

// ── MPTCP errors ──────────────────────────────────────────────────
enum class MptcpError {
    NetlinkError,
    EndpointNotFound,
    UnsupportedKernel,
};

[[nodiscard]] constexpr std::string_view toString(MptcpError e) noexcept {
    switch (e) {
        case MptcpError::NetlinkError:      return "NetlinkError";
        case MptcpError::EndpointNotFound:  return "EndpointNotFound";
        case MptcpError::UnsupportedKernel: return "UnsupportedKernel";
    }
    return "Unknown";
}

// ── WPA monitor errors ────────────────────────────────────────────
enum class WpaError {
    ConnectFailed,    // failed to open wpa_ctrl socket
    AttachFailed,     // wpa_ctrl_attach() failed
    RecvError,        // wpa_ctrl_recv() returned error
    Timeout,          // no events within expected window
};

[[nodiscard]] constexpr std::string_view toString(WpaError e) noexcept {
    switch (e) {
        case WpaError::ConnectFailed: return "ConnectFailed";
        case WpaError::AttachFailed:  return "AttachFailed";
        case WpaError::RecvError:     return "RecvError";
        case WpaError::Timeout:       return "Timeout";
    }
    return "Unknown";
}

// ── API errors ────────────────────────────────────────────────────
enum class ApiError {
    SocketError,
    InvalidMessage,
    UnknownClassId,
    RegistrationFailed,
};

[[nodiscard]] constexpr std::string_view toString(ApiError e) noexcept {
    switch (e) {
        case ApiError::SocketError:         return "SocketError";
        case ApiError::InvalidMessage:      return "InvalidMessage";
        case ApiError::UnknownClassId:      return "UnknownClassId";
        case ApiError::RegistrationFailed:  return "RegistrationFailed";
    }
    return "Unknown";
}

} // namespace netservice
