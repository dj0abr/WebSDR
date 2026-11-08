#include "NoiseReduction.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

NoiseReduction::NoiseReduction() { reset(); }

void NoiseReduction::reset() {
    state_.reset(speex_preprocess_state_init(frameSize, 8000));
    if (!state_) throw std::runtime_error("Cannot create SpeexDSP denoiser");
    int enabled = 1, disabled = 0;
    speex_preprocess_ctl(state_.get(), SPEEX_PREPROCESS_SET_DENOISE, &enabled);
    speex_preprocess_ctl(state_.get(), SPEEX_PREPROCESS_SET_AGC, &disabled);
    // VAD and dereverb retain their disabled defaults, as in QO100_TRX2.
    used_ = 0;
    mix_ = 0;
    set(percent_);
}

void NoiseReduction::set(unsigned percent) {
    if (percent > 100) throw std::invalid_argument("NR percent must be 0..100");
    percent_ = percent;
    int attenuation = -std::max(1, int(std::lround(.3 * percent)));
    speex_preprocess_ctl(state_.get(), SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &attenuation);
}

void NoiseReduction::process(const float* input, std::size_t count,
                             std::vector<float>& output, bool voiceMode) {
    for (std::size_t i = 0; i < count; ++i) {
        dry_[used_++] = input[i];
        if (used_ != frameSize) continue;
        std::array<spx_int16_t, frameSize> wet;
        for (std::size_t j = 0; j < frameSize; ++j) {
            const float value = std::isfinite(dry_[j]) ? dry_[j] : 0.f;
            wet[j] = static_cast<spx_int16_t>(std::lround(std::clamp(value, -1.f, 1.f) * 32767.f));
        }
        speex_preprocess_run(state_.get(), wet.data());
        const float target = percent_ && voiceMode ? 1.f : 0.f;
        for (std::size_t j = 0; j < frameSize; ++j) {
            mix_ += std::clamp(target - mix_, -1.f / frameSize, 1.f / frameSize);
            if (mix_ < 1e-5f) mix_ = 0;
            if (mix_ > 1 - 1e-5f) mix_ = 1;
            output.push_back(mix_ == 0 ? dry_[j] :
                dry_[j] + mix_ * (wet[j] / 32767.f - dry_[j]));
        }
        used_ = 0;
    }
}
