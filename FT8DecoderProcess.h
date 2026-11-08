#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct FT8Slot {
    std::vector<float> audio;
    std::int64_t utcSeconds = 0;
    std::uint32_t dialHz = 0;
};

struct FT8DecodedSlot {
    std::int64_t utcSeconds = 0;
    std::uint32_t dialHz = 0;
    std::vector<std::string> lines;
};

class FT8DecoderProcess {
public:
    using ResultSink = std::function<void(FT8DecodedSlot&&)>;
    explicit FT8DecoderProcess(std::string executable = {},
        std::chrono::milliseconds timeout = std::chrono::seconds(45), ResultSink resultSink = {});
    ~FT8DecoderProcess();
    FT8DecoderProcess(const FT8DecoderProcess&) = delete;
    FT8DecoderProcess& operator=(const FT8DecoderProcess&) = delete;
    bool enqueue(FT8Slot&& slot);
    void stop();
    static std::string executablePath();
    // One process per complete slot; also used by the offline reception tests.
    static std::string decode(const FT8Slot& slot, const std::string& executable,
        std::chrono::milliseconds timeout, const std::atomic<bool>& cancel);
private:
    void worker();
    std::string executable_;
    std::chrono::milliseconds timeout_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<FT8Slot> queue_;
    ResultSink resultSink_;
    std::thread thread_;
};
