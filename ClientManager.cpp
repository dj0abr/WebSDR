#include "ClientManager.h"
#include "WebSocketServer.h"
#include "SDRQUEUE.h"
#include "Callsign.h"
#include "ClientDiagnostics.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <cstring>

using namespace std::chrono;

// Singleton instance accessor
ClientManager& ClientManager::getInstance() {
    static ClientManager instance;
    return instance;
}

// Constructor
ClientManager::ClientManager()
    : ft8Decoder_({}, std::chrono::seconds(45), [](FT8DecodedSlot&& result) {
          WebSocketServer::getInstance().broadcastFT8(result);
      }),
      ft8Receiver_([this](FT8Slot&& slot) { ft8Decoder_.enqueue(std::move(slot)); },
                   FT8Receiver::configuredTimeOffset()),
      inputResampler_([this](const void* iq, std::size_t count, const SDRStreamInfo& info) {
          distributeSamples(iq, count, info);
      }) {}

// Destructor
ClientManager::~ClientManager() {
    stopProcessing();
}


// Start processing messages
void ClientManager::startProcessing() {
    if (processing_.exchange(true)) return;
    processingThread_ = std::thread(&ClientManager::processMessages, this);
}

void ClientManager::stopProcessing() {
    processing_ = false;
    if (processingThread_.joinable()) processingThread_.join();
    ft8Decoder_.stop();
}

// get number of active clients
int ClientManager::getNumberOfLoggedInClients() {
    return clientCount_.load();
}

// Function for WebSocketServer to push client info into the queue
// used by the WebSocket to send User data to the ClientObject (e.g., tuning, mode...)
bool ClientManager::enqueueClientInfo(const ClientInfo& clientInfo) {
    return clientQueue.push(clientInfo);  // Push data to the lock-free queue
}

// Function to enqueue raw sample data
// use to send 480kS/s raw samples to the ClientObject for demodulation and smallFFT
bool ClientManager::enqueueRawSamples(const SampleData& sampleData) {
    return rawSamplesQueue.push(sampleData);  // Push raw sample data to the queue
}

// Function to enqueue FFT data into the bigFFTqueue
// the FFTProcessor Object uses this function to send its data to all clients via the bigFFTqueue
bool ClientManager::enqueueFFTData(const std::array<float, 1025>& fftData) {
    return bigFFTqueue.push(fftData);  // Push FFT data to the queue
}

