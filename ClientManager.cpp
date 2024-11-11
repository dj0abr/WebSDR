#include "ClientManager.h"
#include "WebSocketServer.h"
#include <iostream>
#include <chrono>
#include <thread>

using namespace std::chrono;

// Singleton instance accessor
ClientManager& ClientManager::getInstance() {
    static ClientManager instance;
    return instance;
}

// Start processing messages
void ClientManager::startProcessing() {
    std::thread([this]() {
        processMessages();
    }).detach();  // Detach the thread to run independently
}

// get number of active clients
int ClientManager::getNumberOfLoggedInClients() {
    return clientMap.size();
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
    while (true) {
        ClientInfo clientInfo;
        // Check if there's an event in the queue
        if (clientQueue.pop(clientInfo)) {
            // Handle the message based on the messageId
            switch (clientInfo.messageId) {
                case 0: {  // Client connection
                    std::cout << "Client connected: " << clientInfo.clientId 
                              << " IP:" << clientInfo.clientIP << std::endl;

                    if(clientMap.size() < max_users) {
                        // Create and start a new ClientObject
                        clientMap[clientInfo.clientId] = std::make_unique<ClientObject>(clientInfo.clientId, clientInfo.clientIP);
                        //printf("Inserted Client %d into the ClientMap\n",clientInfo.clientId);
                    }
                    break;
                }
                case 1: {  // Client disconnection
                    std::cout << "Client disconnected: " << clientInfo.clientId 
                              << " IP:" << clientInfo.clientIP << std::endl;

                    // Stop and remove the ClientObject
                    auto it = clientMap.find(clientInfo.clientId);
                    if (it != clientMap.end()) {
                        it->second->stop();  // Ensure thread is stopped
                        clientMap.erase(it);  // Erase the object from the map
                    }
                    else {
                        std::cout << "Client ignored, max_user reached" << std::endl;
                    }
                    break;
                }
                case 2: {  // Send message to the specific client object
                    //std::cout << "Processing message from client " << clientInfo.clientId << " IP:" << clientInfo.clientIP << std::endl;

                    // Find the appropriate ClientObject and send the message
                    auto it = clientMap.find(clientInfo.clientId);
                    if (it != clientMap.end()) {
                        // Push the message into the client's queue
                        if (!it->second->enqueueInfoForCLient(clientInfo)) {
                            std::cerr << "Failed to enqueue message for client " << clientInfo.clientId << std::endl;
                        }
                    } else {
                        std::cerr << "No ClientObject found for client " << clientInfo.clientId << std::endl;
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

        // send raw sample data (messageId == 3) to all clientObjects
        SampleData sampleData;
        if (rawSamplesQueue.pop(sampleData)) {
            // Create ClientInfo for raw data
            ClientInfo rawClientInfo;
            rawClientInfo.messageId = 3;
            rawClientInfo.sdata = sampleData.sdata;  // Copy raw data

            // Send rawClientInfo to all active clientObjects
            for (auto& clientPair : clientMap) {
                ClientObject* clientObject = clientPair.second.get();
                rawClientInfo.clientId = clientPair.first;
                rawClientInfo.clientIP = clientObject->getClientIP();

                if (!clientObject->enqueueInfoForCLient(rawClientInfo)) {
                    std::cerr << "Failed to enqueue raw data for client " << rawClientInfo.clientId << std::endl;
                }
            }
        }

        // check user and password
        checkUserPW();

        // Sleep for a short duration to avoid busy-waiting
        std::this_thread::sleep_for(std::chrono::microseconds(100));
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
