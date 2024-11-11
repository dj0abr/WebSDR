#ifndef NARROWFFT_PROCESSOR_H
#define NARROWFFT_PROCESSOR_H

#include <vector>
#include <thread>
#include <chrono>
#include <cmath>
#include <fftw3.h>
#include <array>
#include <boost/lockfree/spsc_queue.hpp>
#include "global.h"  // Include global variable definitions

class NarrowFFTProcessor {
public:
    explicit NarrowFFTProcessor(size_t fftSize = 8192, float calibrationConstant = -115.0f);
    ~NarrowFFTProcessor();

    void startProcessing();

    // Method to push sample data into the input queue
    bool pushSampleData(const ClientInfo& data);

private:
    int clientID=0;
    size_t fftSize_;
    float calibrationConstant_;
    fftwf_plan fftPlan_ = nullptr;
    fftwf_complex* fftIn_ = nullptr;
    fftwf_complex* fftOut_ = nullptr;
    std::thread processingThread_;
    boost::lockfree::spsc_queue<ClientInfo, boost::lockfree::capacity<1024>> narrowInputQueue_;
    std::vector<liquid_float_complex> sampleBuffer;
    std::chrono::steady_clock::time_point lastUpdate_;
    std::atomic<bool> keepRunning{true};  // Use atomic to ensure thread-safe flag

    std::vector<float> downscaleFftBins(const std::vector<float>& bins, size_t targetSize = 1024);
    std::vector<float> rearrangeFftOutput();
    void fftProcessing();

    // Dummy function to process bins1024 array
    void processBinsOutput(const std::array<float, 1025>& bins1024, int clientID);
};

#endif // NARROWFFT_PROCESSOR_H
