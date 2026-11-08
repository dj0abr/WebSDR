#ifndef WEBSOCKETSERVER_H
#define WEBSOCKETSERVER_H

#include <App.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <deque>
#include <functional>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/lockfree/queue.hpp>
#include "global.h"
#include "FT8WebMessage.h"
#include "ClientDiagnostics.h"

// Per-socket data (can be used to store state for each connection)
struct PerSocketData {
    std::string clientIP;  // Store the client's IP address or other information
    int clientId = 0;      // Unique client identifier
    std::string handshakeDetails;
    ClientDiagnostics::Activity activity;
};

// Singleton WebSocket Server class
class WebSocketServer {
public:
    // Public method to access the singleton instance
    static WebSocketServer& getInstance();

    // Starts the WebSocket server
    using ClientEventSink = std::function<bool(const ClientInfo&)>;
    void startServer(ClientEventSink clientEvents);

    // Sends data to a specific client
    void sendDataToClient(ClientTXData &data);
    void sendDataToClient(const std::array<float, 1025> &data, int clientid = -1);
    // Called from decoder/input threads. Actual sends run on the WebSocket loop.
    bool broadcastFT8(const FT8DecodedSlot& result);
    void setFT8Frequency(std::uint32_t dialHz);

private:
    // Constructor is private to enforce singleton pattern
    WebSocketServer() : nextClientId(1) {}  // Initialize client ID counter to 1
    ~WebSocketServer() = default;

    // Deleted copy constructor and assignment operator to prevent copying
    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    // Internal method to handle client connections
    void onClientConnect(uWS::WebSocket<false, true, PerSocketData>* ws);

    // Internal method to handle client disconnections
    void onClientDisconnect(uWS::WebSocket<false, true, PerSocketData>* ws, int code, std::string_view reason);

    // Internal method to handle received messages from clients
    void onClientMessage(uWS::WebSocket<false, true, PerSocketData>* ws, std::string_view message);

    // Helper to convert IPv6 to IPv4 if applicable
    std::string ipv6ToIpv4(const std::string& ipv6);

    // read external data from the queue and send it to a specific or all clients
    static void processQueue();
    void processFT8Queue();
    void broadcastFT8Frame(const FT8Web::Frame& frame);
    void reportAnonymousClients();
    std::chrono::steady_clock::time_point nextDiagnosticsScan_{};

    // Client ID generator
    std::atomic<int> nextClientId{1};

    // Vector to store connected clients
    std::vector<uWS::WebSocket<false, true, PerSocketData>*> clients;
    ClientEventSink clientEvents_;
    std::mutex ft8Mutex_;
    std::deque<std::vector<FT8Web::Frame>> ft8Queue_;
    std::atomic<std::uint32_t> ft8DialHz_{0};
    std::uint32_t sentFT8DialHz_ = UINT32_MAX;
};

#endif // WEBSOCKETSERVER_H
