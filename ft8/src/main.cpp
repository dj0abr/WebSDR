// Standalone CLI, 2026. SPDX-License-Identifier: GPL-3.0-only
#include "ft8_bridge.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::size_t sample_count = 180000;
constexpr std::size_t min_samples = 156000;
constexpr std::size_t max_input_bytes = 16 * 1024 * 1024;
struct InputError : std::runtime_error { using std::runtime_error::runtime_error; };
struct Options {
    std::string input, format = "wav", output = "jsonl", utc;
    int low = 200, high = 4000, depth = 3;
};

void help() {
    std::cout <<
        "ft8-decoder 1.0.0 - standalone WSJT-X FT8 decoder\n"
        "Usage: ft8-decoder [options] FILE\n"
        "       ft8-decoder --format s16le - < slot.raw\n\n"
        "Input: mono audio, 12000 Hz, 13..15 s; one UTC-aligned slot per call.\n"
        "Use '-' for standard input (read until EOF). No IQ input or resampling.\n\n"
        "  --format wav|s16le|f32le   Input format (default: wav, PCM16)\n"
        "  --output jsonl|text       Output format (default: jsonl)\n"
        "  --min-freq HZ            Lowest tone-zero frequency (default: 200)\n"
        "  --max-freq HZ            Highest tone-zero frequency (default: 4000)\n"
        "  --depth 1|2|3            WSJT-X decoding depth (default: 3)\n"
        "  --utc HHMMSS             Optional UTC slot label (00/15/30/45 s)\n"
        "  --help                   Show help\n"
        "  --version                Show version and upstream commit\n\n"
        "JSONL: decode objects followed by one complete object, also for silence.\n"
        "Exit codes: 0=success (including no decodes), 2=invalid input/options,\n"
        "            3=internal/runtime error. Diagnostics go to stderr.\n";
}
int integer(const std::string& value, const std::string& option) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw InputError(option + " expects a nonnegative integer");
    try {
        std::size_t end;
        int result = std::stoi(value, &end);
        if (end != value.size()) throw InputError("Invalid integer");
        return result;
    } catch (const std::logic_error&) { throw InputError(option + " is out of range"); }
}
Options parse(int argc, char** argv) {
    Options o;
    bool positional = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (!positional && arg == "--") { positional = true; continue; }
        if (!positional && arg == "--help") { help(); std::exit(0); }
        if (!positional && arg == "--version") {
            std::cout << "ft8-decoder 1.0.0; WSJT-X 967c85a6119110788b0df3782cf796485b2cee14; GPL-3.0\n";
            std::exit(0);
        }
        if (!positional && arg.rfind("--", 0) == 0) {
            if (arg != "--format" && arg != "--output" && arg != "--utc" &&
                arg != "--min-freq" && arg != "--max-freq" && arg != "--depth")
                throw InputError("Unknown option: " + arg);
            if (++i == argc) throw InputError("Missing value for " + arg);
            const std::string value = argv[i];
            if (arg == "--format") o.format = value;
            else if (arg == "--output") o.output = value;
            else if (arg == "--utc") o.utc = value;
            else if (arg == "--min-freq") o.low = integer(value, arg);
            else if (arg == "--max-freq") o.high = integer(value, arg);
            else if (arg == "--depth") o.depth = integer(value, arg);
        } else {
            if (!positional && arg.size() > 1 && arg[0] == '-')
                throw InputError("Unknown option: " + arg);
            if (!o.input.empty()) throw InputError("Supply exactly one input file");
            o.input = arg;
        }
    }
    if (o.input.empty()) throw InputError("Missing input file; see --help");
    if (o.format != "wav" && o.format != "s16le" && o.format != "f32le")
        throw InputError("--format must be wav, s16le or f32le");
    if (o.output != "jsonl" && o.output != "text")
        throw InputError("--output must be jsonl or text");
    // Upstream baseline fit needs ten segments with several bins each, and
    // sync8 accesses bins above tone zero. Stay inside its supported range.
    if (o.low < 100 || o.high > 4900 || o.high - o.low < 200)
        throw InputError("Frequency range must be inside 100..4900 Hz and at least 200 Hz wide");
    if (o.depth < 1 || o.depth > 3) throw InputError("--depth must be 1, 2 or 3");
    if (!o.utc.empty()) {
        if (o.utc.size() != 6 || o.utc.find_first_not_of("0123456789") != std::string::npos ||
            integer(o.utc.substr(0, 2), "--utc") > 23 ||
            integer(o.utc.substr(2, 2), "--utc") > 59 ||
            integer(o.utc.substr(4, 2), "--utc") > 59 ||
            integer(o.utc.substr(4, 2), "--utc") % 15 != 0)
            throw InputError("--utc requires HHMMSS at a 15-second UTC slot boundary");
    }
    return o;
}
using Bytes = std::vector<unsigned char>;
Bytes read_bytes(std::istream& input) {
    Bytes data;
    std::array<char, 8192> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        const auto n = static_cast<std::size_t>(input.gcount());
        if (data.size() + n > max_input_bytes) throw InputError("Input exceeds 16 MiB; supply one slot");
        data.insert(data.end(), buffer.begin(), buffer.begin() + n);
    }
    if (!input.eof()) throw InputError("Could not read input");
    return data;
}
std::uint16_t u16(const Bytes& b, std::size_t p) {
    return std::uint16_t(b.at(p)) | (std::uint16_t(b.at(p + 1)) << 8);
}
std::uint32_t u32(const Bytes& b, std::size_t p) {
    return std::uint32_t(u16(b, p)) | (std::uint32_t(u16(b, p + 2)) << 16);
}
bool tag(const Bytes& b, std::size_t p, const char* s) {
    return p <= b.size() && b.size() - p >= 4 && std::memcmp(b.data() + p, s, 4) == 0;
}
struct Audio {
    std::array<std::int16_t, sample_count> samples{};
    std::size_t count = 0;
};
Audio audio(const Bytes& b, const std::string& format) {
    std::size_t start = 0, size = b.size();
    const std::size_t stride = format == "f32le" ? 4 : 2;
    if (format == "wav") {
        if (b.size() < 12 || !tag(b, 0, "RIFF") || !tag(b, 8, "WAVE"))
            throw InputError("Expected a RIFF/WAVE file (PCM16, mono, 12000 Hz)");
        const std::uint64_t riff_end = std::uint64_t(u32(b, 4)) + 8;
        // WSJT-X recordings may omit the last metadata chunk's padding byte
        // from the RIFF length while still writing that byte to the file.
        const bool final_pad = (riff_end % 2 == 1 && riff_end + 1 == b.size() && b.back() == 0);
        if (riff_end != b.size() && !final_pad)
            throw InputError("WAV RIFF length does not match the input size");
        bool have_fmt = false, have_data = false;
        std::size_t p = 12;
        while (p < b.size()) {
            if (b.size() - p < 8) throw InputError("Truncated WAV chunk header");
            const auto length = std::size_t(u32(b, p + 4));
            const auto payload = p + 8;
            if (length > b.size() - payload) throw InputError("Truncated WAV chunk");
            if (tag(b, p, "fmt ")) {
                if (have_fmt || length < 16) throw InputError("Invalid or duplicate WAV fmt chunk");
                have_fmt = true;
                if (u16(b, payload) != 1 || u16(b, payload + 2) != 1 ||
                    u32(b, payload + 4) != 12000 || u16(b, payload + 14) != 16 ||
                    u16(b, payload + 12) != 2 || u32(b, payload + 8) != 24000)
                    throw InputError("WAV must contain PCM16 mono audio at 12000 Hz");
            } else if (tag(b, p, "data")) {
                if (have_data) throw InputError("Multiple WAV data chunks are not supported");
                have_data = true;
                start = payload;
                size = length;
            }
            p = payload + length;
            if (length % 2) {
                if (p == b.size()) throw InputError("Missing WAV chunk padding");
                ++p;
            }
        }
        if (!have_fmt || !have_data) throw InputError("WAV lacks fmt or data chunk");
    }
    if (size % stride) throw InputError("Input ends in a partial audio sample");
    const auto n = size / stride;
    if (n < min_samples || n > sample_count)
        throw InputError("Expected 156000..180000 mono samples (13..15 seconds at 12000 Hz)");
    Audio result;
    result.count = n;
    for (std::size_t i = 0; i < n; ++i) {
        const auto p = start + stride * i;
        if (format == "f32le") {
            static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                          "IEEE 754 binary32 required");
            const auto bits = u32(b, p);
            float value;
            std::memcpy(&value, &bits, 4);
            if (!std::isfinite(value) || value < -1 || value > 1)
                throw InputError("f32le samples must be finite and normalized to [-1,1]");
            result.samples[i] = static_cast<std::int16_t>(std::clamp(std::lround(value * 32768.0), -32768L, 32767L));
        } else {
            const int value = u16(b, p);
            result.samples[i] = static_cast<std::int16_t>(value >= 32768 ? value - 65536 : value);
        }
    }
    return result;
}
std::string quoted(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c);
        else out << c;
    }
    out << '"';
    return out.str();
}
struct Result {
    std::string message;
    int snr;
    float dt, frequency, sync, quality;
    int ap;
};
struct Results {
    std::vector<Result> values;
    bool failed = false;
};
// Do not propagate C++ exceptions through Fortran stack frames.
void receive(const char* message, int length, int snr, float dt, float freq,
             float sync, float quality, int ap, void* context) noexcept {
    auto& results = *static_cast<Results*>(context);
    try {
        if (!std::isfinite(dt) || !std::isfinite(freq) || !std::isfinite(sync) || !std::isfinite(quality))
            throw std::runtime_error("Non-finite decoder result");
        results.values.push_back({std::string(message, length), snr, dt, freq, sync, quality, ap});
    } catch (...) { results.failed = true; }
}
void print(const Options& o, const Audio& a, const Results& results) {
    std::cout << std::fixed << std::setprecision(3);
    for (const auto& r : results.values) {
        if (o.output == "jsonl") {
            std::cout << "{\"type\":\"decode\",\"utc\":" << (o.utc.empty() ? "null" : quoted(o.utc))
                << ",\"snr_db\":" << r.snr << ",\"dt_s\":" << r.dt
                << ",\"frequency_hz\":" << r.frequency << ",\"sync\":" << r.sync
                << ",\"quality\":" << r.quality << ",\"ap\":" << r.ap
                << ",\"message\":" << quoted(r.message) << "}\n";
        } else {
            std::cout << (o.utc.empty() ? "------" : o.utc) << ' ' << std::setw(3) << r.snr
                << ' ' << std::setw(7) << r.dt << ' ' << std::setw(8) << r.frequency
                << " ~ " << r.message << '\n';
        }
    }
    if (o.output == "jsonl")
        std::cout << "{\"type\":\"complete\",\"decodes\":" << results.values.size()
            << ",\"samples\":" << a.count << ",\"sample_rate\":12000}\n";
}
} // namespace
int main(int argc, char** argv) {
    std::locale::global(std::locale::classic());
    try {
        const auto options = parse(argc, argv);
        Bytes bytes;
        if (options.input == "-") bytes = read_bytes(std::cin);
        else {
            std::ifstream input(options.input, std::ios::binary);
            if (!input) throw InputError("Cannot open input: " + options.input);
            bytes = read_bytes(input);
        }
        const auto samples = audio(bytes, options.format);
        Results results;
        // Silence has no finite spectral baseline in upstream. Skip it safely.
        if (std::any_of(samples.samples.begin(), samples.samples.end(), [](auto x) { return x != 0; }))
            ft8_decode_slot(samples.samples.data(), options.low, options.high, options.depth, receive, &results);
        if (results.failed) throw std::runtime_error("Could not collect decoder results");
        print(options, samples, results);
        std::cout.flush();
        if (!std::cout) throw std::runtime_error("Could not write results");
        return 0;
    } catch (const InputError& e) {
        std::cerr << "ft8-decoder: " << e.what() << '\n';
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "ft8-decoder: " << e.what() << '\n';
        return 3;
    }
}
