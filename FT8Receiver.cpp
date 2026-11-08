#include <complex>
#include "liquid.h"
#include "FT8Receiver.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>

namespace {
constexpr std::uint32_t inputRate = 480000, audioRate = 12000;
constexpr std::int64_t slotSamples = 180000;
std::int64_t audioTick(std::int64_t ns) {
    return (ns / 1000000000LL) * audioRate +
        std::llround((ns % 1000000000LL) * (double(audioRate) / 1e9));
}
}

struct FT8Receiver::Impl {
    SlotSink sink;
    std::int64_t offsetNs, delayNs = 0, anchorNs = 0, firstTick = 0;
    std::uint64_t generation = 0, expectedInput = 0, produced = 0, received = 0;
    std::uint32_t dial = 0;
    bool started = false, collecting = false;
    FT8Slot slot;
    nco_crcf oscillator = nullptr;
    msresamp_crcf resampler = nullptr;
    ampmodem usb = nullptr;
    firfilt_rrrf audioFilter = nullptr;
    std::vector<liquid_float_complex> input, narrow;

    Impl(SlotSink callback, std::int64_t offset) : sink(std::move(callback)), offsetNs(offset) {
        try {
            oscillator = nco_crcf_create(LIQUID_VCO);
            resampler = msresamp_crcf_create(float(audioRate)/inputRate, 70);
            usb = ampmodem_create(0.99f, LIQUID_AMPMODEM_USB, 1);
            audioFilter = firfilt_rrrf_create_kaiser(129, 4500.0f/audioRate, 70, 0);
            if (!oscillator || !resampler || !usb || !audioFilter)
                throw std::runtime_error("cannot create FT8 DSP objects");
            // Runtime Liquid 1.6 reports the resampler delay in INPUT samples
            // (the bundled old header incorrectly labels them output samples).
            delayNs = std::llround(1e9 * (msresamp_crcf_get_delay(resampler)/inputRate +
                       (ampmodem_get_delay_demod(usb) + 64.0)/audioRate));
        } catch (...) { destroy(); throw; }
    }
    void destroy() {
        if (audioFilter) firfilt_rrrf_destroy(audioFilter);
        if (usb) ampmodem_destroy(usb);
        if (resampler) msresamp_crcf_destroy(resampler);
        if (oscillator) nco_crcf_destroy(oscillator);
    }
    ~Impl() { destroy(); }
    void reset(const SDRStreamInfo& info, std::uint32_t frequency,
               SDRStreamChange reason, std::int64_t sampleDelta, std::int64_t clockDelta) {
        if (collecting && !slot.audio.empty()) {
            const std::time_t seconds = slot.utcSeconds;
            std::tm utc{};
            char label[16] = "unknown", detail[128] = "";
            if (gmtime_r(&seconds, &utc)) std::strftime(label, sizeof(label), "%H%M%S", &utc);
            if (reason == SDRStreamChange::HardwareSampleGap ||
                reason == SDRStreamChange::QueueSampleGap || reason == SDRStreamChange::IQSampleGap)
                std::snprintf(detail, sizeof(detail), ", sample delta %+lld at %u samples/s",
                    static_cast<long long>(sampleDelta),
                    reason == SDRStreamChange::IQSampleGap ? inputRate : 2400000U);
            else if (reason == SDRStreamChange::ClockStep || reason == SDRStreamChange::TimestampMismatch)
                std::snprintf(detail, sizeof(detail), ", time delta %+.3f ms", clockDelta/1e6);
            std::fprintf(stderr, "[FT8] discarding partial UTC slot %s (%.6f MHz): %s%s; collected %zu/180000 samples\n",
                label, slot.dialHz/1e6, sdrStreamChangeName(reason), detail, slot.audio.size());
        }
        if (!started || frequency != dial) {
            if (frequency) std::fprintf(stderr,
                "[FT8] USB %.6f MHz, 12000 samples/s; waiting for complete UTC slot\n", frequency/1e6);
            else std::fprintf(stderr, "[FT8] Inactive: no FT8 frequency in current segment\n");
        }
        started = true; dial = frequency; generation = info.generation;
        received = produced = 0; anchorNs = info.utcNanoseconds;
        firstTick = audioTick(anchorNs - delayNs + offsetNs);
        collecting = false; slot.audio.clear();
        nco_crcf_reset(oscillator);
        nco_crcf_set_frequency(oscillator, 2*M_PI*(double(dial)-info.centerHz)/inputRate);
        msresamp_crcf_reset(resampler); ampmodem_reset(usb); firfilt_rrrf_reset(audioFilter);
    }
};

