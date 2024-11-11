#include "WebSocketServer.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include "ClientManager.h"

boost::lockfree::queue<ClientTXData, boost::lockfree::capacity<100>> websocketTXQueue;
// Vector to store connected clients
std::vector<uWS::WebSocket<false, true, PerSocketData>*> clients;

// Singleton instance accessor
WebSocketServer& WebSocketServer::getInstance() {
    static WebSocketServer instance;
    return instance;
}

// Function to start the WebSocket server
void WebSocketServer::startServer() {

    std::thread wsThread([this]() {

        uWS::App().ws<PerSocketData>("/*", {
            .compression = uWS::SHARED_COMPRESSOR,
            .maxPayloadLength = 16 * 1024,
            .idleTimeout = 10,
            .maxBackpressure = 1 * 1024 * 1024,

            .upgrade = [](auto* res, auto* req, auto* context) {
                std::string xForwardedFor = std::string(req->getHeader("x-forwarded-for"));
                std::string clientIP = xForwardedFor.empty() ?
                    std::string(res->getRemoteAddressAsText()) : xForwardedFor;

                // Convert IPv6-mapped IPv4 to IPv4
                WebSocketServer& server = WebSocketServer::getInstance();
                clientIP = server.ipv6ToIpv4(clientIP);

                // Upgrade the connection and store the client's IP
                res->template upgrade<PerSocketData>({ clientIP }, req->getHeader("sec-websocket-key"),
                                                     req->getHeader("sec-websocket-protocol"),
                                                     req->getHeader("sec-websocket-extensions"), context);
            },

            .open = [this](auto* ws) {
                printf("clients.size(): %ld\n",clients.size());
                if (clients.size() >= max_users) {
                    std::cerr << "Maximum number of clients reached. Closing connection." << std::endl;
                    ws->close();
                }
                else {
                    clients.push_back(ws);
                    onClientConnect(ws);
                }
            },

            .message = [this](auto* ws, std::string_view message, uWS::OpCode opCode) {
                if (opCode == uWS::OpCode::BINARY) {
                    onClientMessage(ws, message);
                }
            },

            .close = [this](auto* ws, int /*code*/, std::string_view /*message*/) {
                auto it = std::remove(clients.begin(), clients.end(), ws);
                if (it != clients.end()) {
                    // Client was found and moved to the end; now erase it
                    clients.erase(it, clients.end());
                    onClientDisconnect(ws);
                } else {
                    // Client was not found; optionally log a message or handle this case
                    std::cerr << "Client to be removed was not found in the list." << std::endl;
                }
            }
        }).listen(9001, [this](auto* listen_socket) {
            if (listen_socket) {
            std::cout << "Thread " << std::this_thread::get_id() << " listening on port 9001" << std::endl;
            // Get the event loop associated with the current thread
            uWS::Loop *loop = uWS::Loop::get();

            // Cast the uWS loop to a µSockets loop
            us_loop_t *us_loop = (us_loop_t *)loop;

            // Create a µSockets timer
            us_timer_t *timer = us_create_timer(us_loop, 0, 0);

            // Set the timer to call the callback every 10ms
            us_timer_set(timer, [](us_timer_t *t) {
                processQueue();  // get external data and send it to a client
            }, 10, 10);
        } else {
            std::cerr << "Thread " << std::this_thread::get_id() << " failed to listen on port 9001" << std::endl;
        }
        }).run();
    });

    wsThread.detach();
}

// send messages to the browser clients, if available in the websocketTXQueue
void WebSocketServer::processQueue() {
    ClientTXData item;

    while (!websocketTXQueue.empty()) {
        websocketTXQueue.pop(item);
        int clientid = item.clientId;
        const std::array<float, 1025>& data = item.data;

        if (clientid == -1) {
            // send message to all clients
            for (auto *ws : clients) {
                std::string_view dataBytes(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(float));
                ws->send(dataBytes, uWS::OpCode::BINARY);
            }
        } else {
            // send message to clientid
            auto it = std::find_if(clients.begin(), clients.end(), [clientid](uWS::WebSocket<false, true, PerSocketData>* ws) {
                return ws->getUserData()->clientId == clientid;
            });

            if (it != clients.end()) {
                bool auth = item.authenticated;
                if(auth) {
                    // authentication ok
                    // send data
                    uWS::WebSocket<false, true, PerSocketData>* ws = *it;
                    std::string_view dataBytes(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(float));
                    ws->send(dataBytes, uWS::OpCode::BINARY);
                } else {
                    // authentication failed
                    std::array<float, 1025UL> msg = {5};    
                    uWS::WebSocket<false, true, PerSocketData>* ws = *it;
                    std::string_view dataBytes(reinterpret_cast<const char*>(msg.data()), msg.size() * sizeof(float));
                    ws->send(dataBytes, uWS::OpCode::BINARY);
                }
            } else {
                std::cerr << "Client with ID " << clientid << " not found." << std::endl;
            }
        }
    }
}

