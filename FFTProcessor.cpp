#include "FFTProcessor.h"
#include "ClientManager.h"
#include "global.h"
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <chrono>

using std::vector;
using std::complex;
namespace chrono = std::chrono;

// Singleton instance
FFTProcessor& FFTProcessor::getInstance() {
    static FFTProcessor instance;
    return instance;
}

// Constructor
FFTProcessor::FFTProcessor() : fftPlan(nullptr), fftOut(nullptr) {
    lastUpdateTime = chrono::steady_clock::now();
}

// Destructor (clean up FFT resources)
FFTProcessor::~FFTProcessor() {
    cleanupFFT();
}

// Initialize the FFT resources
void FFTProcessor::initFFT() {
    fftOut = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * FFT_SIZE);
    fftPlan = fftwf_plan_dft_1d(FFT_SIZE, nullptr, fftOut, FFTW_FORWARD, FFTW_ESTIMATE);
}

// Cleanup FFT resources
void FFTProcessor::cleanupFFT() {
    if (fftPlan) {
        fftwf_destroy_plan(fftPlan);
        fftPlan = nullptr;
    }
    if (fftOut) {
        fftwf_free(fftOut);
        fftOut = nullptr;
    }
}

void FFTProcessor::pushFFTinputSamples(SampleData data) {
    queue480.push(data);
}

// Apply a Hamming window to the IQ samples
void FFTProcessor::applyWindow(vector<complex<float>>& data) {
    for (size_t i = 0; i < data.size(); ++i) {
        float windowValue = 0.54f - 0.46f * std::cos(2 * M_PI * i / (data.size() - 1));
        data[i] *= windowValue;
    }
}

// FFT processing thread
void FFTProcessor::processFFTThread() {
    SampleData sampleData;
    vector<complex<float>> iqSamples(FFT_SIZE);

    while (keeprunning) {
        int currentIndex = 0;
        int samplesNeeded = FFT_SIZE;

        // Gather enough samples for FFT
        while (currentIndex < samplesNeeded && keeprunning) {
            if (queue480.pop(sampleData)) {
                for (int i = 0; i < sampleData.numSamples && currentIndex < samplesNeeded; ++i) {
                    float iValue = static_cast<float>(sampleData.sdata[i].real());
                    float qValue = static_cast<float>(sampleData.sdata[i].imag());
                    iqSamples[currentIndex] = complex<float>(iValue, qValue);
                    ++currentIndex;
                }
            } else {
                // Sleep if no samples are available
                std::this_thread::sleep_for(chrono::microseconds(1000));
            }
        }

        // Perform FFT if enough samples are collected
        if (currentIndex == FFT_SIZE) {
            applyWindow(iqSamples);

            // Execute FFT
            fftwf_execute_dft(fftPlan, reinterpret_cast<fftwf_complex*>(iqSamples.data()), fftOut);
            // Process FFT output and send to the queue
            vector<float> rearrangedOutput = rearrange_fft_output(fftOut, FFT_SIZE);
            //vector<float> downscaledOutput = downscale_fft_bins(rearrangedOutput, 0.0f, 480000.0f, 1024);
            vector<float> downscaledOutput = downscale_fft_bins_f(rearrangedOutput, 0.0f, (float)(EndQRG - StartQRG), 480000.0f, 1024);

            std::array<float, 1025> bins1024;
            bins1024[0] = 0.0f;  // ID or timestamp (placeholder)
            std::copy_n(downscaledOutput.begin(), 1024, bins1024.begin() + 1);

            // Get the current time and check if 100 ms have passed since the last update
            auto now = chrono::steady_clock::now();
            if (chrono::duration_cast<chrono::milliseconds>(now - lastUpdateTime).count() >= 100) {
                // send to the Client Manager
                ClientManager& CMinstance = ClientManager::getInstance();
                CMinstance.enqueueFFTData(bins1024);

                lastUpdateTime = now;
            }
        }
    }

    // Cleanup when the loop exits
    cleanupFFT();
}

