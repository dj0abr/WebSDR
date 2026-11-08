#pragma once
#include "FT8DecoderProcess.h"
#include "SDRStreamInfo.h"
#include <functional>
#include <memory>

class FT8Receiver {
public:
    using SlotSink = std::function<void(FT8Slot&&)>;
    explicit FT8Receiver(SlotSink sink, std::int64_t timeOffsetNs = 0);
    ~FT8Receiver();
    FT8Receiver(const FT8Receiver&) = delete;
    FT8Receiver& operator=(const FT8Receiver&) = delete;
    // Binary interleaved float32 I,Q, at 480000 samples/s. A void pointer
    // avoids exposing Liquid's include-order-dependent complex type in this API.
    void process(const void* iq, std::size_t count, const SDRStreamInfo& info);
    static std::uint32_t dialFrequency(const SDRStreamInfo& info);
    static std::int64_t configuredTimeOffset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
