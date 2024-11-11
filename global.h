#ifndef GLOBALS_H
#define GLOBALS_H

#include <vector>
#include <string>
#include "liquid.h"

// structure to hold raw samples
// already converted into liquid DSP format
typedef struct {
    std::vector<liquid_float_complex> sdata;
    int numSamples;
} SampleData;

struct ClientInfo {
    std::string clientIP;             // IP address of the client
    int clientId;                // Unique identifier for each client (not IP)
    int messageId;               // 0 = connect, 1 = disconnect, 2 = message, 3= raw data
    std::vector<float> message;  // Data vector (for message events)
    std::vector<liquid_float_complex> sdata;    // raw data (if messageID == 3)
};

struct ClientTXData {
    int clientId;
    bool authenticated = true;
    std::array<float, 1025> data;
};

// Start frequencies (in Hz) for each ham radio band
const uint32_t start_630m = 400000;    // 472 kHz
const uint32_t start_160m = 1800000;   // 1.8 MHz
const uint32_t start_80m = 3500000;    // 3.5 MHz
const uint32_t start_60m = 5300000;    
const uint32_t start_40m = 7000000;    // 7 MHz
const uint32_t start_30m = 10100000;   // 10.1 MHz
const uint32_t start_20m = 14000000;   // 14 MHz
const uint32_t start_17m = 18068000;   // 18.068 MHz
const uint32_t start_15m = 21000000;   // 21 MHz
const uint32_t start_12m = 24890000;   // 24.89 MHz
const uint32_t start_11m = 26965000;   // 27 MHz CB
const uint32_t start_10m_a = 28000000;
const uint32_t start_10m_b = 28450000;
const uint32_t start_10m_c = 29100000;
const uint32_t start_6m = 50000000;    // 50 MHz
const uint32_t start_4m = 70000000;    // 70 MHz
const uint32_t start_2m_a = 144000000;   // 144 MHz
const uint32_t start_2m_b = 144500000;   // 144 MHz
const uint32_t start_2m_c = 145000000;   // 144 MHz
const uint32_t start_2m_d = 145500000;   // 144 MHz
const uint32_t start_70cm = 433500000; // 430 MHz
const uint32_t start_70cm_a = 438800000;
const uint32_t start_PMR446 = 446000000;

const uint32_t end_630m = 500000;     // 479 kHz (breit genug machen, sonst Probleme mit der fft)
const uint32_t end_160m = 2000000;    // 2.0 MHz
const uint32_t end_80m = 3800000;     // 3.8 MHz
const uint32_t end_60m = 5400000;     
const uint32_t end_40m = 7200000;     // 7.2 MHz
const uint32_t end_30m = 10150000;    // 10.15 MHz
const uint32_t end_20m = 14350000;    // 14.35 MHz
const uint32_t end_17m = 18168000;    // 18.168 MHz
const uint32_t end_15m = 21450000;    // 21.45 MHz
const uint32_t end_12m = 24990000;    // 24.99 MHz
const uint32_t end_11m = 27405000;    // 27.405 MHz CB
const uint32_t end_10m_a = 28500000;
const uint32_t end_10m_b = 29000000;
const uint32_t end_10m_c = 29600000;
const uint32_t end_6m = 54000000;     // 54 MHz
const uint32_t end_4m = 70500000;    // 70 MHz
const uint32_t end_2m_a = 144500000;   // 144 MHz
const uint32_t end_2m_b = 145000000;   // 144 MHz
const uint32_t end_2m_c = 145500000;   // 144 MHz
const uint32_t end_2m_d = 146000000;   // 144 MHz
const uint32_t end_70cm = 434000000; // 430 MHz
const uint32_t end_70cm_a = 439300000;
const uint32_t end_PMR446 = 446200000;

// curently used frequencies
extern uint32_t StartQRG;
extern uint32_t EndQRG;
extern bool keeprunning;
extern const long unsigned int max_users;

#endif // GLOBALS_H
