#ifndef CLIENTOBJECT_H
#define CLIENTOBJECT_H

#include <thread>
#include <atomic>
#include <iostream>
#include <boost/lockfree/spsc_queue.hpp>
#include "global.h"
#include "Tuner.h"
#include "SignalDecoder.h"
#include "NarrowFFT.h"

class ClientObject {
public:
    // Constructor that starts the client thread
    ClientObject(int clientId, const std::string& clientIP);
    
    // Destructor to clean up the thread
    ~ClientObject();

    // Function to stop the client's processing
    void stop();

    // Function to enqueue client info (used by ClientManager)
    bool enqueueInfoForCLient(const ClientInfo& clientInfo);

    // Get the client's IP address
    std::string getClientIP() const;

    // check the password
    bool checkPW();

    std::string username = "";
    std::string password = "";

private:
    // The thread that does the processing for the client
    std::thread clientThread;

    // Internal function that runs in the thread
    void processClient();

    void setFrequency(ClientInfo clientInfo);
    void setBand(ClientInfo clientInfo);
    void setMode(ClientInfo clientInfo);
    void setFilter(ClientInfo clientInfo);
    void decodeSamples(ClientInfo clientInfo);
    void userPW(ClientInfo clientInfo);

    // Flag to stop the thread
    int clientId;
    std::atomic<bool> keepRunning;
    std::string clientIP;

    // Queue for incoming messages (from ClientManager)
    boost::lockfree::spsc_queue<ClientInfo, boost::lockfree::capacity<100>> clientObjectInputQueue;

    // Tuner: shifts the wanted frequency into the baseband
    Tuner tuner;    

    // SignalDecoder: demodulates and returns audio
    SignalDecoder signaldecoder;

    // narrow band FFT processor
    NarrowFFTProcessor narrowFFT;
};

#endif // CLIENTOBJECT_H
