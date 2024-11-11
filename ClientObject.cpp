#include "ClientObject.h"
#include "liquid.h"
#include "global.h"
#include "WebSocketServer.h"
#include "ClientManager.h"
#include "SDRHardware.h"
#include <chrono>

using namespace std::chrono;

// Constructor: Starts the thread for processing
ClientObject::ClientObject(int clientId, const std::string& clientIP)
    : clientId(clientId), keepRunning(true), clientIP(clientIP), clientObjectInputQueue() {
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
                        int BrowserMessageID = static_cast<int>(std::round(clientInfo.message[0]));
                        // Message ID:
                        // 0 ... waterfall tuning frequency from the user
                        // 1 ... band selection
                        // 2 ... mode selection
                        // 3 ... ssb filter
                        // 4 ... user login data
                        switch (BrowserMessageID) {
                            case 0: setFrequency(clientInfo);
                                    break;
                            case 1: setBand(clientInfo);
                                    break;
                            case 2: setMode(clientInfo);
                                    break;
                            case 3: setFilter(clientInfo);
                                    break;
                            case 4: userPW(clientInfo);
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

            ClientTXData configdata;
            configdata.clientId = clientId;
            configdata.data[0] = 2.0f;  // ID for configuration data
            configdata.data[1] = hardware.getTuningFrequency();
            configdata.data[2] = tuner.getFrequencyShift();
            configdata.data[3] = signaldecoder.getUsbLsb();
            configdata.data[4] = StartQRG;
            configdata.data[5] = EndQRG;
            configdata.data[6] = ClientManager::getInstance().getNumberOfLoggedInClients();
            configdata.authenticated = checkPW();

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
    float fband = static_cast<int>(std::round(clientInfo.message[1]));
    SDRHardware& hardware = SDRHardware::getInstance();
    hardware.setBand(fband);
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

void ClientObject::userPW(ClientInfo clientInfo) {
    // Convert the float vector to a string
    std::string combinedText;

    // Start the loop from index 1, ignoring index 0
    for (size_t i = 1; i < clientInfo.message.size(); ++i) {
        // Convert each float to a char (byte) by rounding and casting
        char byte = static_cast<char>(std::round(clientInfo.message[i]));
        combinedText += byte;
    }

    // Split the combinedText string into username and password
    size_t separatorPos = combinedText.find(':'); // Assuming ':' was used as a delimiter
    if (separatorPos == std::string::npos) {
        throw std::runtime_error("Delimiter not found in received data");
    }

    // Extract username and password
    username = combinedText.substr(0, separatorPos);
    password = combinedText.substr(separatorPos + 1);
}

bool ClientObject::checkPW() {
    bool auth = true;
    if(password != username) auth = false;
    if(username.length() < 3) auth = false;
    //printf("%d %s %s\n",auth?1:0,username.c_str(),password.c_str())    ;
    return auth;
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
            txdata.authenticated = checkPW();

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
    if(!checkPW()) {
        // not authenticated, clear data
        samples_baseband_48.sdata.clear();
    }
    narrowFFT.pushSampleData(samples_baseband_48);
}
