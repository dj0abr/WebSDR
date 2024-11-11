#ifndef FFT_PROCESSOR_H
#define FFT_PROCESSOR_H

#include <vector>
#include <array>
#include <fftw3.h>
#include <complex>
#include <chrono>
#include <thread>
#include <boost/lockfree/spsc_queue.hpp>
#include "global.h"
#include "liquid.h"

// Constants
const int SAMPLE_RATE = 480000;   // 480 kS/s
const int FFT_SIZE = 16384;        // FFT size

class FFTProcessor {
public:
    // Singleton instance
    static FFTProcessor& getInstance();
    
    void startFFTThread();                  // Start the FFT thread
    void pushFFTinputSamples(SampleData data);   // push received samples into the FFT input queue

    // Read data from the FFT queue
    bool readFFTQueue(std::array<float, 1025>& data);

private:
    FFTProcessor();  // Private constructor for Singleton
    ~FFTProcessor(); // Destructor to clean up FFT resources

    FFTProcessor(const FFTProcessor&) = delete;               // No copy constructor
    FFTProcessor& operator=(const FFTProcessor&) = delete;    // No assignment operator

    // Initialize and clean up FFT
    void initFFT();
    void cleanupFFT();

    // FFT processing thread
    void processFFTThread();

    // Apply a window to the samples (Hamming window)
    void applyWindow(std::vector<std::complex<float>>& data);

    std::vector<float> rearrange_fft_output(fftwf_complex* fftOut, size_t fftSize);
    std::vector<float> downscale_fft_bins_f(const std::vector<float>& bins, float firstFrequency, float lastFrequency, float maxFrequency, size_t targetSize);
    std::vector<float> downscale_fft_bins(const std::vector<float>& bins, size_t firstBin, size_t lastBin, size_t targetSize);

    // FFTW plan and output
    fftwf_plan fftPlan;
    fftwf_complex* fftOut;

    // Queue for samples from the SDRplay callback
    // any number of samples, queue can store 1024 packets
    boost::lockfree::spsc_queue<SampleData, boost::lockfree::capacity<1024>> queue480;

    // Helper variables
    std::chrono::steady_clock::time_point lastUpdateTime;
};

#endif // FFT_PROCESSOR_H
