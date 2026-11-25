#include "SignalDecoder.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

namespace {
constexpr int iqRate = 48000;
constexpr int audioRate = 8000;
constexpr double tau = 6.283185307179586;

class TestSignal {
public:
    std::vector<float> run(SignalDecoder& decoder, int milliseconds, float carrier,
                           float noise, double offset = 0, double deviation = 0) {
        const int end = sample_ + milliseconds * iqRate / 1000;
        std::vector<float> audio;
        ClientInfo input{};
        while (sample_ < end) {
            input.sdata.resize(std::min(173, end - sample_));
            for (auto& sample : input.sdata) {
                const double tone = std::sin(tau * 1000 * sample_ / iqRate);
                phase_ = std::remainder(phase_ + tau * (offset + deviation * tone) / iqRate, tau);
                sample = {carrier * static_cast<float>(std::cos(phase_)) + noise * white_(generator_),
                          carrier * static_cast<float>(std::sin(phase_)) + noise * white_(generator_)};
                ++sample_;
            }
            const auto packet = decoder.demodulate(input);
            if (packet.empty()) continue;
            assert(packet.size() == 1025 && packet.front() == 3.0f);
            audio.insert(audio.end(), packet.begin() + 1, packet.end());
        }
        return audio;
    }

private:
    std::mt19937 generator_{732};
    std::normal_distribution<float> white_;
    int sample_ = 0;
    double phase_ = 0;
};

double amplitude(const std::vector<float>& samples, double frequency) {
    std::complex<double> sum{};
    for (std::size_t i = 0; i < samples.size(); ++i)
        sum += static_cast<double>(samples[i]) * std::polar(1.0, -tau * frequency * i / audioRate);
    return 2 * std::abs(sum) / samples.size();
}

void expectSilence(const std::vector<float>& samples) {
    assert(!samples.empty());
    assert(std::all_of(samples.begin(), samples.end(), [](float value) {
        return std::abs(value) < 1e-7f;
    }));
}
}

int main() {
    constexpr unsigned off = 0, closed = 1, open = 2;
    SignalDecoder decoder;
    TestSignal signal;
    decoder.setMode(2);
    assert(decoder.getFMSquelchState() == off);
    assert(amplitude(signal.run(decoder, 384, 0, 0.025f), 1000) > 0.0001);

    decoder.setFMSquelch(1);
    assert(decoder.getFMSquelchState() == closed);
    expectSilence(signal.run(decoder, 512, 0, 0.03f));
    assert(decoder.getFMSquelchState() == closed);
    decoder.resetFMSquelch();
    expectSilence(signal.run(decoder, 384, 0, 0));
    expectSilence(signal.run(decoder, 512, 0.8f, 0.001f, 12500));
    assert(decoder.getFMSquelchState() == closed);

    decoder.setFMSquelch(50);
    decoder.resetFMSquelch();
    signal.run(decoder, 100, 0.08f, 0.0005f, 900);
    assert(decoder.getFMSquelchState() == open); // A carrier alone is enough.
    const auto modulated = signal.run(decoder, 640, 0.12f, 0.004f, 400, 2500);
    assert(amplitude(modulated, 1000) > 0.04);
    signal.run(decoder, 55, 0, 0.03f);
    assert(decoder.getFMSquelchState() == open);
    signal.run(decoder, 185, 0, 0.03f);
    assert(decoder.getFMSquelchState() == closed);
    signal.run(decoder, 256, 0, 0.03f);
    expectSilence(signal.run(decoder, 256, 0, 0.03f));

    decoder.setFMSquelch(10);
    decoder.resetFMSquelch();
    signal.run(decoder, 384, 0.05f, 0.03f, 400, 2500);
    assert(decoder.getFMSquelchState() == open);
    decoder.setFMSquelch(100);
    decoder.resetFMSquelch();
    expectSilence(signal.run(decoder, 384, 0.05f, 0.03f, 400, 2500));

    SignalDecoder other;
    other.setMode(2);
    assert(other.getFMSquelchState() == off);
    for (float bad : {-2.0f, 12.5f, 101.0f, std::numeric_limits<float>::quiet_NaN()})
        decoder.setFMSquelch(bad);
    assert(decoder.getFMSquelchLevel() == 100);
    decoder.setMode(3);
    assert(decoder.getFMSquelchState() == off);
    decoder.setMode(2);
    assert(decoder.getFMSquelchState() == closed && decoder.getFMSquelchLevel() == 100);
    decoder.setFMSquelch(0);
    assert(decoder.getFMSquelchState() == off);
    assert(amplitude(signal.run(decoder, 384, 0, 0.025f), 1000) > 0.0001);
    std::cout << "FM squelch: noise, adjacent carrier, voice and unmodulated carrier, hang time,"
                 " mode transitions, listener isolation and audio bypass passed.\n";
}
