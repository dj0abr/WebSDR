#ifndef SDR_HARDWARE_H
#define SDR_HARDWARE_H

#include <vector>             // Needed for std::vector in SampleData
#include <boost/lockfree/spsc_queue.hpp> // Needed for the lock-free queue
#include "sdrplay_api.h"      // Needed for the API types in the class declaration
#include "liquid.h"
#include "SDRStreamInfo.h"


class SDRHardware {
public:
    static SDRHardware& getInstance();
    bool init();
    void stop();
    void setBand(float band);   // set band value from another thread
    void changeBand();          // reads the new band and sets the tuner
    float getTuningFrequency(); // reads the tuner frequency

    static constexpr float IN_RATE_HZ   = 2400000.0f;   // Hardware Sample Rate 2.4MS/s

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
    std::atomic<float> band{20};
    std::atomic<std::uint64_t> pendingSegment_{0};
    std::atomic<bool> capturePending_{false};
    std::uint64_t confirmedSegment_ = sdrSegment(14000000, 14350000);
    SDRCaptureClock captureClock_;
    bool streaming_ = false;
    bool apiOpen_ = false;
    std::atomic<bool> bandReady = false;

    static std::array<liquid_float_complex,2048> rxarr;

    std::atomic<bool> adsbRunning_{false};
};

#endif // SDR_HARDWARE_H
