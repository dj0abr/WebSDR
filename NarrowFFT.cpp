#include "NarrowFFT.h"
#include "WebSocketServer.h"
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <cmath>

// Constructor and Destructor remain unchanged
NarrowFFTProcessor::NarrowFFTProcessor(size_t fftSize, float calibrationConstant)
    : fftSize_(fftSize), calibrationConstant_(calibrationConstant) {
    fftIn_ = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * fftSize_);
    fftOut_ = (fftwf_complex*) fftwf_malloc(sizeof(fftwf_complex) * fftSize_);
    fftPlan_ = fftwf_plan_dft_1d(fftSize_, fftIn_, fftOut_, FFTW_FORWARD, FFTW_ESTIMATE);
    lastUpdate_ = std::chrono::steady_clock::now();

    startProcessing();
}

NarrowFFTProcessor::~NarrowFFTProcessor() {
    keepRunning = false;  // Signal the thread to exit

    if (processingThread_.joinable()) {
        processingThread_.join();  // Wait for fftProcessing thread to finish
    }

    if (fftPlan_) {
        fftwf_destroy_plan(fftPlan_);
        fftPlan_ = nullptr;
    }
    if (fftIn_) {
        fftwf_free(fftIn_);
        fftIn_ = nullptr;
    }
    if (fftOut_) {
        fftwf_free(fftOut_);
        fftOut_ = nullptr;
    }
}


void NarrowFFTProcessor::startProcessing() {    
    processingThread_ = std::thread(&NarrowFFTProcessor::fftProcessing, this);
}

// Pushes sample data into the input queue
bool NarrowFFTProcessor::pushSampleData(const ClientInfo& data) {
    if (!narrowInputQueue_.push(data)) {
        //std::cerr << "Queue is full; dropping data" << std::endl;
        return false;
    }
    return true;
}


std::vector<float> NarrowFFTProcessor::downscaleFftBins(const std::vector<float>& bins, size_t targetSize) {
    if (bins.size() < targetSize * 2) {
        throw std::invalid_argument("Input vector size must be at least 2 * targetSize.");
    }

    size_t binSize = bins.size();
    float groupSize = static_cast<float>(binSize) / targetSize;
    std::vector<float> result;
    result.reserve(targetSize);

    for (size_t i = 0; i < targetSize; ++i) {
        size_t startIdx = static_cast<size_t>(i * groupSize);
        size_t endIdx = std::min(static_cast<size_t>((i + 1) * groupSize), binSize);
        result.push_back(*std::max_element(bins.begin() + startIdx, bins.begin() + endIdx));
    }

    return result;
}

std::vector<float> NarrowFFTProcessor::rearrangeFftOutput() {
    std::vector<float> output(fftSize_);

    for (size_t i = fftSize_ / 2; i < fftSize_; ++i) {
        float magnitude = std::sqrt(fftOut_[i][0] * fftOut_[i][0] + fftOut_[i][1] * fftOut_[i][1]);
        float dBm = 20 * log10(magnitude) + calibrationConstant_;
        output[i - fftSize_ / 2] = dBm;
    }

    for (size_t i = 0; i < fftSize_ / 2; ++i) {
        float magnitude = std::sqrt(fftOut_[i][0] * fftOut_[i][0] + fftOut_[i][1] * fftOut_[i][1]);
        float dBm = 20 * log10(magnitude) + calibrationConstant_;
        output[i + fftSize_ / 2] = dBm;
    }

    return output;
}

void NarrowFFTProcessor::fftProcessing() {
    while (keeprunning && keepRunning) {  // Uses the global keeprunning variable
        ClientInfo data;

        // Attempt to pop data from the narrow input queue
        if (narrowInputQueue_.pop(data)) {
            if (data.messageId == 3) {  // Ensure it's raw data before accessing sdata
                int currentClientID = data.clientId; // Store client ID locally

                for (const auto& sample : data.sdata) {
                    sampleBuffer.push_back(sample); // Buffer samples

                    if (sampleBuffer.size() == fftSize_) {
                        // Ensure fftIn_ and fftOut_ are allocated before using
                        if (!fftIn_ || !fftOut_) {
                            std::cerr << "FFT buffers not allocated!" << std::endl;
                            return;  // Exit the function if not allocated
                        }

                        for (size_t i = 0; i < fftSize_; ++i) {
                            fftIn_[i][0] = sampleBuffer[i].real;
                            fftIn_[i][1] = sampleBuffer[i].imag;
                        }

                        fftwf_execute(fftPlan_);

                        std::vector<float> rearrangedOutput = rearrangeFftOutput();
                        std::vector<float> downscaledOutput = downscaleFftBins(rearrangedOutput, 1024);

                        std::array<float, 1025> bins1024;
                        bins1024[0] = 1.0f;
                        std::copy_n(downscaledOutput.begin(), 1024, bins1024.begin() + 1);

                        auto now = std::chrono::steady_clock::now();
                        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate_).count() >= 100) {
                            processBinsOutput(bins1024, currentClientID);  // Pass currentClientID
                            lastUpdate_ = now;
                        }

                        sampleBuffer.clear();
                    }
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void NarrowFFTProcessor::processBinsOutput(const std::array<float, 1025>& bins1024, int clientID) {
    WebSocketServer& WSSinstance = WebSocketServer::getInstance();
    WSSinstance.sendDataToClient(bins1024, clientID);
}
