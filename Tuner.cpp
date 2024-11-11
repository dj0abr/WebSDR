#include "Tuner.h"
#include "SDRHardware.h"
#include "liquid.h"

// Constructor
Tuner::Tuner() :
    frequency_shift(0.0f),
    change_frequency(0) {
    setupTuner();
}

// Destructor
Tuner::~Tuner() {
    if(resampler_480to48) {
        msresamp_crcf_destroy(resampler_480to48);
        resampler_480to48 = nullptr;
    }
    if(nco) {
        nco_crcf_destroy(nco);
        nco = nullptr;
    }
}

// Setup method to initialize the SDR components
void Tuner::setupTuner() {
    
    float As_480to48 = 60.0f;

    // Initialize resamplers
    resampler_480to48 = msresamp_crcf_create(r_480to48, As_480to48);

    // Initialize the frequency shifter (NCO)
    nco = nco_crcf_create(LIQUID_NCO);
    normalized_frequency = 2.0f * M_PI * frequency_shift / SAMPLE_RATE;
    nco_crcf_set_frequency(nco, normalized_frequency);
}

// Set RX frequency offset
// value: offset Frquency above band start
void Tuner::setRXFrequencyOffset(float value) {
    //printf("setRXFrequencyOffset value:%f\n",value);
    // Band start offet is at -240kHz
    frequency_shift = value - 240000.0f;
    normalized_frequency = 2.0f * M_PI * frequency_shift / SAMPLE_RATE;
    change_frequency = 1;
}

float Tuner::getFrequencyShift() {
    return frequency_shift;
}

// Decode the SSB signal
ClientInfo Tuner::doTuning(ClientInfo clientInfo) {

    liquid_float_complex *data = clientInfo.sdata.data();
    // data are the raw I/Q samples in liquid DSP format
    // with a speed of 480 kS/s

    // Mix down of the wanted frequency to the baseband
    if(change_frequency == 1) {
        change_frequency = 0;
        nco_crcf_set_frequency(nco, normalized_frequency);
    }
    unsigned int len480 = clientInfo.sdata.size();
    liquid_float_complex samplesBaseband[len480];
    for (unsigned int i = 0; i < len480; i++) {
        nco_crcf_mix_down(nco, data[i], &samplesBaseband[i]);
        nco_crcf_step(nco);
    }

    // samplesBaseband are the I/Q samples of the wanted frequency
    // with a speed of 480 kS/s

    // all other processing is done at 48 kS/s, so downsample by 10
    unsigned int num_samples_48_expected = (unsigned int)(r_480to48 * len480 + 0.5f);
    liquid_float_complex samples_48[num_samples_48_expected];
    unsigned int num_samples_48;
    msresamp_crcf_execute(resampler_480to48, samplesBaseband, len480, samples_48, &num_samples_48);

    // samples_48 are the I/Q samples of the wanted frequency
    ClientInfo samples_baseband_48;
    samples_baseband_48.sdata.assign(samples_48, samples_48 + num_samples_48);

    return samples_baseband_48;
}
