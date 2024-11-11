#ifndef SignalDecoder_H
#define SignalDecoder_H

#include <vector>
#include <boost/lockfree/spsc_queue.hpp>
#include <complex>
#include "liquid.h"
#include <array>
#include <iostream>
#include "global.h"

class SignalDecoder {
public:
    // Constructor
    SignalDecoder();

    // Destructor
    ~SignalDecoder();

    // Setup method for initializing SDR components
    void setupSignalDecoder();

    // Decode method for processing samples
    std::vector<float> demodulate(ClientInfo &data);

    // set decoder mode usb, lsb, fm
    void setMode(float value);

    // set SSB Filter
    void setFilter(float value);

    // read the current OpMode
    float getUsbLsb();

private:
    void create_lowpass_filter(iirfilt_crcf &filter, float fc, float f0);
    void create_bandpass_filter(iirfilt_crcf &filter, float fc, float f0);

    // Constants
    const float BANDWIDTH = 2500.0f;
    const float AUDIO_SAMPLE_RATE = 48000.0f;
    float r_48to8 = 8000.0f / AUDIO_SAMPLE_RATE;
    int usblsb = 0;
    int filter = 3600;
    std::vector<float> audioSamples;

    // SSB Filter
    iirfilt_crcf ssb_filter_500 = nullptr;
    iirfilt_crcf ssb_filter_1800 = nullptr;
    iirfilt_crcf ssb_filter_2700 = nullptr;
    iirfilt_crcf ssb_filter_3600 = nullptr;

    float fc_500 = 1000.0f / AUDIO_SAMPLE_RATE;
    float f0_500 = 500.0f / AUDIO_SAMPLE_RATE;
    float fc_1800 = 1800.0f / AUDIO_SAMPLE_RATE; // Normalized cutoff frequency
    float f0_1800 = 0.0f;
    float fc_2700 = 2400.0f / AUDIO_SAMPLE_RATE; // Normalized cutoff frequency
    float f0_2700 = 0.0f;
    float fc_3600 = 3600.0f / AUDIO_SAMPLE_RATE; // Normalized cutoff frequency
    float f0_3600 = 0.0f;
    unsigned int order = 4;  // Filter order, can be adjusted for sharper or smoother roll-off

    // Demodulators
    ampmodem demod_usb = nullptr, demod_lsb = nullptr;
    freqdem demod_fm = nullptr;

    // Resampler for Audio 48k to 8k
    msresamp_rrrf resampler_48to8_audio = nullptr;
    float As_48to8 = 60.0f;               // Stop-band attenuation in dB (60 dB is a good choice)
};

#endif // SignalDecoder_H
