#pragma once
#include "SDRStreamInfo.h"
#include <cstddef>
#include <functional>
#include <memory>

// Shared 2.4 MHz -> 480 kHz input path. Small forward gaps are filled at the
// original rate so that all subsequent DSP stages retain their sample clocks.
// IQ is interleaved float32; output storage is valid during the callback only.
class SDRInputResampler {
public:
    using Sink = std::function<void(const void*, std::size_t, const SDRStreamInfo&)>;
    struct Stats {
        std::uint64_t filledSamples = 0, filledGaps = 0, rejectedGaps = 0;
    };
    explicit SDRInputResampler(Sink sink);
    ~SDRInputResampler();
    SDRInputResampler(const SDRInputResampler&) = delete;
    SDRInputResampler& operator=(const SDRInputResampler&) = delete;
    void process(const void* iq, std::size_t count, const SDRStreamInfo& info);
    Stats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
