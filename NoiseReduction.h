#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <speex/speex_preprocess.h>

// Per-listener voice denoiser, adapted from QO100_TRX2. Call only from the
// owning client thread. Input/output are normalized floats at 8 kHz.
class NoiseReduction {
public:
    static constexpr std::size_t frameSize = 160; // 20 ms
    NoiseReduction();
    void set(unsigned percent);
    unsigned percent() const { return percent_; }
    void reset();
    // Append complete frames, keeping up to 159 input samples for the next call.
    // Even in bypass Speex keeps learning; the dry float samples stay exact.
    void process(const float* input, std::size_t count, std::vector<float>& output,
                 bool voiceMode = true);
private:
    std::unique_ptr<SpeexPreprocessState, decltype(&speex_preprocess_state_destroy)>
        state_{nullptr, speex_preprocess_state_destroy};
    std::array<float, frameSize> dry_{};
    std::size_t used_ = 0;
    unsigned percent_ = 0;
    float mix_ = 0;
};
