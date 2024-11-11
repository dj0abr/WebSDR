#ifndef WEBSOCKETSERVER_H
#define WEBSOCKETSERVER_H

#include <App.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/lockfree/queue.hpp>
#include "global.h"

// Per-socket data (can be used to store state for each connection)
struct PerSocketData {
    std::string clientIP;  // Store the client's IP address or other information
    int clientId;          // Unique client identifier
};

// Singleton WebSocket Server class
class WebSocketServer {
public:
    // Public method to access the singleton instance
    static WebSocketServer& getInstance();

    // Starts the WebSocket server
    void startServer();

    // Sends data to a specific client
    void sendDataToClient(ClientTXData &data);
    void sendDataToClient(const std::array<float, 1025> &data, int clientid = -1);

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
    void onClientDisconnect(uWS::WebSocket<false, true, PerSocketData>* ws);

    // Internal method to handle received messages from clients
    void onClientMessage(uWS::WebSocket<false, true, PerSocketData>* ws, std::string_view message);

    // Helper to convert IPv6 to IPv4 if applicable
    std::string ipv6ToIpv4(const std::string& ipv6);

    // read external data from the queue and send it to a specific or all clients
    static void processQueue();

    // Client ID generator
    int nextClientId;
};

#endif // WEBSOCKETSERVER_H
