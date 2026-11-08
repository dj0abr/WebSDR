#ifndef ADSB_FFT_H
#define ADSB_FFT_H

#include <vector>
#include <array>
#include <complex>
#include <chrono>
#include <thread>
#include <fftw3.h>

#include "global.h"
#include "SDRQUEUE.h"
#include "ClientManager.h"
#include "SDRHardware.h"

// ADS-B FFT parameters
static constexpr int   ADSB_FFT_SIZE   = 256;        // 256 bins out
static constexpr float ADSB_SAMPLE_RATE = 2400000.0f; // 2.4 MS/s IQ (centered @ 1090 MHz)
static constexpr int   ADSB_UPDATE_MS  = 20;          // send max every 20ms (50 fps)

class ADSB_FFT {
public:
    static ADSB_FFT& getInstance();

    void startFFTThread();

private:
    ADSB_FFT();
    ~ADSB_FFT();

    ADSB_FFT(const ADSB_FFT&) = delete;
    ADSB_FFT& operator=(const ADSB_FFT&) = delete;

    void initFFT();
    void cleanupFFT();
    void processFFTThread();

    void applyWindow(std::vector<std::complex<float>>& data); // Hann (oder Hamming, siehe .cpp)
    std::vector<float> rearrange_fft_output_power(const fftwf_complex* fftOut, size_t fftSize);

private:
    fftwf_plan     fftPlan_ = nullptr;
    fftwf_complex* fftIn_   = nullptr;
    fftwf_complex* fftOut_  = nullptr;

    std::chrono::steady_clock::time_point lastUpdateTime_;
};

#endif // ADSB_FFT_H
