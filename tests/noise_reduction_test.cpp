#include "NoiseReduction.h"
#include "SignalDecoder.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>

int main() {
    constexpr std::size_t rate = 8000, n = 16 * rate;
    constexpr double pi = 3.141592653589793;
    std::mt19937 random(78129);
    std::normal_distribution<double> noise(0, 1700. / 32767);
    std::vector<float> input(n), clean(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = double(i) / rate;
        double voice = 0;
        if (t >= 6 && std::fmod(t - 6, .65) < .4) {
            const auto envelope = std::pow(std::sin(pi * std::fmod(t - 6, .65) / .4), 2);
            for (int k = 1; k <= 18; ++k)
                voice += 3400. / 32767 * envelope * std::sin(k * (2*pi*155*t + 1.4*std::sin(2*pi*2.7*t))) / k;
        }
        clean[i] = voice;
        input[i] = std::clamp(voice + noise(random), -1., 1.);
    }
    auto run = [&](unsigned percent, std::size_t chunk) {
        NoiseReduction nr;
        nr.set(percent);
        std::vector<float> output;
        for (std::size_t i = 0; i < n; i += chunk)
            nr.process(input.data() + i, std::min(chunk, n - i), output);
        assert(output.size() == input.size());
        return output;
    };
    const auto started = std::chrono::steady_clock::now();
    const auto dry = run(0, 1024), medium = run(50, 97), strong = run(100, 1024);
    assert(dry == input); // Includes values not exactly representable as PCM16.
    assert(strong == run(100, 160));
    auto energy = [&](const auto& samples, std::size_t begin, std::size_t end) {
        double sum = 0;
        for (auto i = begin; i < end; ++i) sum += double(samples[i]) * samples[i];
        return sum / (end - begin);
    };
    const double midDb = 10 * std::log10(energy(input,4*rate,6*rate) / energy(medium,4*rate,6*rate));
    const double highDb = 10 * std::log10(energy(input,4*rate,6*rate) / energy(strong,4*rate,6*rate));
    assert(midDb > 7 && highDb > 15 && highDb > midDb + 4);
    double projection = 0, reference = 0;
    for (std::size_t i = 7*rate; i < n; ++i) {
        const double c = clean[i - NoiseReduction::frameSize];
        projection += c * strong[i]; reference += c * c;
    }
    const double voiceGain = projection / reference;
    assert(voiceGain > .35 && voiceGain < 1.2);

    NoiseReduction nr;
    nr.set(100);
    std::vector<float> output;
    nr.process(input.data(), 160, output);
    nr.set(0);
    nr.process(input.data(), 160, output); // Short crossfade.
    output.clear();
    nr.process(input.data(), 160, output);
    assert(std::equal(output.begin(), output.end(), input.begin()));
    nr.set(100);
    nr.process(input.data(), 17, output); // A reset must discard even partial old frames.
    nr.reset(); output.clear();
    std::vector<float> zeros(160, 0.f);
    nr.process(zeros.data(), zeros.size(), output);
    assert(output == zeros && nr.percent() == 100);
    bool rejected = false;
    try { nr.set(101); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected && nr.percent() == 100);
    output.clear(); nr.reset();
    nr.process(input.data(), 160, output, false);
    assert(std::equal(output.begin(), output.end(), input.begin()));

    // Exercise the actual decoder: wire marker, silence and per-listener AGC.
    SignalDecoder silence;
    ClientInfo empty{};
    assert(silence.demodulate(empty).empty());
    ClientInfo zeroIQ{}; zeroIQ.sdata.assign(960, {0,0});
    unsigned packets = 0;
    for (int i = 0; i < 100; ++i) {
        auto packet = silence.demodulate(zeroIQ);
        if (packet.empty()) continue;
        assert(packet.size() == 1025 && packet[0] == 3);
        assert(std::all_of(packet.begin()+1, packet.end(), [](float x) { return x == 0; }));
        ++packets;
    }
    assert(packets >= 14 && packets <= 16);
    SignalDecoder referenceDecoder, listener, other;
    ClientInfo quiet{}, loud{};
    quiet.sdata.resize(960); loud.sdata.resize(960);
    for (int block = 0; block < 100; ++block) {
        for (std::size_t i = 0; i < quiet.sdata.size(); ++i) {
            const auto phase = 2*pi*1200*(block*960+i)/48000.;
            quiet.sdata[i] = .0004f * std::complex<float>(std::cos(phase), std::sin(phase));
            loud.sdata[i] = 1000.f * quiet.sdata[i];
        }
        const auto expected = referenceDecoder.demodulate(quiet);
        other.demodulate(loud);
        assert(listener.demodulate(quiet) == expected);
    }
    assert(listener.setNoiseReduction(80));
    listener.setMode(2);
    assert(listener.getNoiseReduction() == 80 && listener.getEffectiveNoiseReduction() == 0);
    listener.setMode(0);
    assert(listener.getEffectiveNoiseReduction() == 80);
    for (float invalid : {-2.f, 101.f, .5f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()})
        assert(!listener.setNoiseReduction(invalid) && listener.getNoiseReduction() == 80);
    assert(other.getNoiseReduction() == 0);
    std::cout << "8 kHz SpeexDSP: 50%=" << midDb << " dB, 100%=" << highDb
              << " dB stationary-noise attenuation; voice gain=" << voiceGain
              << "; " << std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()
              << " s. Float bypass, arbitrary chunks, reset, FM, packet format and independent AGC passed.\n";
}