void WebSocketServer::sendDataToClient(ClientTXData &data) {
    if (!websocketTXQueue.push(data)) {
        std::cerr << "Queue is full, could not push data." << std::endl;
    }
}

void WebSocketServer::sendDataToClient(const std::array<float, 1025> &data, int clientid) {
    ClientTXData txdata;
    std::memcpy(txdata.data.data(), data.data(), 1025 * sizeof(float));
    txdata.clientId = clientid;
    if (!websocketTXQueue.push(txdata)) {
        std::cerr << "Queue is full, could not push data." << std::endl;
    }
}

// Internal method for when a client connects
void WebSocketServer::onClientConnect(uWS::WebSocket<false, true, PerSocketData>* ws) {

    // Assign a unique client ID
    ws->getUserData()->clientId = nextClientId++;
    
    const std::string& clientIP = ws->getUserData()->clientIP;
    std::cout << "Client connected: " << clientIP << " with client ID: " << ws->getUserData()->clientId << std::endl;

    // Create ClientInfo for connection
    ClientInfo info;
    info.clientIP = clientIP;
    info.clientId = ws->getUserData()->clientId;  // Use the assigned client ID
    info.messageId = 0;                           // Connection event


    // Push into the queue using ClientManager's enqueue method
    if (!ClientManager::getInstance().enqueueClientInfo(info)) {
        std::cerr << "Failed to enqueue client connection info!" << std::endl;
    }
}

// Internal method for when a client disconnects
void WebSocketServer::onClientDisconnect(uWS::WebSocket<false, true, PerSocketData>* ws) {
    int clientId = ws->getUserData()->clientId;
    std::string clientIP = ws->getUserData()->clientIP;
    std::cout << "Client disconnected: " << clientIP << " with client ID: " << clientId << std::endl;

    // Create ClientInfo for disconnection
    ClientInfo info;
    info.clientIP = clientIP;
    info.clientId = clientId;  // Use the stored client ID
    info.messageId = 1;        // Disconnection event
    // Push into the queue using ClientManager's enqueue method
    if (!ClientManager::getInstance().enqueueClientInfo(info)) {
        std::cerr << "Failed to enqueue client disconnection info!" << std::endl;
    }
    // Remove the client from the list
    clients.erase(std::remove(clients.begin(), clients.end(), ws), clients.end());
}

// Internal method for handling incoming messages
void WebSocketServer::onClientMessage(uWS::WebSocket<false, true, PerSocketData>* ws, std::string_view message) {
    int clientId = ws->getUserData()->clientId;

    size_t numBytes = message.size();
    size_t numFloats = numBytes / sizeof(float);

    std::vector<float> data(numFloats);
    std::memcpy(data.data(), message.data(), numBytes);

    // Create ClientInfo for message
    ClientInfo info;
    info.clientId = clientId;  // Use the stored client ID
    info.messageId = 2;        // Message event
    info.message = data;       // Store the message data

    // Push into the queue using ClientManager's enqueue method
    if (!ClientManager::getInstance().enqueueClientInfo(info)) {
        std::cerr << "Failed to enqueue client message info!" << std::endl;
    }
}

// Helper function to convert IPv6 to IPv4
std::string WebSocketServer::ipv6ToIpv4(const std::string& ipv6) {
    std::vector<std::string> parts;
    std::istringstream tokenStream(ipv6);
    std::string token;
    while (getline(tokenStream, token, ':')) {
        parts.push_back(token);
    }

    if (parts.size() == 8 && parts[5] == "ffff") {
        unsigned int part1, part2;
        std::stringstream ss1, ss2;
        ss1 << std::hex << parts[6];
        ss1 >> part1;
        ss2 << std::hex << parts[7];
        ss2 >> part2;

        std::stringstream ipv4;
        ipv4 << ((part1 >> 8) & 0xff) << "."
             << (part1 & 0xff) << "."
             << ((part2 >> 8) & 0xff) << "."
             << (part2 & 0xff);
        return ipv4.str();
    }

    return ipv6;
}
