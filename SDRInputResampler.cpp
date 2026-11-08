#include <complex>
#include "liquid.h"
#include "SDRInputResampler.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
constexpr std::uint32_t inputRate = 2400000, outputRate = 480000;
constexpr std::uint64_t maxGap = inputRate / 200; // 5 ms per gap
constexpr std::uint64_t maxSlotFill = inputRate / 50; // 20 ms per UTC slot
constexpr std::int64_t slotNs = 15000000000LL;
constexpr std::size_t blockSize = 2048;
}

struct SDRInputResampler::Impl {
    Sink sink;
    Stats stats;
    msresamp_crcf resampler = nullptr;
    std::array<liquid_float_complex, blockSize> input{}, output{}, zeros{};
    SDRStreamInfo current, resetInfo;
    std::uint64_t expected = 0, rawStart = 0, produced = 0;
    std::int64_t rawAnchor = 0, outputAnchor = 0, delayNs = 0;
    std::int64_t lastFillLog = 0;
    bool started = false, pendingReset = false;
    struct Budget { std::int64_t slot = -1; std::uint64_t samples = 0; };
    std::array<Budget, 2> budgets{};

    explicit Impl(Sink callback) : sink(std::move(callback)) {
        resampler = msresamp_crcf_create(float(outputRate)/inputRate, 60);
        if (!resampler) throw std::runtime_error("cannot create SDR input resampler");
        delayNs = std::llround(1e9 * msresamp_crcf_get_delay(resampler) / inputRate);
    }
    ~Impl() { msresamp_crcf_destroy(resampler); }

    bool reserveGap(std::uint64_t gap) {
        if (gap > maxGap) return false;
        const auto firstUtc = rawAnchor + sdrSampleTime(expected - rawStart, inputRate);
        if (firstUtc < 0) return false;
        const auto slot = firstUtc / slotNs;
        // Count samples before the next UTC boundary, including a fractional
        // sample period. A gap spanning that boundary uses both slot budgets.
        const auto beforeBoundary = static_cast<std::uint64_t>(
            ((slotNs - firstUtc % slotNs)*inputRate + 999999999LL) / 1000000000LL);
        const auto firstPart = std::min(gap, beforeBoundary);
        const std::uint64_t parts[]{firstPart, gap - firstPart};
        auto proposed = budgets;
        for (int i = 0; i < 2; ++i) {
            if (!parts[i]) continue;
            auto found = std::find_if(proposed.begin(), proposed.end(),
                [&](const Budget& b) { return b.slot == slot + i; });
            if (found == proposed.end()) {
                found = std::min_element(proposed.begin(), proposed.end(),
                    [](const Budget& a, const Budget& b) { return a.slot < b.slot; });
                *found = Budget{slot + i, 0};
            }
            if (found->samples + parts[i] > maxSlotFill) return false;
            found->samples += parts[i];
        }
        budgets = proposed;
        return true;
    }

    void emit(liquid_float_complex* data, std::size_t count) {
        unsigned int outputs = 0;
        msresamp_crcf_execute(resampler, data, count, output.data(), &outputs);
        if (!outputs) return;
        auto info = current;
        info.firstSample = produced;
        info.utcNanoseconds = outputAnchor + sdrSampleTime(produced, outputRate);
        info.discontinuity = pendingReset;
        if (pendingReset) {
            info.changeReason = resetInfo.changeReason;
            info.sampleDelta = resetInfo.sampleDelta;
            info.clockDeltaNs = resetInfo.clockDeltaNs;
        } else {
            info.changeReason = SDRStreamChange::None;
            info.sampleDelta = info.clockDeltaNs = 0;
        }
        produced += outputs;
        pendingReset = false;
        sink(output.data(), outputs, info);
    }
};

SDRInputResampler::SDRInputResampler(Sink sink) : impl_(new Impl(std::move(sink))) {}
SDRInputResampler::~SDRInputResampler() = default;
SDRInputResampler::Stats SDRInputResampler::stats() const { return impl_->stats; }

void SDRInputResampler::process(const void* iq, std::size_t count, const SDRStreamInfo& capture) {
    if (!count) return;
    if (!iq || count > blockSize) throw std::runtime_error("invalid SDR input block");
    static_assert(sizeof(liquid_float_complex) == 2*sizeof(float), "interleaved float IQ required");
    auto& d = *impl_;
    auto info = capture;
    const bool newEpoch = !d.started || info.generation != d.current.generation ||
        info.startHz != d.current.startHz || info.endHz != d.current.endHz ||
        info.centerHz != d.current.centerHz;
    bool broken = newEpoch || info.discontinuity;
    const auto delta = static_cast<std::int64_t>(info.firstSample) -
                       static_cast<std::int64_t>(d.expected);
    if (!broken && info.firstSample >= d.rawStart) {
        const auto predicted = d.rawAnchor + sdrSampleTime(info.firstSample - d.rawStart, inputRate);
        if (std::abs(info.utcNanoseconds - predicted) > 2000000LL) {
            broken = true;
            info.changeReason = SDRStreamChange::TimestampMismatch;
            info.clockDeltaNs = info.utcNanoseconds - predicted;
        }
    }
    if (!broken && delta) {
        const auto reason = info.changeReason == SDRStreamChange::HardwareSampleGap && info.sampleDelta > 0 ?
            SDRStreamChange::HardwareSampleGap : SDRStreamChange::QueueSampleGap;
        if (delta > 0 && d.reserveGap(delta)) {
            d.stats.filledSamples += delta;
            ++d.stats.filledGaps;
            // At most one repair message per five seconds.
            if (!d.lastFillLog || info.utcNanoseconds - d.lastFillLog >= 5000000000LL) {
                std::fprintf(stderr, "[SDR] filling %s: +%lld samples (%.3f ms) with zeros; UTC timeline preserved\n",
                    sdrStreamChangeName(reason), static_cast<long long>(delta), 1000.0*delta/inputRate);
                d.lastFillLog = info.utcNanoseconds;
            }
            for (std::uint64_t remaining = delta; remaining;) {
                const auto part = std::min<std::uint64_t>(remaining, blockSize);
                d.emit(d.zeros.data(), part);
                remaining -= part;
            }
        } else {
            broken = true;
            ++d.stats.rejectedGaps;
            info.changeReason = reason;
            info.sampleDelta = delta;
            info.clockDeltaNs = 0;
        }
    }
    if (broken) {
        msresamp_crcf_reset(d.resampler);
        d.produced = 0;
        d.rawStart = info.firstSample;
        d.rawAnchor = info.utcNanoseconds;
        d.outputAnchor = d.rawAnchor - d.delayNs;
        d.pendingReset = true;
        if (newEpoch) { d.budgets = {}; d.lastFillLog = 0; }
        if (info.changeReason == SDRStreamChange::None)
            info.changeReason = d.started ? SDRStreamChange::StreamRestart : SDRStreamChange::Startup;
        d.resetInfo = info;
    }
    d.current = info;
    d.started = true;
    d.expected = info.firstSample + count;
    std::memcpy(d.input.data(), iq, count*sizeof(liquid_float_complex));
    d.emit(d.input.data(), count);
}
