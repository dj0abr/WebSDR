#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace Callsign {
// Deliberately a simple identifier check, without license verification.
inline std::string normalize(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string::npos) return {};
    text = text.substr(first, text.find_last_not_of(" \t\r\n\f\v") - first + 1);
    if (text.size() < 3 || text.size() > 32) return {};
    bool hasLetter = false;
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (c >= 'A' && c <= 'Z') hasLetter = true;
        else if (!(c >= '0' && c <= '9') && c != '/') return {};
    }
    return hasLetter ? text : std::string{};
}

// Preserve the existing type-4 "callsign:callsign" wire format. Invalid
// packets mean logged out, including older browsers sending blank fields.
struct LoginResult {
    std::string callsign;
    std::string username; // Only the name before ':', never the password.
    std::string reason = "valid";
};

inline LoginResult inspectLoginMessage(const std::vector<float>& message) {
    LoginResult result;
    if (message.size() < 2 || message[0] != 4) {
        result.reason = "invalid-login-format";
        return result;
    }
    std::string text;
    for (std::size_t i = 1; i < message.size(); ++i) {
        const float value = message[i];
        if (value == 0) break;
        if (!std::isfinite(value) || value < 1 || value > 127 || std::trunc(value) != value) {
            result.reason = "invalid-login-byte";
            return result;
        }
        text += static_cast<char>(value);
    }
    const auto separator = text.find(':');
    if (separator == std::string::npos) {
        result.reason = "missing-separator";
        return result;
    }
    result.username = text.substr(0, separator);
    const auto username = normalize(result.username);
    if (username.empty()) {
        if (result.username.empty()) result.reason = "empty-callsign";
        else if (result.username.find_first_not_of(" \t\r\n\f\v") == std::string::npos)
            result.reason = "whitespace-only-callsign";
        else result.reason = "invalid-callsign";
    } else if (username != normalize(text.substr(separator + 1))) {
        result.reason = "callsign-mismatch";
    } else result.callsign = username;
    return result;
}

inline std::string fromLoginMessage(const std::vector<float>& message) {
    return inspectLoginMessage(message).callsign;
}
}
