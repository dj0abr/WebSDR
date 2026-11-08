#include <signal.h>
#include "SDRHardware.h"
#include "FFTProcessor.h"
#include "WebSocketServer.h"
#include "ClientManager.h"
#include "ADSB.h"
#include "ADSB_FFT.h"
#include "ADSBstream.h"
#include "SDRQUEUE.h"
#include "global.h"

std::atomic<bool> keeprunning{true};
std::atomic<bool> isADSB{false};
uint32_t StartQRG = start_20m;
uint32_t EndQRG = end_20m;

// maximum nunber of allowed users
const long unsigned int max_users = 20;

static void requestStop(int) { keeprunning = false; }

int main() {
    signal(SIGINT, requestStop);
    signal(SIGTERM, requestStop);
    signal(SIGPIPE, SIG_IGN);
    // Reset the queue (optinal, würde auch mit Default gehen)
    SDRQUEUE::reset();
    // Create an object of SDRHardware
    SDRHardware& hardware = SDRHardware::getInstance();
    bool ret = hardware.init();
    if(!ret) {
        printf("cannot init SDR hardware\n");
        return 1;
    }

    ADSB::getInstance();
    ADSBstream s("127.0.0.1", 30047);
    FFTProcessor::getInstance().startFFTThread();
    ADSB_FFT::getInstance().startFFTThread();
    WebSocketServer::getInstance().startServer([](const ClientInfo& info) {
        return ClientManager::getInstance().enqueueClientInfo(info);
    });
    ClientManager::getInstance().startProcessing();

    // Endless loop
    while (keeprunning) {
        hardware.changeBand();
        usleep(100);
    }

    hardware.stop();
    ClientManager::getInstance().stopProcessing();
    s.stop();
    return 0;
}