// Function to rearrange FFT output: -24000 Hz to +24000 Hz mapping
// convert values to dBm
vector<float> FFTProcessor::rearrange_fft_output(fftwf_complex* fftOut, size_t fftSize) 
{
    vector<float> output(fftSize);  // Output vector to hold magnitudes
    const float calibration_constant = -115.0;

    // First half of FFT (positive frequencies) is at the start of fftOut
    // Second half (negative frequencies) is at the end of fftOut

    // Copy the negative frequencies (from the second half of fftOut) to the start of output
    for (size_t i = fftSize / 2; i < fftSize; ++i) {
        size_t newIndex = i - fftSize / 2;  // Map to the start of the output array
        float magnitude = std::sqrt(fftOut[i][0] * fftOut[i][0] + fftOut[i][1] * fftOut[i][1]);
        float dBm = 20 * log10(magnitude) + calibration_constant;
        output[newIndex] = dBm;  // Place negative frequencies
    }

    // Copy the positive frequencies (from the first half of fftOut) to the second half of output
    for (size_t i = 0; i < fftSize / 2; ++i) {
        size_t newIndex = i + fftSize / 2;  // Map to the second half of the output array
        float magnitude = std::sqrt(fftOut[i][0] * fftOut[i][0] + fftOut[i][1] * fftOut[i][1]);
        float dBm = 20 * log10(magnitude) + calibration_constant;
        output[newIndex] = dBm;  // Place negative frequencies
    }

    return output;
}

// Downscale bins by specifying the frequency
vector<float> FFTProcessor::downscale_fft_bins_f(const vector<float>& bins, float firstFrequency, float lastFrequency, float maxFrequency, size_t targetSize)
{
    //printf("%f  %f\n",firstFrequency,lastFrequency);
    // Calculate the bin resolution (frequency range per bin)
    float binResolution = maxFrequency / bins.size();

    // Convert frequencies to bin indices
    size_t firstBin = static_cast<size_t>(firstFrequency / binResolution);
    size_t lastBin = static_cast<size_t>(lastFrequency / binResolution);

    // Ensure lastBin does not exceed the maximum index in the bins array
    lastBin = std::min(lastBin, bins.size() - 1);

    // Call the original downscale function with calculated bin indices
    return downscale_fft_bins(bins, firstBin, lastBin, targetSize);
}

// Downscale bins by specifying the bin index
vector<float> FFTProcessor::downscale_fft_bins(const vector<float>& bins, size_t firstBin, size_t lastBin, size_t targetSize = 1024)
{
    // Ensure firstBin and lastBin indices are within the bounds of the bins vector
    if (firstBin >= bins.size() || lastBin >= bins.size() || firstBin > lastBin) {
        printf("%ld %ld %ld\n",bins.size(),firstBin,lastBin);
        throw std::invalid_argument("downscale_fft_bins: Invalid bin range specified.");
    }

    // Check that the selected range is at least targetSize
    if ((lastBin - firstBin + 1) < targetSize) {
        throw std::invalid_argument("downscale_fft_bins: Selected range must be at least targetSize. Start/End Frequencies too close");
    }

    // Calculate the number of bins in the specified range
    size_t rangeSize = lastBin - firstBin + 1;
    
    // Calculate the size of each group within the specified range
    float groupSize = static_cast<float>(rangeSize) / targetSize;

    // Result vector to hold the downscaled data
    vector<float> result;
    result.reserve(targetSize);

    // Loop through the specified bins and downscale
    for (size_t i = 0; i < targetSize; ++i) {
        // Determine the range of indices within the specified bin range
        size_t startIdx = firstBin + static_cast<size_t>(i * groupSize);
        size_t endIdx = firstBin + static_cast<size_t>((i + 1) * groupSize);

        // Ensure endIdx does not exceed lastBin
        endIdx = std::min(endIdx, lastBin + 1);

        // Find the maximum value in the current group
        float maxVal = *std::max_element(bins.begin() + startIdx, bins.begin() + endIdx);

        // Append the maximum value to the result
        result.push_back(maxVal);
    }

    return result;
}

// Start the FFT thread
void FFTProcessor::startFFTThread() {
    initFFT();

    std::thread fftThread([this]() {
        processFFTThread();
    });

    fftThread.detach();
}
