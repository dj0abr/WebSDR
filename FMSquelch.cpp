#include "FMSquelch.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr unsigned sampleRate = 48000;
constexpr unsigned openSamples = sampleRate * 15 / 1000;
constexpr unsigned closeSamples = sampleRate * 120 / 1000;
constexpr float fadeStep = 1.0f / (sampleRate * 0.005f);
constexpr float smoothing = 1.0f / (sampleRate * 0.008f);
// freqdem with kf=0.5 maps a 24 kHz frequency offset to 1.0.
constexpr float maxCarrierOffset = 4000.0f / 24000.0f;
constexpr float minInputPower = 1e-12f;
}

FMSquelch::FMSquelch()
    : channel_(iirfilt_crcf_create_prototype(LIQUID_IIRDES_ELLIP, LIQUID_IIRDES_LOWPASS,
                   LIQUID_IIRDES_SOS, 6, 7500.0f / sampleRate, 0, 0.5f, 55.0f)),
      noise_(iirfilt_rrrf_create_prototype(LIQUID_IIRDES_BUTTER, LIQUID_IIRDES_HIGHPASS,
                   LIQUID_IIRDES_SOS, 4, 4500.0f / sampleRate, 0, 1.0f, 40.0f)) {}

FMSquelch::~FMSquelch() {
    iirfilt_crcf_destroy(channel_);
    iirfilt_rrrf_destroy(noise_);
}

bool FMSquelch::setLevel(float level) {
    if (!std::isfinite(level) || level < 0 || level > 100 || level != std::round(level)) return false;
    if (level == level_) return true;
    level_ = static_cast<unsigned>(level);
    // Higher settings require a quieter discriminator output to open.
    const float rms = 0.14f - 0.10f * level_ / 100.0f;
    openThreshold_ = rms * rms;
    closeThreshold_ = openThreshold_ * 1.5f;
    samplesPastThreshold_ = 0;
    return true;
}

void FMSquelch::reset() {
    iirfilt_crcf_reset(channel_);
    iirfilt_rrrf_reset(noise_);
    samplesPastThreshold_ = 0;
    noisePower_ = 0.06f;
    inputPower_ = carrierOffset_ = gain_ = 0;
    open_ = false;
}

liquid_float_complex FMSquelch::filterChannel(liquid_float_complex sample) {
    iirfilt_crcf_execute(channel_, sample, &sample);
    return sample;
}

float FMSquelch::audioGain(liquid_float_complex sample, float discriminator) {
    float noise;
    iirfilt_rrrf_execute(noise_, discriminator, &noise);
    noisePower_ += smoothing * (noise * noise - noisePower_);
    inputPower_ += smoothing * (std::norm(sample) - inputPower_);
    carrierOffset_ += smoothing * (discriminator - carrierOffset_);

    const bool onChannel = inputPower_ > minInputPower && std::abs(carrierOffset_) < maxCarrierOffset;
    const bool change = open_ ? !onChannel || noisePower_ > closeThreshold_
                              : onChannel && noisePower_ < openThreshold_;
    if (!change) {
        samplesPastThreshold_ = 0;
    } else if (++samplesPastThreshold_ >= (open_ ? closeSamples : openSamples)) {
        open_ = !open_;
        samplesPastThreshold_ = 0;
    }
    gain_ = std::clamp(gain_ + (open_ ? fadeStep : -fadeStep), 0.0f, 1.0f);
    return gain_;
}
