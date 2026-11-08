#include <stdio.h>
#include <cstdint>
#include <iostream>
#include <chrono>
#include <vector>
#include <array>
#include <boost/lockfree/spsc_queue.hpp>
#include "SDRHardware.h"
#include "global.h"
#include "FFTProcessor.h"
#include "ClientManager.h"
#include "ADSB.h"
#include "SDRQUEUE.h"
#include "sdrplay_api.h" // the SDRplay driver must be installed!

using namespace std::chrono;

std::array<liquid_float_complex, 2048> SDRHardware::rxarr{};

// Constructor
SDRHardware::SDRHardware() : numDevs(0), deviceParams(nullptr), chParams(nullptr), err(sdrplay_api_Success), chosenDevice(nullptr), TUNED_FREQUENCY(14240000), SDR_SAMPLE_RATE(2400000) {
    std::cout << "SDRHardware object created.\n";
}

// Destructor (cleans up the API)
SDRHardware::~SDRHardware() {
    stop();
/*    if (chosenDevice) {
        sdrplay_api_Uninit(chosenDevice->dev);
        sdrplay_api_ReleaseDevice(chosenDevice);
    }
    sdrplay_api_Close();*/
    std::cout << "SDRHardware object destroyed and SDRplay API closed.\n";
}

// Singleton implementation
SDRHardware& SDRHardware::getInstance() {
    static SDRHardware instance;  // This creates the single instance of the class
    return instance;
}

// Initializes the SDR hardware
bool SDRHardware::init() {
    printf("Initialize SDRplay hardware\n");

    // Öffne die SDRplay API
    if ((err = sdrplay_api_Open()) != sdrplay_api_Success) {
        printf("sdrplay_api_Open failed: %s\n", sdrplay_api_GetErrorString(err));
        return false;
    }

    apiOpen_ = true;
    printf("SDRplay API opened\n");

    // Geräte scannen
    sdrplay_api_GetDevices(devices, &numDevs, sizeof(devices) / sizeof(sdrplay_api_DeviceT));

    // Überprüfen, ob ein Gerät verfügbar ist
    if (numDevs == 0) {
        printf("ERROR: No RSP devices available.\n");
        return false;
    }
    printf("%d devices detected. devices[0].hwVer = %d\n",numDevs,devices[0].hwVer);

    // Wähle das erste verfügbare Gerät (RSP1A oder RSP1B)
    if (devices[0].hwVer == SDRPLAY_RSP1A_ID) {
        chosenDevice = &devices[0];
        printf("SDRPLAY_RSP1A_ID found\n");
    } 
    else if (devices[0].hwVer == SDRPLAY_RSP1B_ID) {
        chosenDevice = &devices[0];
        printf("SDRPLAY_RSP1B_ID found\n");
    } else {
        printf("ERROR: RSP selected is not available.\n");
        return false;
    }

    // Gerät zur Nutzung auswählen
    sdrplay_api_SelectDevice(chosenDevice);
    printf("device selected\n");

    // Hole die Geräteparameter
    sdrplay_api_GetDeviceParams(chosenDevice->dev, &deviceParams);
    printf("device parameters read\n");

    // Setze die Tunerparameter
    printf("set tuner parameters\n");
    chParams = deviceParams->rxChannelA;
    deviceParams->rxChannelA->tunerParams.gain.gRdB = 20;        // Example: 40 dB gain reduction
    deviceParams->rxChannelA->tunerParams.gain.LNAstate = 1;     // Example: LNA state
    deviceParams->rxChannelA->tunerParams.gain.syncUpdate = 0;   // Default: no sync update needed
    deviceParams->rxChannelA->tunerParams.gain.minGr = sdrplay_api_NORMAL_MIN_GR; // Normal gain reduction
    chParams->tunerParams.rfFreq.rfHz = TUNED_FREQUENCY;
    chParams->tunerParams.bwType = sdrplay_api_BW_5_000; // für ADSB so breit
    deviceParams->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Zero; // output is in baseband
    deviceParams->devParams->fsFreq.fsHz = IN_RATE_HZ;  // sample rate 2.4 MS/s
    deviceParams->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_100HZ;
    deviceParams->rxChannelA->ctrlParams.agc.setPoint_dBfs = -30;

    // Callbacks einrichten
    sdrplay_api_CallbackFnsT cbFns{};
    cbFns.StreamACbFn = StreamACallback;
    cbFns.EventCbFn = EventCallback;

    // Gerät initialisieren und Stream starten
    if ((err = sdrplay_api_Init(chosenDevice->dev, &cbFns, NULL)) != sdrplay_api_Success) {
        printf("sdrplay_api_Init failed: %s\n", sdrplay_api_GetErrorString(err));
        return false;
    }

    streaming_ = true;
    return true;
}

