#include "SignalDecoder.h"
#include <cstdint>
#include <iostream>
#include <chrono>

using namespace std::chrono;

// Constructor
SignalDecoder::SignalDecoder() : usblsb(1) {
    setupSignalDecoder();
}

// Destructor
SignalDecoder::~SignalDecoder() {
    // Destroy each filter and set pointers to nullptr
    if (ssb_filter_500) {
        iirfilt_crcf_destroy(ssb_filter_500);
        ssb_filter_500 = nullptr;
    }
    if (ssb_filter_1800) {
        iirfilt_crcf_destroy(ssb_filter_1800);
        ssb_filter_1800 = nullptr;
    }
    if (ssb_filter_2700) {
        iirfilt_crcf_destroy(ssb_filter_2700);
        ssb_filter_2700 = nullptr;
    }
    if (ssb_filter_3600) {
        iirfilt_crcf_destroy(ssb_filter_3600);
        ssb_filter_3600 = nullptr;
    }
    if (demod_usb) {
        ampmodem_destroy(demod_usb);
        demod_usb = nullptr;
    }
    if (demod_lsb) {
        ampmodem_destroy(demod_lsb);
        demod_lsb = nullptr;
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
    // create the SSB filter
    create_bandpass_filter(ssb_filter_500, fc_500, f0_500);
    create_lowpass_filter(ssb_filter_1800, fc_1800, f0_1800);
    create_lowpass_filter(ssb_filter_2700, fc_2700, f0_2700);
    create_lowpass_filter(ssb_filter_3600, fc_3600, f0_3600);

    // Create the amplitude demodulator object for USB
    demod_usb = ampmodem_create(0.99f, LIQUID_AMPMODEM_USB, 1);
    demod_lsb = ampmodem_create(0.99f, LIQUID_AMPMODEM_LSB, 1);

    // FM demodulator setup
    float modulation_index = 0.5f;
    demod_fm = freqdem_create(modulation_index);

    // Create the fractional resampler 48 to 8 kS/s
    resampler_48to8_audio = msresamp_rrrf_create(r_48to8, As_48to8);

    // prepare the audio buffer
    audioSamples.clear();
    audioSamples.insert(audioSamples.begin(), 3.0f);    // 3.0f is the ID for audio samples
}

void SignalDecoder::setMode(float value) {
    usblsb = static_cast<int>(std::round(value));
}

void SignalDecoder::setFilter(float value) {
    filter = static_cast<int>(std::round(value));
    //printf("Filter: %d\n",filter);
}

float SignalDecoder::getUsbLsb() {
    return (float(usblsb));
}

// Decode the SSB signal
std::vector<float> SignalDecoder::demodulate(ClientInfo &data) {
    liquid_float_complex *samples_48 = data.sdata.data();
    unsigned int len48 = data.sdata.size();

    // SSB filter, no filter for FM
    liquid_float_complex filtered_samples[len48];
    for (unsigned int i = 0; i < len48; i++) {
        if(usblsb == 2) {
            // FM signal cannot be filtered here
            filtered_samples[i] = samples_48[i];
        }
        else {
            switch (filter) {
                case 500:   iirfilt_crcf_execute(ssb_filter_500, samples_48[i], &filtered_samples[i]);
                            break;
                case 1800:   iirfilt_crcf_execute(ssb_filter_1800, samples_48[i], &filtered_samples[i]);
                            break;
                case 2700:   iirfilt_crcf_execute(ssb_filter_2700, samples_48[i], &filtered_samples[i]);
                            break;
                case 3600:   iirfilt_crcf_execute(ssb_filter_3600, samples_48[i], &filtered_samples[i]);
                            break;
            }            
        }
    }

    // SSB demodulator
    float usb_audio[len48];  // Buffer for demodulated audio
    for (unsigned int i = 0; i < len48; i++) {
        if (usblsb == 1)
            ampmodem_demodulate(demod_usb, filtered_samples[i], &usb_audio[i]);
        else if(usblsb == 0)
            ampmodem_demodulate(demod_lsb, filtered_samples[i], &usb_audio[i]);
        else if(usblsb == 2)
            freqdem_demodulate(demod_fm, filtered_samples[i], &usb_audio[i]);
    }

    // ===== AGC ======
    static float targetLevel = 0.3f;     // Desired signal level
    static float maxGain = 1000.0f;        // Maximum allowed gain
    static float attack = 1.0f;          // How quickly to reduce gain (faster for loud sounds)
    static float decay = 0.01f;          // How quickly to restore gain (slower for quiet sounds)
    static float gain = 1.0f;            // Initial gain

    for (unsigned int i = 0; i < len48; i++) {
        float absSample = fabs(usb_audio[i] * gain);

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

        usb_audio[i] *= gain;

        // clipping
        usb_audio[i] = std::max(-0.99f, std::min(usb_audio[i], 0.99f));
    }
    // =======================

    // downsample from 48 to 8kS/s 
    unsigned int num_samples_out_8_expected = (unsigned int)(r_48to8 * len48 + 0.5f);
    float samples_8[num_samples_out_8_expected];
    unsigned int num_output_samples_8;
    msresamp_rrrf_execute(resampler_48to8_audio, usb_audio, len48, samples_8, &num_output_samples_8);

    // add the new samples to the buffer
    audioSamples.insert(audioSamples.end(), samples_8, samples_8 + num_output_samples_8);

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