FT8Receiver::FT8Receiver(SlotSink sink, std::int64_t timeOffsetNs)
    : impl_(new Impl(std::move(sink), timeOffsetNs)) {}
FT8Receiver::~FT8Receiver() = default;

std::int64_t FT8Receiver::configuredTimeOffset() {
    const char* text = std::getenv("FT8_TIME_OFFSET_MS");
    if (!text) return 0;
    char* end = nullptr;
    const double ms = std::strtod(text, &end);
    if (end == text || *end || !std::isfinite(ms) || std::abs(ms) > 5000) {
        std::fprintf(stderr, "[FT8] Invalid FT8_TIME_OFFSET_MS; using 0\n");
        return 0;
    }
    return std::llround(ms*1e6);
}

std::uint32_t FT8Receiver::dialFrequency(const SDRStreamInfo& info) {
    constexpr std::uint32_t frequencies[]{1840000,3573000,5357000,7074000,
        10136000,14074000,18100000,21074000,24915000,28074000,50313000,
        70154000,144174000,432174000};
    for (auto frequency : frequencies)
        if (frequency >= info.startHz && std::uint64_t(frequency)+4500 <= info.endHz &&
            std::abs(double(frequency)-info.centerHz)+4500 < 234000)
            return frequency;
    return 0;
}

void FT8Receiver::process(const void* iq, std::size_t count, const SDRStreamInfo& info) {
    if (!count) return;
    auto& d = *impl_;
    const auto frequency = dialFrequency(info);
    const auto predictedUtc = d.anchorNs + sdrSampleTime(d.received, inputRate);
    auto reason = SDRStreamChange::None;
    auto sampleDelta = info.sampleDelta, clockDelta = info.clockDeltaNs;
    if (!d.started) reason = SDRStreamChange::Startup;
    else if (frequency != d.dial) reason = SDRStreamChange::BandChange;
    else if (info.discontinuity || info.generation != d.generation)
        reason = info.changeReason == SDRStreamChange::None ?
            SDRStreamChange::StreamRestart : info.changeReason;
    else if (info.firstSample != d.expectedInput) {
        reason = SDRStreamChange::IQSampleGap;
        sampleDelta = static_cast<std::int64_t>(info.firstSample) -
                      static_cast<std::int64_t>(d.expectedInput);
    } else if (std::abs(info.utcNanoseconds - predictedUtc) > 2000000LL) {
        reason = SDRStreamChange::TimestampMismatch;
        clockDelta = info.utcNanoseconds - predictedUtc;
    }
    if (reason != SDRStreamChange::None) d.reset(info, frequency, reason, sampleDelta, clockDelta);
    d.expectedInput = info.firstSample + count;
    d.received += count;
    if (!frequency) return;
    if (!iq || count > 16384) throw std::runtime_error("invalid FT8 IQ block");
    static_assert(sizeof(liquid_float_complex) == 2*sizeof(float), "interleaved float IQ required");
    d.input.resize(count);
    std::memcpy(d.input.data(), iq, count*sizeof(liquid_float_complex));
    for (auto& sample : d.input) {
        nco_crcf_mix_down(d.oscillator, sample, &sample);
        nco_crcf_step(d.oscillator);
    }
    d.narrow.resize(static_cast<std::size_t>(std::ceil(2.0*count*audioRate/inputRate))+64);
    unsigned int outputs = 0;
    msresamp_crcf_execute(d.resampler, d.input.data(), count, d.narrow.data(), &outputs);
    for (unsigned int i = 0; i < outputs; ++i) {
        float demodulated, audio;
        ampmodem_demodulate(d.usb, d.narrow[i], &demodulated);
        firfilt_rrrf_push(d.audioFilter, demodulated);
        firfilt_rrrf_execute(d.audioFilter, &audio);
        const auto tick = d.firstTick + static_cast<std::int64_t>(d.produced++);
        if (tick < 0) continue;
        const auto index = tick % slotSamples;
        if (index == 0) {
            d.slot = FT8Slot{};
            d.slot.utcSeconds = (tick / slotSamples)*15;
            d.slot.dialHz = frequency;
            d.slot.audio.reserve(slotSamples);
            d.collecting = true;
        }
        if (!std::isfinite(audio)) { d.collecting = false; d.slot.audio.clear(); }
        if (!d.collecting) continue;
        d.slot.audio.push_back(audio);
        if (d.slot.audio.size() == slotSamples) {
            d.collecting = false;
            d.sink(std::move(d.slot));
        }
    }
}