void SDRHardware::stop() {
    if (streaming_) {
        sdrplay_api_Uninit(chosenDevice->dev);
        sdrplay_api_ReleaseDevice(chosenDevice);
        streaming_ = false;
    }
    if (apiOpen_) { sdrplay_api_Close(); apiOpen_ = false; }
}

// Sets the band and updates the frequency
// this function is called from the clientManager which is another process
// therefor we use an atomic flag to signal that the band value is ready to read
void SDRHardware::setBand(float b) {
    int numclients = ClientManager::getInstance().getNumberOfLoggedInClients();
    if(numclients != 1) return;
    band = b;
    bandReady = true;
}

void SDRHardware::convertToLiquidDSPFormat(short *xi, short *xq, unsigned int numSamples, liquid_float_complex* output) 
{
    int didx = 0;
    float div = 32768.0f;

    for(unsigned int i=0; i<numSamples; i++)
    {
        output[didx].real = (float)xi[i] / div;
        output[didx].imag = (float)xq[i] / div;
        didx++;
    }
}

static inline unsigned int estimate_out_len(unsigned int inN, float r) {
    // Liquid braucht etwas Headroom wegen Filter-Latenz; lieber etwas zu groß.
    // 64 ist konservativ, kostet kaum was.
    return static_cast<unsigned int>(std::ceil(inN * r)) + 64u;
}

void SDRHardware::StreamACallback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                                  unsigned int numSamples, unsigned int reset, void *cbContext)
{
    if (reset) {
        std::printf("StreamACallback: Reset detected, numSamples=%u\n", numSamples);
    }

    auto& hardware = SDRHardware::getInstance();
    if (params->rfChanged && hardware.capturePending_.load()) {
        hardware.confirmedSegment_ = hardware.pendingSegment_.load();
        hardware.capturePending_ = false;
    }
    const auto monoBefore = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    const auto utc = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    const auto monoAfter = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    const auto event = reset ? SDRStreamChange::HardwareReset :
        params->fsChanged ? SDRStreamChange::SampleRateChange :
        params->rfChanged ? SDRStreamChange::FrequencyChange : SDRStreamChange::None;
    const auto segment = hardware.capturePending_.load() ? 0 : hardware.confirmedSegment_;
    const auto first = hardware.captureClock_.observe(params->firstSampleNum, numSamples,
        utc, monoBefore + (monoAfter - monoBefore)/2, segment, event, monoAfter - monoBefore);
    // Split unusually large callbacks safely; never truncate or overflow rxarr.
    for (unsigned int offset = 0; offset < numSamples;) {
        const auto count = std::min<unsigned int>(rxarr.size(), numSamples - offset);
        hardware.convertToLiquidDSPFormat(xi + offset, xq + offset, count, rxarr.data());
        auto info = first;
        info.firstSample += offset;
        info.utcNanoseconds += sdrSampleTime(offset, 2400000);
        info.discontinuity = first.discontinuity && offset == 0;
        SDRQUEUE::pushData(rxarr.data(), count, info);
        offset += count;
    }
}

// Event callback function (static member function)
void SDRHardware::EventCallback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext) {
    if (eventId == sdrplay_api_GainChange) {
        /*
        // set LNAstate to keep this gRdB between 20 and 59)
        printf("Baseband Gain Reduction (gRdB): %d\n", params->gainParams.gRdB);
        printf("LNA Gain Reduction (lnaGRdB): %d\n", params->gainParams.lnaGRdB);
        printf("Current Total Gain: %.2f dB\n", params->gainParams.currGain);
        */
    }
}

float SDRHardware::getTuningFrequency() {
    return (float)TUNED_FREQUENCY;
}

