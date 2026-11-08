#include "WebSocketServer.h"
#include "Callsign.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

boost::lockfree::queue<ClientTXData, boost::lockfree::capacity<100>> websocketTXQueue;

// Singleton instance accessor
WebSocketServer& WebSocketServer::getInstance() {
    static WebSocketServer instance;
    return instance;
}

// Function to start the WebSocket server
void WebSocketServer::startServer(ClientEventSink clientEvents) {
    clientEvents_ = std::move(clientEvents);

    std::thread wsThread([this]() {

        uWS::App().ws<PerSocketData>("/*", { 
            .compression = uWS::SHARED_COMPRESSOR,
            .maxPayloadLength = 16 * 1024,
            .idleTimeout = 10,
            .maxBackpressure = 1 * 1024 * 1024,

            .upgrade = [](auto* res, auto* req, auto* context) {
                const std::string peerIP(res->getRemoteAddressAsText());
                std::string xForwardedFor = std::string(req->getHeader("x-forwarded-for"));
                std::string clientIP = xForwardedFor.empty() ?
                    peerIP : xForwardedFor;

                // Convert IPv6-mapped IPv4 to IPv4
                WebSocketServer& server = WebSocketServer::getInstance();
                clientIP = server.ipv6ToIpv4(clientIP);

                // Keep the transport peer separate from proxy/browser claims.
                // Never log query strings, cookies or authorization headers.
                using ClientDiagnostics::quote;
                std::ostringstream details;
                details << "peer=" << quote(server.ipv6ToIpv4(peerIP))
                        << " x-forwarded-for=" << quote(xForwardedFor)
                        << " x-real-ip=" << quote(req->getHeader("x-real-ip"))
                        << " forwarded=" << quote(req->getHeader("forwarded"))
                        << " host=" << quote(req->getHeader("host"))
                        << " path=" << quote(req->getUrl())
                        << " origin=" << quote(req->getHeader("origin"))
                        << " user-agent=" << quote(req->getHeader("user-agent"));
                PerSocketData socketData;
                socketData.clientIP = clientIP;
                socketData.handshakeDetails = details.str();
                res->template upgrade<PerSocketData>(std::move(socketData), req->getHeader("sec-websocket-key"),
                                                     req->getHeader("sec-websocket-protocol"),
                                                     req->getHeader("sec-websocket-extensions"), context);
            },

            .open = [this](auto* ws) {
                ws->getUserData()->clientId = nextClientId++;
                if (clients.size() >= max_users) {
                    ClientDiagnostics::log(ws->getUserData()->clientId,
                        "connection rejected reason=max-connections " + ws->getUserData()->handshakeDetails);
                    ws->close();
                }
                else {
                    clients.push_back(ws);
                    onClientConnect(ws);
                }
            },

            .message = [this](auto* ws, std::string_view message, uWS::OpCode opCode) {
                auto& activity = ws->getUserData()->activity;
                ++activity.messages;
                activity.lastMessageAt = ClientDiagnostics::Activity::Clock::now();
                if (opCode == uWS::OpCode::BINARY) {
                    ++activity.binary;
                    onClientMessage(ws, message);
                } else {
                    ++activity.text;
                    if (!activity.reportedText) {
                        activity.reportedText = true;
                        ClientDiagnostics::log(ws->getUserData()->clientId,
                            "text frame ignored bytes=" + std::to_string(message.size()));
                    }
                }
            },

            .close = [this](auto* ws, int code, std::string_view reason) {
                auto it = std::remove(clients.begin(), clients.end(), ws);
                if (it != clients.end()) {
                    // Client was found and moved to the end; now erase it
                    clients.erase(it, clients.end());
                    onClientDisconnect(ws, code, reason);
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
    auto& server  = WebSocketServer::getInstance();
    server.reportAnonymousClients();
    server.processFT8Queue();
    auto& clients = server.clients;
    ClientTXData item;

    //while (!websocketTXQueue.empty()) {
    //    websocketTXQueue.pop(item);
    while (websocketTXQueue.pop(item)) {
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

bool WebSocketServer::broadcastFT8(const FT8DecodedSlot& result) {
    auto frames = FT8Web::encode(result);
    if (frames.empty()) return true;
    // Keep slot bursts separate from the 100-packet audio/FFT queue. A full
    // decoder output is capped at 256 KiB; at most four slots wait here.
    std::lock_guard<std::mutex> lock(ft8Mutex_);
    if (ft8Queue_.size() >= 4) {
        std::fprintf(stderr, "[FT8] Web output busy; dropping decoded slot %lld\n",
            static_cast<long long>(result.utcSeconds));
        return false;
    }
    ft8Queue_.push_back(std::move(frames));
    return true;
}

void WebSocketServer::setFT8Frequency(std::uint32_t dialHz) { ft8DialHz_.store(dialHz); }

void WebSocketServer::broadcastFT8Frame(const FT8Web::Frame& frame) {
    const std::string_view bytes(reinterpret_cast<const char*>(frame.data()), sizeof(frame));
    std::vector<uWS::WebSocket<false, true, PerSocketData>*> failed;
    for (auto* ws : clients)
        if (ws->send(bytes, uWS::OpCode::BINARY) == uWS::WebSocket<false, true, PerSocketData>::DROPPED) failed.push_back(ws);
    // A slow connection must not silently show incomplete FT8 data. Closing it
    // lets the existing browser reconnect logic report/recover the connection.
    for (auto* ws : failed) {
        std::fprintf(stderr, "[FT8] Slow WebSocket client %d; reconnect required\n", ws->getUserData()->clientId);
        ws->end(1013, "FT8 output buffer full");
    }
}

void WebSocketServer::processFT8Queue() {
    const auto dial = ft8DialHz_.load();
    if (dial != sentFT8DialHz_) {
        sentFT8DialHz_ = dial;
        broadcastFT8Frame(FT8Web::status(dial));
    }
    std::deque<std::vector<FT8Web::Frame>> pending;
    {
        std::lock_guard<std::mutex> lock(ft8Mutex_);
        pending.swap(ft8Queue_);
    }
    for (const auto& slot : pending)
        for (const auto& frame : slot) broadcastFT8Frame(frame);
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
    const std::string& clientIP = ws->getUserData()->clientIP;
    ClientDiagnostics::log(ws->getUserData()->clientId,
        "connected state=awaiting-login sockets=" + std::to_string(clients.size()) + " " +
        ws->getUserData()->handshakeDetails);

    // Create ClientInfo for connection
    ClientInfo info;
    info.clientIP = clientIP;
    info.clientId = ws->getUserData()->clientId;  // Use the assigned client ID
    info.messageId = 0;                           // Connection event


    // Push into the queue using ClientManager's enqueue method
    if (!clientEvents_(info)) {
        ClientDiagnostics::log(info.clientId, "connection event queue full");
    }
    const auto status = FT8Web::status(ft8DialHz_.load());
    ws->send(std::string_view(reinterpret_cast<const char*>(status.data()), sizeof(status)), uWS::OpCode::BINARY);
}

// Internal method for when a client disconnects
void WebSocketServer::onClientDisconnect(uWS::WebSocket<false, true, PerSocketData>* ws, int code, std::string_view reason) {
    int clientId = ws->getUserData()->clientId;
    std::string clientIP = ws->getUserData()->clientIP;
    ClientDiagnostics::log(clientId,
        "disconnected code=" + std::to_string(code) + " reason=" + ClientDiagnostics::quote(reason) + " " +
        ws->getUserData()->activity.summary() + " sockets=" + std::to_string(clients.size()));

    // Create ClientInfo for disconnection
    ClientInfo info;
    info.clientIP = clientIP;
    info.clientId = clientId;  // Use the stored client ID
    info.messageId = 1;        // Disconnection event
    // Push into the queue using ClientManager's enqueue method
    if (!clientEvents_(info)) {
        ClientDiagnostics::log(clientId, "disconnection event queue full");
    }
    // Remove the client from the list
    // ist eh schon gelöscht 
    //clients.erase(std::remove(clients.begin(), clients.end(), ws), clients.end());
}

// Internal method for handling incoming messages
void WebSocketServer::onClientMessage(uWS::WebSocket<false, true, PerSocketData>* ws, std::string_view message) {
    int clientId = ws->getUserData()->clientId;
    auto& activity = ws->getUserData()->activity;

    size_t numBytes = message.size();
    if (numBytes < 2*sizeof(float) || numBytes % sizeof(float) != 0) {
        ++activity.malformed;
        if (!activity.reportedMalformed) {
            activity.reportedMalformed = true;
            ClientDiagnostics::log(clientId, "binary frame ignored reason=invalid-size bytes=" + std::to_string(numBytes));
        }
        return;
    }
    size_t numFloats = numBytes / sizeof(float);

    std::vector<float> data(numFloats);
    std::memcpy(data.data(), message.data(), numBytes);

    if (data[0] == 4) {
        const auto login = Callsign::inspectLoginMessage(data);
        const auto now = ClientDiagnostics::Activity::Clock::now();
        const bool previouslyValid = activity.validLoginData;
        const bool firstRejection = activity.loginAttempts == 0 || previouslyValid;
        ++activity.loginAttempts;
        activity.validLoginData = !login.callsign.empty();
        if (activity.validLoginData) {
            activity.loginInfo = "valid callsign=" + ClientDiagnostics::quote(login.callsign);
        } else {
            activity.loginInfo = login.reason + " username=" + ClientDiagnostics::quote(login.username, 64);
            if (previouslyValid) {
                activity.nextAnonymousReport = now + std::chrono::seconds(15);
                activity.reportedAnonymousCommand = false;
            }
            if (firstRejection || (login.reason != activity.lastLoggedRejection &&
                now - activity.lastRejectedLoginLog >= std::chrono::seconds(10))) {
                ClientDiagnostics::log(clientId, "login rejected reason=" + activity.loginInfo);
                activity.lastRejectedLoginLog = now;
                activity.lastLoggedRejection = login.reason;
            }
        }
    } else if (!activity.validLoginData && !activity.reportedAnonymousCommand) {
        activity.reportedAnonymousCommand = true;
        std::ostringstream detail;
        detail << "command received before valid login type=" << data[0] << " bytes=" << numBytes;
        ClientDiagnostics::log(clientId, detail.str());
    }

    // Create ClientInfo for message
    ClientInfo info;
    info.clientId = clientId;  // Use the stored client ID
    info.clientIP = ws->getUserData()->clientIP;
    info.messageId = 2;        // Message event
    info.message = data;       // Store the message data

    // Push into the queue using ClientManager's enqueue method
    if (!clientEvents_(info)) {
        ClientDiagnostics::log(clientId, "message event queue full");
    }
}

void WebSocketServer::reportAnonymousClients() {
    const auto now = ClientDiagnostics::Activity::Clock::now();
    if (now < nextDiagnosticsScan_) return;
    nextDiagnosticsScan_ = now + std::chrono::seconds(1);
    for (auto* ws : clients) {
        auto& activity = ws->getUserData()->activity;
        if (activity.anonymousReportDue(now))
            ClientDiagnostics::log(ws->getUserData()->clientId,
                "still awaiting valid login " + activity.summary(now));
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
