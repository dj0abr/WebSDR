#ifndef FM_SQUELCH_H
#define FM_SQUELCH_H

#include <complex>
#include "liquid.h"

class FMSquelch {
public:
    FMSquelch();
    ~FMSquelch();
    FMSquelch(const FMSquelch&) = delete;
    FMSquelch& operator=(const FMSquelch&) = delete;

    bool setLevel(float level);
    unsigned level() const { return level_; }
    bool isOpen() const { return open_; }
    void reset();
    liquid_float_complex filterChannel(liquid_float_complex sample);
    float audioGain(liquid_float_complex sample, float discriminator);

private:
    iirfilt_crcf channel_;
    iirfilt_rrrf noise_;
    unsigned level_ = 0;
    unsigned samplesPastThreshold_ = 0;
    float openThreshold_ = 0.0f;
    float closeThreshold_ = 0.0f;
    float noisePower_ = 0.06f;
    float inputPower_ = 0.0f;
    float carrierOffset_ = 0.0f;
    float gain_ = 0.0f;
    bool open_ = false;
};

#endif
