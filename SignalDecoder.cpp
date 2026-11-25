#include "SignalDecoder.h"
#include <cstdint>
#include <iostream>
#include <chrono>

using namespace std::chrono;

// Constructor
SignalDecoder::SignalDecoder() {
    setupSignalDecoder();
}

// Destructor
SignalDecoder::~SignalDecoder() {
    // Destroy each filter and set pointers to nullptr
    if (ssb_filter_500) {
        iirfilt_crcf_destroy(ssb_filter_500);
        ssb_filter_500 = nullptr;
    }
    if (am_filter_500) {
        iirfilt_crcf_destroy(am_filter_500);
        am_filter_500 = nullptr;
    }
    if (lowpass_1800) {
        iirfilt_crcf_destroy(lowpass_1800);
        lowpass_1800 = nullptr;
    }
    if (lowpass_2400) {
        iirfilt_crcf_destroy(lowpass_2400);
        lowpass_2400 = nullptr;
    }
    if (lowpass_3600) {
        iirfilt_crcf_destroy(lowpass_3600);
        lowpass_3600 = nullptr;
    }
    if (demod_usb) {
        ampmodem_destroy(demod_usb);
        demod_usb = nullptr;
    }
    if (demod_lsb) {
        ampmodem_destroy(demod_lsb);
        demod_lsb = nullptr;
    }
    if (am_dc_blocker) {
        iirfilt_rrrf_destroy(am_dc_blocker);
        am_dc_blocker = nullptr;
    }
    if (demod_fm) {
        freqdem_destroy(demod_fm);
        demod_fm = nullptr;
    }
    if (resampler_48to8_audio) {
        msresamp_rrrf_destroy(resampler_48to8_audio);
        resampler_48to8_audio = nullptr;
    }
}

void SignalDecoder::create_lowpass_filter(iirfilt_crcf &filter, float fc, float f0) {
    filter = iirfilt_crcf_create_prototype(
        LIQUID_IIRDES_ELLIP,    // Filter type
        LIQUID_IIRDES_LOWPASS,  // Lowpass filter
        LIQUID_IIRDES_SOS,      // Second-order sections
        order,                  // Filter order
        fc,                     // Bandwidth
        f0,                     // Center frequency
        1.0f,                   // Ripple (only used in Chebyshev)
        40.0f                   // Stop-band attenuation in dB
    );
}

void SignalDecoder::create_bandpass_filter(iirfilt_crcf &filter, float fc, float f0) {
    filter = iirfilt_crcf_create_prototype(
        LIQUID_IIRDES_CHEBY1,   // Filter type
        LIQUID_IIRDES_BANDPASS, // Bandpass filter
        LIQUID_IIRDES_SOS,      // Second-order sections
        order,                  // Filter order
        fc,                     // Bandwidth
        f0,                     // Center frequency
        0.5f,                   // Ripple (only used in Chebyshev)
        60.0f                   // Stop-band attenuation in dB
    );
}

// Setup method to initialize the SDR components
void SignalDecoder::setupSignalDecoder() {
    create_bandpass_filter(ssb_filter_500, fc_500, f0_500);
    create_lowpass_filter(am_filter_500, 500.0f / AUDIO_SAMPLE_RATE, 0.0f);
    create_lowpass_filter(lowpass_1800, fc_1800, f0_1800);
    create_lowpass_filter(lowpass_2400, fc_2400, f0_2400);
    create_lowpass_filter(lowpass_3600, fc_3600, f0_3600);

    demod_usb = ampmodem_create(0.99f, LIQUID_AMPMODEM_USB, 1);
    demod_lsb = ampmodem_create(0.99f, LIQUID_AMPMODEM_LSB, 1);
    // Remove the carrier's DC level from the AM envelope at about 38 Hz.
    am_dc_blocker = iirfilt_rrrf_create_dc_blocker(0.005f);

    // FM demodulator setup
    float modulation_index = 0.5f;
    demod_fm = freqdem_create(modulation_index);

    // Create the fractional resampler 48 to 8 kS/s
    resampler_48to8_audio = msresamp_rrrf_create(r_48to8, As_48to8);

    // prepare the audio buffer
    audioSamples.clear();
    // The type-3 marker belongs only in the transport header, never in audio.
}

void SignalDecoder::setMode(float value) {
    if (!std::isfinite(value) || value < LSB || value > AM || value != std::round(value)) return;
    const auto next = static_cast<Mode>(value);
    if (next == mode) return;
    mode = next;
    resetAudio();
}

void SignalDecoder::setFilter(float value) {
    if (value == 500 || value == 1800 || value == 2700 || value == 3600)
        filter = static_cast<int>(value);
}

bool SignalDecoder::setNoiseReduction(float percent) {
    if (!std::isfinite(percent) || percent < 0 || percent > 100 || percent != std::round(percent)) return false;
    noiseReduction.set(static_cast<unsigned>(percent));
    return true;
}

