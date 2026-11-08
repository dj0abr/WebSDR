#include "ClientObject.h"
#include "liquid.h"
#include "global.h"
#include "WebSocketServer.h"
#include "ClientManager.h"
#include "SDRHardware.h"
#include <chrono>

using namespace std::chrono;

// Constructor: Starts the thread for processing
ClientObject::ClientObject(int clientId, const std::string& clientIP, const std::string& callsign)
    : username(callsign), clientId(clientId), keepRunning(true), clientIP(clientIP), clientObjectInputQueue() {
    clientThread = std::thread(&ClientObject::processClient, this);
}

// Destructor: Ensures the thread is stopped and joined
ClientObject::~ClientObject() {
    stop();
    if (clientThread.joinable()) {
        clientThread.join();
    }
}

// Function to stop the thread
void ClientObject::stop() {
    keepRunning = false;
}

// Function to enqueue client info (used by ClientManager)
bool ClientObject::enqueueInfoForCLient(const ClientInfo& clientInfo) {
    return clientObjectInputQueue.push(clientInfo);  // Push data into the queue
}

// Get the client's IP address
std::string ClientObject::getClientIP() const {
    return clientIP;
}

// Thread function to process client messages
void ClientObject::processClient() {
    float freq=0.0f,shift=0.0f,mode=0.0f,startQRG=0.0f,endQRG=0.0f,unum=-1;
    auto start_time = std::chrono::steady_clock::now();
    bool executed = false;

    while (keepRunning && keeprunning) {
        ClientInfo clientInfo;
        // Process any incoming messages in the queue
        if (clientObjectInputQueue.pop(clientInfo)) {
            // Message ID:
            // 0 ... waterfall index 0...1024 in the big waterfall
            // 1 ... band selection
            // 2 ... mode selection
            // 3 ... raw samples 480 kS/s
            switch (clientInfo.messageId) {
                case 2: { // message from Browser
                        if (clientInfo.message.size() < 2 || !std::isfinite(clientInfo.message[0]) ||
                            clientInfo.message[0] < 0 || clientInfo.message[0] > 5 ||
                            clientInfo.message[0] != std::round(clientInfo.message[0])) break;
                        int BrowserMessageID = static_cast<int>(clientInfo.message[0]);
                        // Message ID:
                        // 0 ... waterfall tuning frequency from the user
                        // 1 ... band selection
                        // 2 ... mode selection
                        // 3 ... ssb filter
                        // Login data is handled synchronously by ClientManager.
                        switch (BrowserMessageID) {
                            case 0: setFrequency(clientInfo);
                                    break;
                            case 1: setBand(clientInfo);
                                    break;
                            case 2: setMode(clientInfo);
                                    sendNoiseReductionState();
                                    break;
                            case 3: setFilter(clientInfo);
                                    break;
                            // Browser command 5: NR percent, -1 queries the state.
                            // Always acknowledge, including rejected values.
                            case 5: if (clientInfo.message[1] != -1)
                                        signaldecoder.setNoiseReduction(clientInfo.message[1]);
                                    sendNoiseReductionState();
                                    break;
                        }
                        break;
                }
                case 3: decodeSamples(clientInfo);
                        break;
            }
        } else {
            // send configuration data to the client browser
            SDRHardware& hardware = SDRHardware::getInstance();

            ClientTXData configdata{};
            configdata.clientId = clientId;
            configdata.data[0] = 2.0f;  // ID for configuration data
            configdata.data[1] = hardware.getTuningFrequency();
            configdata.data[2] = tuner.getFrequencyShift();
            configdata.data[3] = signaldecoder.getUsbLsb();
            configdata.data[4] = StartQRG;
            configdata.data[5] = EndQRG;
            configdata.data[6] = ClientManager::getInstance().getNumberOfLoggedInClients();

            bool hasChanged = false;
            if(freq != configdata.data[1]) hasChanged = true;
            if(shift != configdata.data[2]) hasChanged = true;
            if(mode != configdata.data[3]) hasChanged = true;
            if(startQRG != configdata.data[4]) hasChanged = true;
            if(endQRG != configdata.data[5]) hasChanged = true;
            if(unum != configdata.data[6]) hasChanged = true;

            // Check if 2 seconds have elapsed after start and delayedFunction() has not been executed yet
            auto current_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();

            if (elapsed >= 1 && !executed) {
                hasChanged = true;
                executed = true; // Ensure delayedFunction() is called only once
            }

            if (hasChanged) {
                WebSocketServer& WSSinstance = WebSocketServer::getInstance();
                WSSinstance.sendDataToClient(configdata);

                freq = configdata.data[1];
                shift = configdata.data[2];
                mode = configdata.data[3];
                startQRG = configdata.data[4];
                endQRG = configdata.data[5];
                unum = configdata.data[6];
            }

            // Sleep for a short duration to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

void ClientObject::setFrequency(ClientInfo clientInfo)
{
    float findex = clientInfo.message[1];
    tuner.setRXFrequencyOffset(findex);
}

void ClientObject::setBand(ClientInfo clientInfo)
{
    // Recheck when the queued command executes: another user may have logged in.
    if (ClientManager::getInstance().getNumberOfLoggedInClients() != 1) return;
    float fband = static_cast<int>(std::round(clientInfo.message[1]));
    SDRHardware& hardware = SDRHardware::getInstance();
    hardware.setBand(fband);
    signaldecoder.resetAudio();
}

void ClientObject::setMode(ClientInfo clientInfo)
{
    float fmode = clientInfo.message[1];
    signaldecoder.setMode(fmode);
}

void ClientObject::setFilter(ClientInfo clientInfo)
{
    float ffilter = clientInfo.message[1];
    signaldecoder.setFilter(ffilter);
}

void ClientObject::sendNoiseReductionState()
{
    ClientTXData state{};
    state.clientId = clientId;
    state.data[0] = 8; // NR state: version, configured percent, effective percent.
    state.data[1] = 1;
    state.data[2] = signaldecoder.getNoiseReduction();
    state.data[3] = signaldecoder.getEffectiveNoiseReduction();
    WebSocketServer::getInstance().sendDataToClient(state);
}

// process raw samples coming at a speed of 480 kS/s
void ClientObject::decodeSamples(ClientInfo clientInfo)
{
    // shift the wanted frequency into the baseband
    // and resample to 48 kS/s
    ClientInfo samples_baseband_48 = tuner.doTuning(clientInfo);

    // send the samples to the SignalDecoder for demodulation
    std::vector<float> audiosamples = signaldecoder.demodulate(samples_baseband_48);
    if (!audiosamples.empty()) {
        if (audiosamples.size() == 1025) {
            // the 1024 audio samples and the ID are in the float vector audiosamples.message
            ClientTXData txdata;
            std::copy(audiosamples.begin(), audiosamples.end(), txdata.data.begin());
            txdata.clientId = clientId;

            WebSocketServer& WSSinstance = WebSocketServer::getInstance();
            WSSinstance.sendDataToClient(txdata);
        }
        else {
            std::cerr << "Error: audiosamples has an incorrect size of " << audiosamples.size() << " elements.\n";
        }
    }

    // send the samples to the narrow band FFT
    samples_baseband_48.clientId = clientId;
    samples_baseband_48.messageId = 3;
    narrowFFT.pushSampleData(samples_baseband_48);
}
