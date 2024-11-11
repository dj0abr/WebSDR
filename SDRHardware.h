#ifndef SDR_HARDWARE_H
#define SDR_HARDWARE_H

#include <vector>             // Needed for std::vector in SampleData
#include <boost/lockfree/spsc_queue.hpp> // Needed for the lock-free queue
#include "sdrplay_api.h"      // Needed for the API types in the class declaration
#include "liquid.h"


class SDRHardware {
public:
    static SDRHardware& getInstance();
    bool init();
    void setBand(float band);   // set band value from another thread
    void changeBand();          // reads the new band and sets the tuner
    float getTuningFrequency(); // reads the tuner frequency

private:
    SDRHardware();
    ~SDRHardware();

    static void StreamACallback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext);
    static void EventCallback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext);
    void convertToLiquidDSPFormat(short *xi, short *xq, unsigned int numSamples, liquid_float_complex* output);

    sdrplay_api_DeviceT devices[4];
    unsigned int numDevs;
    sdrplay_api_DeviceParamsT *deviceParams;
    sdrplay_api_RxChannelParamsT *chParams;
    sdrplay_api_ErrT err;
    sdrplay_api_DeviceT *chosenDevice;
    uint32_t TUNED_FREQUENCY;
    const uint32_t SDR_SAMPLE_RATE;
    float band;
    std::atomic<bool> bandReady = false;

    // resampler 2400 kS/s to 480 kS/s
    msresamp_crcf resampler_2400to480 = nullptr;
    float r_2400to480 = 480.0f / 2400.0f;   // Resampling ratio: 480 kS/s / 2400 kS/s = 0.2
    float As_2400to480 = 60.0f;             // Stop-band attenuation in dB (60 dB is a good choice)
};

#endif // SDR_HARDWARE_H