void SignalDecoder::resetAudio() {
    if (mode == AM) iirfilt_rrrf_reset(am_dc_blocker);
    resetFMSquelch();
    noiseReduction.reset();
    audioSamples.clear();
    gain = 1.0f;
}

void SignalDecoder::setFMSquelch(float level) {
    const bool wasEnabled = fmSquelch.level() != 0;
    if (!fmSquelch.setLevel(level) || mode != FM || wasEnabled == (fmSquelch.level() != 0)) return;
    if (wasEnabled) {
        audioSamples.clear();
        noiseReduction.discardPendingAudio();
        msresamp_rrrf_reset(resampler_48to8_audio);
    } else {
        resetFMSquelch();
    }
}

unsigned SignalDecoder::getFMSquelchState() const {
    if (mode != FM || fmSquelch.level() == 0) return 0;
    return fmSquelch.isOpen() ? 2 : 1;
}

void SignalDecoder::resetFMSquelch() {
    if (mode != FM || fmSquelch.level() == 0) return;
    fmSquelch.reset();
    freqdem_reset(demod_fm);
    msresamp_rrrf_reset(resampler_48to8_audio);
    audioSamples.clear();
    noiseReduction.discardPendingAudio();
    gain = 1.0f;
}

float SignalDecoder::getUsbLsb() {
    return static_cast<float>(mode);
}

std::vector<float> SignalDecoder::demodulate(ClientInfo &data) {
    if (data.sdata.empty()) return {};
    liquid_float_complex *samples_48 = data.sdata.data();
    unsigned int len48 = data.sdata.size();

    iirfilt_crcf inputFilter = nullptr;
    if (mode != FM) {
        switch (filter) {
            case 500: inputFilter = mode == AM ? am_filter_500 : ssb_filter_500; break;
            case 1800: inputFilter = lowpass_1800; break;
            case 2700: inputFilter = lowpass_2400; break; // Existing protocol ID for 2.4 kHz.
            case 3600: inputFilter = lowpass_3600; break;
        }
    }

    const bool useSquelch = mode == FM && fmSquelch.level() != 0;
    constexpr float targetLevel = 0.3f;
    constexpr float maxGain = 1000.0f;
    constexpr float attack = 1.0f;
    constexpr float decay = 0.01f;

    float audio[len48];
    for (unsigned int i = 0; i < len48; i++) {
        auto sample = samples_48[i];
        if (inputFilter) iirfilt_crcf_execute(inputFilter, sample, &sample);
        if (useSquelch) sample = fmSquelch.filterChannel(sample);
        switch (mode) {
            case LSB: ampmodem_demodulate(demod_lsb, sample, &audio[i]); break;
            case USB: ampmodem_demodulate(demod_usb, sample, &audio[i]); break;
            case FM: freqdem_demodulate(demod_fm, sample, &audio[i]); break;
            case AM: iirfilt_rrrf_execute(am_dc_blocker, std::abs(sample), &audio[i]); break;
        }
        const float gate = useSquelch ? fmSquelch.audioGain(sample, audio[i]) : 1.0f;
        if (gate == 0.0f) {
            audio[i] = 0.0f;
            continue; // Keep the audio AGC from turning up the muted noise.
        }
        float absSample = fabs(audio[i] * gain);

        // If the sample is too loud, reduce gain (attack)
        if (absSample > targetLevel) {
            gain -= attack * (absSample - targetLevel);
        }
        // If the sample is too quiet, increase gain (decay)
        else if (absSample < targetLevel) {
            gain += decay * (targetLevel - absSample);
        }

        // Ensure the gain is within reasonable bounds
        if (gain > maxGain) gain = maxGain;
        if (gain < 1.0f) gain = 1.0f;

        audio[i] *= gain;

        // clipping
        audio[i] = std::max(-0.99f, std::min(audio[i], 0.99f)) * gate;
    }

    // downsample from 48 to 8kS/s 
    // Follow liquid-dsp's documented conservative output-buffer bound.
    std::vector<float> samples_8(static_cast<std::size_t>(std::ceil(1 + 2 * r_48to8 * len48)));
    unsigned int num_output_samples_8;
    msresamp_rrrf_execute(resampler_48to8_audio, audio, len48, samples_8.data(), &num_output_samples_8);

    // Speex frames are independent of the unchanged 1024-sample wire packets.
    noiseReduction.process(samples_8.data(), num_output_samples_8, audioSamples, mode != FM);

    if (audioSamples.size() < 1024) {
        return std::vector<float>();
    }

    // Prepare the output vector with 3.0f as the first element, followed by exactly 1024 samples
    std::vector<float> output(1025); // Create a vector with 1025 elements
    output[0] = 3.0f; // Set the first element to 3.0f
    std::copy(audioSamples.begin(), audioSamples.begin() + 1024, output.begin() + 1); // Copy the next 1024 samples

    // Erase the samples we just used from the buffer
    audioSamples.erase(audioSamples.begin(), audioSamples.begin() + 1024);

    return output;
}
