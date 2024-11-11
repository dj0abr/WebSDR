#ifndef Tuner_H
#define Tuner_H

#include <vector>
#include <boost/lockfree/spsc_queue.hpp>
#include <complex>
#include "liquid.h"
#include <array>
#include <iostream>
#include "global.h"

class Tuner {
public:
    // Constructor
    Tuner();

    // Destructor
    ~Tuner();

    // Setup method for initializing SDR components
    void setupTuner();

    // Method for setting RX frequency offset
    void setRXFrequencyOffset(float value);

    // read the current freq shift
    float getFrequencyShift();

    // Decode method for processing samples
    ClientInfo doTuning(ClientInfo clientInfo);

private:
    msresamp_crcf resampler_480to48 = nullptr;
    nco_crcf nco = nullptr;

    float normalized_frequency;
    float frequency_shift;
    int change_frequency;

    // Helper methods
    float roundToNearestStep(float num, float step);

    // Constants
    const float SAMPLE_RATE = 480000.0f;
    const float AUDIO_SAMPLE_RATE = 48000.0f;
    float r_480to48 = AUDIO_SAMPLE_RATE / SAMPLE_RATE;
};

#endif // Tuner_H