// Internal method to process messages from the SPSC queue
void ClientManager::processMessages() {
    while (processing_ && keeprunning) {
        ClientInfo clientInfo;
        // Check if there's an event in the queue
        if (clientQueue.pop(clientInfo)) {
            // Handle the message based on the messageId
            switch (clientInfo.messageId) {
                case 0: {  // Client connection
                    // An open WebSocket is not a login. Allocate receiver state
                    // and count this client only after valid type-4 login data.
                    break;
                }
                case 1: {  // Client disconnection
                    // Stop and remove the ClientObject
                    auto it = clientMap.find(clientInfo.clientId);
                    if (it != clientMap.end()) {
                        const auto callsign = it->second->username;
                        it->second->stop();  // Ensure thread is stopped
                        clientMap.erase(it);  // Erase the object from the map
                        clientCount_ = clientMap.size();
                        ClientDiagnostics::log(clientInfo.clientId,
                            "logged out callsign=" + ClientDiagnostics::quote(callsign) +
                            " reason=connection-closed logged_in=" + std::to_string(clientMap.size()));
                    }
                    break;
                }
                case 2: {  // Send message to the specific client object
                    //std::cout << "Processing message from client " << clientInfo.clientId << " IP:" << clientInfo.clientIP << std::endl;

                    if (clientInfo.message.size() < 2) break;
                    auto it = clientMap.find(clientInfo.clientId);
                    if (clientInfo.message[0] == 4) {
                        const auto login = Callsign::inspectLoginMessage(clientInfo.message);
                        const auto& callsign = login.callsign;
                        if (callsign.empty()) {
                            if (it != clientMap.end()) {
                                const auto previous = it->second->username;
                                it->second->stop();
                                clientMap.erase(it);
                                clientCount_ = clientMap.size();
                                ClientDiagnostics::log(clientInfo.clientId,
                                    "logged out callsign=" + ClientDiagnostics::quote(previous) +
                                    " reason=" + login.reason + " logged_in=" + std::to_string(clientMap.size()));
                            }
                        } else if (it != clientMap.end()) {
                            if (it->second->username != callsign)
                                ClientDiagnostics::log(clientInfo.clientId,
                                    "callsign changed from=" + ClientDiagnostics::quote(it->second->username) +
                                    " to=" + ClientDiagnostics::quote(callsign));
                            it->second->username = callsign;
                        } else if (clientMap.size() < max_users) {
                            clientMap[clientInfo.clientId] = std::make_unique<ClientObject>(
                                clientInfo.clientId, clientInfo.clientIP, callsign);
                            clientCount_ = clientMap.size();
                            ClientDiagnostics::log(clientInfo.clientId,
                                "login accepted callsign=" + ClientDiagnostics::quote(callsign) +
                                " logged_in=" + std::to_string(clientMap.size()));
                        }
                        break;
                    }
                    // Unregistered clients cannot tune; band changes require
                    // exactly one logged-in client, regardless of browser UI.
                    if (clientInfo.message[0] == 1 && clientMap.size() != 1) break;
                    if (it != clientMap.end()) {
                        // Push the message into the client's queue
                        if (!it->second->enqueueInfoForCLient(clientInfo)) {
                            std::cerr << "Failed to enqueue message for client " << clientInfo.clientId << std::endl;
                        }
                    }
                    break;
                }
                default:
                    std::cerr << "Unknown message ID: " << clientInfo.messageId << std::endl;
                    break;
            }
        } 

        // Send FFT data (bigFFTqueue) to all Web-clients via the WebSocket
        std::array<float, 1025> fftData;
        if (bigFFTqueue.pop(fftData)) {
            WebSocketServer& WSSinstance = WebSocketServer::getInstance();
            WSSinstance.sendDataToClient(fftData,-1);
        }

        // send raw sample data (480kS/s) (messageId == 3) to all clientObjects
        SDRQUEUE::View v;
        if (SDRQUEUE::tryPop(2, v)) {
            inputResampler_.process(v.data, v.count, v.info);
            SDRQUEUE::consume(2);
        }

        // check user and password
        checkUserPW();

        // Sleep for a short duration to avoid busy-waiting
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

// The input resampler calls this for both received IQ and short zero-filled gaps.
void ClientManager::distributeSamples(const void* iq, std::size_t count, const SDRStreamInfo& info) {
    const auto dial = FT8Receiver::dialFrequency(info);
    if (dial != lastFT8DialHz_) {
        lastFT8DialHz_ = dial;
        WebSocketServer::getInstance().setFT8Frequency(dial);
    }
    // Runs once per band, including when no browser clients are connected.
    ft8Receiver_.process(iq, count, info);
    if (clientMap.empty()) return;
    ClientInfo rawClientInfo;
    rawClientInfo.messageId = 3;
    rawClientInfo.sdata.resize(count);
    std::memcpy(rawClientInfo.sdata.data(), iq, count*sizeof(liquid_float_complex));
    for (auto& clientPair : clientMap) {
        ClientObject* clientObject = clientPair.second.get();
        rawClientInfo.clientId = clientPair.first;
        rawClientInfo.clientIP = clientObject->getClientIP();
        if (!clientObject->enqueueInfoForCLient(rawClientInfo))
            std::cerr << "Failed to enqueue raw data for client " << rawClientInfo.clientId << std::endl;
    }
}

void ClientManager::checkUserPW()
{
    static auto lastTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime).count();

    // build a list of currently connected clients and send it to all clients        
    if (elapsed >= 1) {
        std::string users;
        for (auto it = clientMap.begin(); it != clientMap.end(); ++it) {
            const auto& clientObject = it->second;
            if (clientObject) {
                users += clientObject->username;
                if (std::next(it) != clientMap.end()) users += ",";
            }
        }


        // convert "users" to a float array
        std::array<float, 1025> data = {4}; // 4 indicating "users list"
        // Copy the string to the array starting at index 1
        size_t length = std::min(users.size(), data.size() - 2); // Leave space for the 0 terminator
        for (size_t i = 0; i < length; ++i) {
            data[i + 1] = static_cast<float>(users[i]);
        }
        // Add a 0 terminator after the string
        data[length + 1] = 0.0f;

        // send to all clients
        WebSocketServer& WSSinstance = WebSocketServer::getInstance();
        WSSinstance.sendDataToClient(data);

        lastTime = now;
        //std::cout << "all users: " << users << std::endl;
    }
}
