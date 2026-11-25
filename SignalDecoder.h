#ifndef SignalDecoder_H
#define SignalDecoder_H

#include <vector>
#include <boost/lockfree/spsc_queue.hpp>
#include <complex>
#include "liquid.h"
#include <array>
#include <iostream>
#include "global.h"
#include "NoiseReduction.h"
#include "FMSquelch.h"

class SignalDecoder {
    enum Mode { LSB = 0, USB = 1, FM = 2, AM = 3 };

public:
    // Constructor
    SignalDecoder();

    // Destructor
    ~SignalDecoder();

    // Setup method for initializing SDR components
    void setupSignalDecoder();

    // Decode method for processing samples
    std::vector<float> demodulate(ClientInfo &data);

    // The decoder modes use the same values as the browser protocol.
    void setMode(float value);

    // Set the SSB/AM audio bandwidth.
    void setFilter(float value);

    // 0 disables NR. FM bypasses it while retaining the selected strength.
    bool setNoiseReduction(float percent);
    unsigned getNoiseReduction() const { return noiseReduction.percent(); }
    unsigned getEffectiveNoiseReduction() const { return mode == FM ? 0 : noiseReduction.percent(); }
    void resetAudio();

    void setFMSquelch(float level);
    unsigned getFMSquelchLevel() const { return fmSquelch.level(); }
    // WebSocket state codes: off (0), closed (1), open (2).
    unsigned getFMSquelchState() const;
    void resetFMSquelch();

    // read the current OpMode
    float getUsbLsb();

private:
    void create_lowpass_filter(iirfilt_crcf &filter, float fc, float f0);
    void create_bandpass_filter(iirfilt_crcf &filter, float fc, float f0);

    // Constants
    const float BANDWIDTH = 2500.0f;
    const float AUDIO_SAMPLE_RATE = 48000.0f;
    float r_48to8 = 8000.0f / AUDIO_SAMPLE_RATE;
    Mode mode = USB;
    int filter = 3600;
    std::vector<float> audioSamples;
    NoiseReduction noiseReduction;
    FMSquelch fmSquelch;
    float gain = 1.0f; // AGC history belongs to this listener.


    // Baseband filters. AM needs a low-pass filter that preserves the carrier.
    iirfilt_crcf ssb_filter_500 = nullptr;
    iirfilt_crcf am_filter_500 = nullptr;
    iirfilt_crcf lowpass_1800 = nullptr;
    iirfilt_crcf lowpass_2400 = nullptr;
    iirfilt_crcf lowpass_3600 = nullptr;

    float fc_500 = 1000.0f / AUDIO_SAMPLE_RATE;
    float f0_500 = 500.0f / AUDIO_SAMPLE_RATE;
    float fc_1800 = 1800.0f / AUDIO_SAMPLE_RATE; // Normalized cutoff frequency
    float f0_1800 = 0.0f;
    float fc_2400 = 2400.0f / AUDIO_SAMPLE_RATE; // Normalized cutoff frequency
    float f0_2400 = 0.0f;
    float fc_3600 = 3600.0f / AUDIO_SAMPLE_RATE; // Normalized cutoff frequency
    float f0_3600 = 0.0f;
    unsigned int order = 4;  // Filter order, can be adjusted for sharper or smoother roll-off

    // Demodulators
    ampmodem demod_usb = nullptr, demod_lsb = nullptr;
    freqdem demod_fm = nullptr;
    iirfilt_rrrf am_dc_blocker = nullptr;

    // Resampler for Audio 48k to 8k
    msresamp_rrrf resampler_48to8_audio = nullptr;
    float As_48to8 = 60.0f;               // Stop-band attenuation in dB (60 dB is a good choice)
};

#endif // SignalDecoder_H
