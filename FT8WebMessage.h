#pragma once
#include "FT8DecoderProcess.h"
#include <array>
#include <stdexcept>
#include <string_view>

namespace FT8Web {
using Frame = std::array<float, 1025>;
constexpr std::size_t payloadSize = 1023; // Leave one float for the zero terminator.

// Type 7 shares the existing 4100-byte binary protocol. Each payload byte is
// represented by one float, as for the user list and ADS-B. Records never span
// frames, so a browser can parse every frame independently.
inline Frame frame(std::string_view text) {
    if (text.size() > payloadSize) throw std::runtime_error("FT8 web frame too large");
    Frame result{};
    result[0] = 7;
    for (std::size_t i = 0; i < text.size(); ++i)
        result[i + 1] = static_cast<unsigned char>(text[i]);
    return result;
}

// S <dial-Hz>, or D <UTC Unix seconds> <dial-Hz> <original decoder line>.
// Fields are tab-separated and each record ends with a newline.
inline Frame status(std::uint32_t dialHz) { return frame("S\t" + std::to_string(dialHz) + "\n"); }

inline std::vector<Frame> encode(const FT8DecodedSlot& result) {
    const auto prefix = "D\t" + std::to_string(result.utcSeconds) + "\t" +
        std::to_string(result.dialHz) + "\t";
    std::vector<Frame> frames;
    std::string payload;
    for (const auto& line : result.lines) {
        if (line.empty()) continue;
        if (line.find_first_of("\r\n\t") != std::string::npos)
            throw std::runtime_error("invalid FT8 web record");
        const auto record = prefix + line + "\n";
        if (record.size() > payloadSize) throw std::runtime_error("FT8 web record too large");
        if (payload.size() + record.size() > payloadSize) {
            frames.push_back(frame(payload));
            payload.clear();
        }
        payload += record;
    }
    if (!payload.empty()) frames.push_back(frame(payload)); // Flush the final partial frame immediately.
    return frames;
}
}
