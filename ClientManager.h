#ifndef CLIENTMANAGER_H
#define CLIENTMANAGER_H

#include <boost/lockfree/spsc_queue.hpp>
#include <unordered_map>
#include <thread>
#include <atomic>
#include "global.h"
#include "ClientObject.h"
#include "SDRHardware.h"
#include "FT8Receiver.h"
#include "SDRInputResampler.h"

class ClientManager {
public:
    // Singleton instance of the ClientManager
    static ClientManager& getInstance();

    // Starts the ClientManager's processing loop
    void startProcessing();

    // Stops the processing loop (optional, not necessary for detached thread)
    void stopProcessing();

    // Function to allow WebSocketServer to push data into the queue
    bool enqueueClientInfo(const ClientInfo& clientInfo);

    // Function to push big FFT data into the queue
    bool enqueueFFTData(const std::array<float, 1025>& fftData);

    // get number of active clients
    int getNumberOfLoggedInClients();

private:
    ClientManager();
    ~ClientManager();

    // Deleted copy constructor and assignment operator to prevent copying
    ClientManager(const ClientManager&) = delete;
    ClientManager& operator=(const ClientManager&) = delete;

    FT8DecoderProcess ft8Decoder_;
    FT8Receiver ft8Receiver_;
    SDRInputResampler inputResampler_;
    std::thread processingThread_;
    std::atomic<bool> processing_{false};
    std::atomic<int> clientCount_{0};
    std::uint32_t lastFT8DialHz_ = UINT32_MAX;

    // Internal method to process messages
    void processMessages();
    void distributeSamples(const void* iq, std::size_t count, const SDRStreamInfo& info);

    // handles user and password of the clients
    void checkUserPW();

    // Function to push SampleData into rawSamplesQueue
    bool enqueueRawSamples(const SampleData& sampleData);
    
    // The SPSC queue for client events
    boost::lockfree::spsc_queue<ClientInfo, boost::lockfree::capacity<100>> clientQueue;

    // Queue for FFT bins (full scale FFT)
    boost::lockfree::spsc_queue<std::array<float, 1025>, boost::lockfree::capacity<100>> bigFFTqueue;

    // Queue for FFT bins (narrowband FFT)
    boost::lockfree::spsc_queue<std::array<float, 1025>, boost::lockfree::capacity<100>> smallFFTqueue;

    // Queue for audio samples from the SignalDecoder
    // queue for about 2 seconds (8kHz sample rate and 1024 sample packets)
    boost::lockfree::spsc_queue<std::array<float, 1025>, boost::lockfree::capacity<20>> audioQueue;

    // Map to store the active ClientObjects by clientId
    std::unordered_map<int, std::unique_ptr<ClientObject>> clientMap;

    // Queue for raw sample data (480kS/s)
    boost::lockfree::spsc_queue<SampleData, boost::lockfree::capacity<1024>> rawSamplesQueue;

};

#endif // CLIENTMANAGER_H