void SDRHardware::changeBand() {
    if (capturePending_.load() || !bandReady.exchange(false)) return;
    const int selectedBand = static_cast<int>(band.load());
    const auto previousStart = StartQRG, previousEnd = EndQRG;
    const auto previousFrequency = TUNED_FREQUENCY;
    const bool previousADSB = isADSB;
    
    uint32_t offset = 240000; // Offset in Hz (uint32_t)
    uint32_t freq = 0;            // Calculated frequency in Hz (uint32_t)

    switch (selectedBand) {
        case 630:
            StartQRG = start_630m;
            EndQRG = end_630m;
            break;
        case 160:
            StartQRG = start_160m;
            EndQRG = end_160m;
            break;
        case 80:
            StartQRG = start_80m;
            EndQRG = end_80m;
            break;
        case 60:
            StartQRG = start_60m;
            EndQRG = end_60m;   // USB for 60m
            break;
        case 40:
            StartQRG = start_40m;
            EndQRG = end_40m;
            break;
        case 30:
            StartQRG = start_30m;
            EndQRG = end_30m;   // USB for 30m (primarily CW and digital modes)
            break;
        case 20:
            StartQRG = start_20m;
            EndQRG = end_20m;   // USB for 20m
            break;
        case 17:
            StartQRG = start_17m;
            EndQRG = end_17m;   // USB for 17m
            break;
        case 15:
            StartQRG = start_15m;
            EndQRG = end_15m;   // USB for 15m
            break;
        case 12:
            StartQRG = start_12m;
            EndQRG = end_12m;   // USB for 12m
            break;
        case 11:
            StartQRG = start_11m;
            EndQRG = end_11m;   // USB for 11m
            break;
        case 280:
            StartQRG = start_10m_a;
            EndQRG = end_10m_a;
            break;
        case 285:
            StartQRG = start_10m_b;
            EndQRG = end_10m_b;
            break;
        case 290:
            StartQRG = start_10m_c;
            EndQRG = end_10m_c;
            break;
        case 6:
            StartQRG = start_6m;
            EndQRG = end_6m;    // USB for 6m
            break;
        case 4:
            StartQRG = start_4m;
            EndQRG = end_4m;    // USB for 4m
            break;
        case 144:   // 144-144.5 MHz
            StartQRG = start_2m_a;
            EndQRG = end_2m_a;  // USB for 2m
            break;
        case 145:   // 144.5-145 MHz
            StartQRG = start_2m_b;
            EndQRG = end_2m_b;  // USB for 2m
            break;
        case 146:   // 145-145.5 MHz
            StartQRG = start_2m_c;
            EndQRG = end_2m_c;  // USB for 2m
            break;
        case 147:   // 145.5-146 MHz
            StartQRG = start_2m_d;
            EndQRG = end_2m_d;  // USB for 2m
            break;
        case 70:
            StartQRG = start_70cm;
            EndQRG = end_70cm;  // USB for 70cm
            break;
        case 438:   // 438.8-439.3 MHz 
            StartQRG = start_70cm_a;
            EndQRG = end_70cm_a; // USB for 70cm
            break;
        case 446:
            StartQRG = start_PMR446;
            EndQRG = end_PMR446; 
            break;
        case 1090:
            StartQRG = start_ADSB;
            EndQRG = end_ADSB; 
            break;
        default:
            std::cout << "Unknown band" << std::endl;
            return;
    }

    // make sure the bandwidth does not exceed 480kHz
    EndQRG = std::min(EndQRG, StartQRG + 480000);

    freq = StartQRG + offset;

    if(selectedBand == 1090) {
        freq = 1090000000;  // Festfrequenz 
        isADSB = true;
    }
    else {
        isADSB = false;
    }

    TUNED_FREQUENCY = freq;
    printf("set tuner to: %d\n",TUNED_FREQUENCY);

    deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = (double)TUNED_FREQUENCY;

    if (freq == previousFrequency && StartQRG == previousStart && EndQRG == previousEnd) return;
    pendingSegment_ = sdrSegment(StartQRG, EndQRG);
    capturePending_ = true; // Callback confirms the new RF setting via rfChanged.

    // Call sdrplay_api_Update to apply the new frequency
    sdrplay_api_ErrT ret = sdrplay_api_Update(chosenDevice->dev, sdrplay_api_Tuner_A, sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None);
    if (ret != sdrplay_api_Success) {
        printf("Error: sdrplay_api_Update failed with code %d: %s\n", ret, sdrplay_api_GetErrorString(ret));
        capturePending_ = false;
        StartQRG = previousStart; EndQRG = previousEnd;
        TUNED_FREQUENCY = previousFrequency; isADSB = previousADSB;
        deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = previousFrequency;
        return;
    }

    // starte/stoppe ADSB Decoder
    // --- ADSB / readsb lifecycle ---
    if (selectedBand == 1090) {
        if (!adsbRunning_) {
            adsbRunning_ = true;

            // FIFO + readsb starten
            ADSB& a = ADSB::getInstance();

            // FIFO Stream aktivieren (Gain wie bei dir bewährt, z.B. 10)
            a.startIQStreamFIFO("/tmp/adsb_iq.fifo", 10.0f);

            // readsb starten (Pfad ggf. anpassen)
            a.startReadsb("./readsb", "/tmp/adsb_iq.fifo", 30047, 0.2f);

            printf("ADSB: started readsb + FIFO stream\n");
        }
    } else {
        if (adsbRunning_) {
            adsbRunning_ = false;

            ADSB& a = ADSB::getInstance();
            a.stopReadsb();
            a.stopIQStreamFIFO();

            printf("ADSB: stopped readsb + FIFO stream\n");
        }
    }
}
