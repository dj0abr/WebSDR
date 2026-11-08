#include "ADSB_FFT.h"
#include <cmath>
#include <atomic>
#include "ClientManager.h"
#include "global.h"
#include "SDRQUEUE.h"
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <chrono>

using std::vector;
using std::complex;
namespace chrono = std::chrono;

// Grober Noise-Floor (kann auch gemittelt werden)
constexpr float noise_floor_db = -110.0f;

ADSB_FFT& ADSB_FFT::getInstance() {
    static ADSB_FFT instance;
    return instance;
}

ADSB_FFT::ADSB_FFT() {
    lastUpdateTime_ = chrono::steady_clock::now();
}

ADSB_FFT::~ADSB_FFT() {
    cleanupFFT();
}

void ADSB_FFT::initFFT() {
    fftIn_  = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * ADSB_FFT_SIZE);
    fftOut_ = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * ADSB_FFT_SIZE);

    if (!fftIn_ || !fftOut_) {
        throw std::runtime_error("ADSB_FFT: FFTW malloc failed");
    }

    fftPlan_ = fftwf_plan_dft_1d(ADSB_FFT_SIZE, fftIn_, fftOut_, FFTW_FORWARD, FFTW_ESTIMATE);
    if (!fftPlan_) {
        throw std::runtime_error("ADSB_FFT: FFTW plan creation failed");
    }
}

void ADSB_FFT::cleanupFFT() {
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

// Für ADS-B ist Hann ok, aber man kann auch Rectangular probieren.
// Hann ist ein guter Start.
void ADSB_FFT::applyWindow(vector<complex<float>>& data) {
    const size_t N = data.size();
    if (N < 2) return;

    for (size_t i = 0; i < N; ++i) {
        float w = 0.5f - 0.5f * std::cos(2.0f * (float)M_PI * (float)i / (float)(N - 1)); // Hann
        data[i] *= w;
    }
}

/**
 * Rearrange (fftshift) + Power->log-Scale für Waterfall
 *
 * WICHTIG: Für Anzeige ist "log power" meist besser als dBm aus Magnitude.
 * Wir nehmen: v = 10*log10(re^2+im^2 + eps)
 */
// fftshift + Power (linear) zurückgeben: p = re^2 + im^2
static inline float mag2(float re, float im) { return re*re + im*im; }

vector<float> ADSB_FFT::rearrange_fft_output_power(const fftwf_complex* fftOut, size_t fftSize) {
    vector<float> output(fftSize);
    const size_t half = fftSize / 2;

    for (size_t i = half; i < fftSize; ++i) {
        size_t newIndex = i - half;
        output[newIndex] = mag2(fftOut[i][0], fftOut[i][1]);
    }
    for (size_t i = 0; i < half; ++i) {
        size_t newIndex = i + half;
        output[newIndex] = mag2(fftOut[i][0], fftOut[i][1]);
    }
    return output;
}

void ADSB_FFT::processFFTThread() {
    vector<complex<float>> iqSamples(ADSB_FFT_SIZE);

    vector<float> peakPower(ADSB_FFT_SIZE, 1e-20f); // sehr klein, aber >0
    constexpr float decay_lin = 0.95f;             // 0.80..0.95 testen
    constexpr float eps = 1e-20f;
    const float calibration_constant = -115.0f;    // wie vorher (GUI erwartet das wohl)

    while (keeprunning) {
        if(!isADSB) {
            std::this_thread::sleep_for(std::chrono::microseconds(10000));
            continue;
        }

        int currentIndex = 0;

        // Gather exactly ADSB_FFT_SIZE samples at 2.4MS/s from SDRQUEUE
        while (currentIndex < ADSB_FFT_SIZE && keeprunning) {
            SDRQUEUE::View v;
            if (SDRQUEUE::tryPop(1, v)) {
                for (unsigned int i = 0; i < v.count && currentIndex < ADSB_FFT_SIZE; ++i) {
                    float iValue = (float)v.data[i].real();
                    float qValue = (float)v.data[i].imag();
                    iqSamples[currentIndex++] = complex<float>(iValue, qValue);
                }
                SDRQUEUE::consume(1);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }

        if (!keeprunning) break;
        if (currentIndex != ADSB_FFT_SIZE) continue;

        // Window + copy into FFTW input buffer
        applyWindow(iqSamples);
        for (int i = 0; i < ADSB_FFT_SIZE; ++i) {
            fftIn_[i][0] = iqSamples[i].real();
            fftIn_[i][1] = iqSamples[i].imag();
        }

        // Execute FFT
        fftwf_execute(fftPlan_);

        vector<float> powerBins = rearrange_fft_output_power(fftOut_, ADSB_FFT_SIZE);

        // DC-Killer (DC liegt bei fftshift in der Mitte)
        const int mid = ADSB_FFT_SIZE / 2;

        // 5-Bin DC-Notch
        float dcRef = std::min({
            powerBins[mid-4], powerBins[mid-3],
            powerBins[mid+3], powerBins[mid+4]
        });

        for (int k = -3; k <= 3; ++k) {
            peakPower[mid + k] *= 0.7f;   // nicht nullen, nur abdämpfen
        }

        // Peak/Decay in linearer Power
        for (int i = 0; i < ADSB_FFT_SIZE; ++i) {
            if (powerBins[i] > peakPower[i]) {
                peakPower[i] = powerBins[i];
            } else {
                // Rauschen schneller abbauen als Peaks
                float localDecay = (peakPower[i] > powerBins[i] * 2.0f) ? 0.97f : 0.90f;
                peakPower[i] *= localDecay;
            }
        }

        // Pack for GUI: 1025 floats, [0]=ID, [1..1024]=bins (stretched)
        std::array<float, 1025> bins1024;
        bins1024[0] = 0.0f;

        // 256 -> 1024: src = i/4
        // Wir nutzen nur ±1 MHz um DC (≈ 213 von 256 Bins)
        constexpr int usedBins = 213;
        constexpr float display_gain_db = +20.0f;

        int start = (ADSB_FFT_SIZE - usedBins) / 2;  // bei 256 => 21

        for (int i = 0; i < 1024; ++i) {
            // linear strecken: 213 → 1024
            int src = start + (i * usedBins) / 1024;

            if (src < 0) src = 0;
            if (src >= ADSB_FFT_SIZE) src = ADSB_FFT_SIZE - 1;

            float p = std::max(peakPower[src], eps);
            float dBm_like = 10.0f * std::log10(p) + calibration_constant + display_gain_db;

            if (dBm_like > noise_floor_db + 10.0f) {
                dBm_like += 3.0f;   // kleiner Boost nur für starke Peaks
            }

            // Unter Noise-Floor zusammenziehen
            float v = dBm_like - noise_floor_db;
            if (v < 0.0f) v = 0.0f;

            // Gamma / Kontrast (0.5…0.8 bewährt)
            constexpr float gamma = 0.6f;
            v = std::pow(v, gamma);

            // zurück in dB-Skala fürs GUI
            bins1024[i + 1] = noise_floor_db + v;

        }

        // Throttle output
        auto now = chrono::steady_clock::now();
        if (chrono::duration_cast<chrono::milliseconds>(now - lastUpdateTime_).count() >= ADSB_UPDATE_MS) {
            ClientManager& cm = ClientManager::getInstance();
            cm.enqueueFFTData(bins1024);
            lastUpdateTime_ = now;
        }
    }

    cleanupFFT();
}

void ADSB_FFT::startFFTThread() {
    initFFT();

    std::thread t([this]() {
        processFFTThread();
    });
    t.detach();
}
