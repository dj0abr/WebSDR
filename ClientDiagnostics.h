#pragma once

#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace ClientDiagnostics {
// Keep untrusted headers and callsign previews on one bounded journal line.
inline std::string quote(std::string_view text, std::size_t limit = 256) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    std::size_t i = 0;
    for (; i < text.size() && i < limit; ++i) {
        const unsigned char c = text[i];
        if (c == '\\' || c == '"') { result += '\\'; result += c; }
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else if (c < 32 || c >= 127) {
            result += "\\x"; result += hex[c >> 4]; result += hex[c & 15];
        } else result += c;
    }
    if (i < text.size()) result += "...";
    return result + '"';
}

inline void log(int clientId, const std::string& message) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    std::cout << "[Client #" << clientId << "] " << message << std::endl;
}

struct Activity {
    using Clock = std::chrono::steady_clock;
    Clock::time_point connectedAt = Clock::now();
    Clock::time_point lastMessageAt{};
    Clock::time_point nextAnonymousReport = connectedAt + std::chrono::seconds(15);
    Clock::time_point lastRejectedLoginLog{};
    std::uint64_t messages = 0, binary = 0, text = 0, malformed = 0, loginAttempts = 0;
    bool validLoginData = false;
    bool reportedMalformed = false, reportedText = false, reportedAnonymousCommand = false;
    std::string loginInfo = "no-login-received";
    std::string lastLoggedRejection;

    bool anonymousReportDue(Clock::time_point now) {
        if (validLoginData || now < nextAnonymousReport) return false;
        nextAnonymousReport = now + std::chrono::minutes(5);
        return true;
    }

    std::string summary(Clock::time_point now = Clock::now()) const {
        using namespace std::chrono;
        std::ostringstream out;
        out << "duration=" << duration_cast<seconds>(now - connectedAt).count() << "s"
            << " rx=" << messages << " binary=" << binary << " text=" << text
            << " malformed=" << malformed << " login_attempts=" << loginAttempts
            << " last_rx=";
        if (messages) out << duration_cast<seconds>(now - lastMessageAt).count() << "s-ago";
        else out << "none";
        out << " login_data=" << loginInfo;
        return out.str();
    }
};
}
