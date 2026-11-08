#pragma once
#include <cmath>
#include <cstdint>

enum class SDRStreamChange : std::uint8_t {
    None, Startup, HardwareReset, FrequencyChange, SampleRateChange,
    BandChange, HardwareSampleGap, QueueSampleGap, IQSampleGap,
    ClockStep, TimestampMismatch, StreamRestart
};

inline const char* sdrStreamChangeName(SDRStreamChange reason) {
    switch (reason) {
    case SDRStreamChange::Startup: return "startup";
    case SDRStreamChange::HardwareReset: return "SDR reset";
    case SDRStreamChange::FrequencyChange: return "RF frequency change";
    case SDRStreamChange::SampleRateChange: return "sample rate change";
    case SDRStreamChange::BandChange: return "band change";
    case SDRStreamChange::HardwareSampleGap: return "SDR sample gap";
    case SDRStreamChange::QueueSampleGap: return "SDR queue gap";
    case SDRStreamChange::IQSampleGap: return "IQ sample gap";
    case SDRStreamChange::ClockStep: return "UTC clock step";
    case SDRStreamChange::TimestampMismatch: return "timestamp discontinuity";
    case SDRStreamChange::StreamRestart: return "stream restart";
    case SDRStreamChange::None: return "none";
    }
    return "unknown";
}

// Time refers to the first sample, before downstream filter delays.
struct SDRStreamInfo {
    std::uint64_t firstSample = 0;
    std::int64_t utcNanoseconds = 0;
    std::uint64_t generation = 0;
    std::uint32_t startHz = 0, endHz = 0, centerHz = 0;
    bool discontinuity = false;
    SDRStreamChange changeReason = SDRStreamChange::None;
    std::int64_t sampleDelta = 0, clockDeltaNs = 0;
};

inline std::int64_t sdrSampleTime(std::uint64_t samples, std::uint32_t rate) {
    return static_cast<std::int64_t>(samples / rate) * 1000000000LL +
        static_cast<std::int64_t>((samples % rate) * 1000000000ULL / rate);
}

inline std::uint64_t sdrSegment(std::uint32_t start, std::uint32_t end) {
    return (std::uint64_t(start) << 32) | end;
}

// Called only by the hardware callback. Sample counters determine capture time;
// callback arrival times can be delayed or delivered in bursts. A UTC step is
// detected against the monotonic clock, independently of sample delivery lag.
class SDRCaptureClock {
public:
    SDRStreamInfo observe(std::uint32_t first, std::uint32_t count,
                          std::int64_t callbackUtc, std::int64_t callbackMonotonic,
                          std::uint64_t segment,
                          SDRStreamChange event = SDRStreamChange::None,
                          std::int64_t clockReadSpanNs = 0) {
        // Ignore a clock pair if this thread was descheduled between the reads.
        const bool validClocks = clockReadSpanNs >= 0 && clockReadSpanNs <= 5000000;
        const auto clockOffset = callbackUtc - callbackMonotonic;
        const auto clockDelta = haveClockOffset_ ? clockOffset - clockOffset_ : 0;
        const bool clockStep = validClocks && haveClockOffset_ &&
                               std::abs(clockDelta) > 250000000LL;
        const std::uint32_t difference = first - expected_;
        const auto sampleDelta = difference < 0x80000000U ? std::int64_t(difference) :
            std::int64_t(difference) - 0x100000000LL;
        auto reason = SDRStreamChange::None;
        if (!started_) reason = SDRStreamChange::Startup;
        else if (event != SDRStreamChange::None) reason = event;
        else if (segment != segment_) reason = SDRStreamChange::BandChange;
        else if (sampleDelta < 0) reason = SDRStreamChange::HardwareSampleGap;
        else if (clockStep) reason = SDRStreamChange::ClockStep;
        // A forward counter jump tells us exactly how much capture time is
        // missing. Keep that time, including across delayed callbacks. The
        // downstream input resampler decides whether the gap can be filled.
        const bool forwardGap = started_ && sampleDelta > 0 &&
            (reason == SDRStreamChange::None || reason == SDRStreamChange::ClockStep);
        if (forwardGap) position_ += sampleDelta;
        const bool broken = reason != SDRStreamChange::None;
        if (broken) {
            // For a UTC step, preserve the sample timeline and driver backlog.
            // Re-anchoring to callback arrival here would add scheduling latency.
            anchor_ = reason == SDRStreamChange::ClockStep ?
                anchor_ + sdrSampleTime(position_, rate) + clockDelta :
                callbackUtc - sdrSampleTime(count, rate);
            epochSampleDelta_ = reason == SDRStreamChange::HardwareSampleGap ?
                sampleDelta : 0;
            epochClockDelta_ = reason == SDRStreamChange::ClockStep ? clockDelta : 0;
            epochReason_ = reason;
            ++generation_;
            position_ = 0;
        }
        if (validClocks) { clockOffset_ = clockOffset; haveClockOffset_ = true; }
        SDRStreamInfo info;
        info.firstSample = position_;
        info.utcNanoseconds = anchor_ + sdrSampleTime(position_, rate);
        info.generation = generation_;
        info.startHz = segment >> 32;
        info.endHz = static_cast<std::uint32_t>(segment);
        info.centerHz = info.startHz == 1089900000 ? 1090000000 :
            (segment ? info.startHz + 240000 : 0);
        info.discontinuity = broken;
        // Keep epoch diagnostics on later blocks, too: a full queue may drop
        // the first block containing the discontinuity marker.
        info.changeReason = forwardGap && !broken ?
            SDRStreamChange::HardwareSampleGap : epochReason_;
        info.sampleDelta = forwardGap && !broken ? sampleDelta : epochSampleDelta_;
        info.clockDeltaNs = epochClockDelta_;
        position_ += count;
        expected_ = first + count;
        segment_ = segment;
        started_ = true;
        return info;
    }
private:
    static constexpr std::uint32_t rate = 2400000;
    bool started_ = false, haveClockOffset_ = false;
    std::uint32_t expected_ = 0;
    std::uint64_t position_ = 0, generation_ = 0, segment_ = 0;
    std::int64_t anchor_ = 0;
    std::int64_t clockOffset_ = 0, epochSampleDelta_ = 0, epochClockDelta_ = 0;
    SDRStreamChange epochReason_ = SDRStreamChange::None;
};
