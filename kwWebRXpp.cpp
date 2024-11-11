#include "SDRHardware.h"
#include "FFTProcessor.h"
#include "WebSocketServer.h"
#include "ClientManager.h"
#include "global.h"

bool keeprunning = true;
uint32_t StartQRG = start_20m;
uint32_t EndQRG = end_20m;

// maximum nunber of allowed users
const long unsigned int max_users = 20;

int main() {
    // Create an object of SDRHardware
    SDRHardware& hardware = SDRHardware::getInstance();
    bool ret = hardware.init();
    if(!ret) {
        printf("cannot init SDR hardware\n");
        exit(0);
    }

    FFTProcessor::getInstance().startFFTThread();
    WebSocketServer::getInstance().startServer();
    ClientManager::getInstance().startProcessing();

    // Endless loop
    while (true) {
        hardware.changeBand();
        usleep(100);
    }

    return 0;
}