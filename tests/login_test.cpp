// Exercise production login/counting/command handling with only the RF hardware
// stubbed. No WebSocket listener, decoder process or SDR device is started.
#include "ClientManager.h"
#include "WebSocketServer.h"
#include "Callsign.h"
#include "ClientDiagnostics.h"
#include <cassert>
#include <limits>
#include <set>
#include <sstream>

std::uint32_t StartQRG = 14000000, EndQRG = 14350000;
std::atomic<bool> keeprunning{true}, isADSB{false};
const unsigned long max_users = 20;
extern boost::lockfree::queue<ClientTXData, boost::lockfree::capacity<100>> websocketTXQueue;
std::atomic<int> bandChanges{0};

SDRHardware::SDRHardware() : TUNED_FREQUENCY(14000000), SDR_SAMPLE_RATE(2400000) {}
SDRHardware::~SDRHardware() = default;
SDRHardware& SDRHardware::getInstance() { static SDRHardware hardware; return hardware; }
float SDRHardware::getTuningFrequency() { return TUNED_FREQUENCY; }
void SDRHardware::setBand(float) { ++bandChanges; }

template<class Predicate> void waitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::vector<float> loginPacket(const std::string& text) {
    std::vector<float> packet{4};
    for (unsigned char c : text) packet.push_back(c);
    return packet;
}

void event(int id, int kind, std::vector<float> message = {}) {
    ClientInfo info{};
    info.clientId = id; info.clientIP = "127.0.0.1"; info.messageId = kind;
    info.message = std::move(message);
    assert(ClientManager::getInstance().enqueueClientInfo(info));
}

void login(int id, const std::string& text) { event(id, 2, loginPacket(text)); }

// A changed frequency in the configuration packet acknowledges that the client
// worker processed all preceding commands, without relying on timed sleeps.
void barrier(int id) {
    static float token = 100;
    const float wanted = ++token;
    event(id, 2, {0, 240000 + wanted});
    waitFor([&] {
        ClientTXData data;
        while (websocketTXQueue.pop(data))
            if (data.clientId == id && data.data[0] == 2 && data.data[2] == wanted) return true;
        return false;
    });
}

int main() {
    assert(ClientDiagnostics::quote("ab\n\r\t\"\\\x1b") == "\"ab\\n\\r\\t\\\"\\\\\\x1b\"");
    assert(ClientDiagnostics::quote("abcdef", 3) == "\"abc...\"");
    ClientDiagnostics::Activity activity;
    const auto connected = activity.connectedAt;
    assert(!activity.anonymousReportDue(connected + std::chrono::seconds(14)));
    assert(activity.anonymousReportDue(connected + std::chrono::seconds(15)));
    assert(!activity.anonymousReportDue(connected + std::chrono::seconds(16)));
    assert(activity.anonymousReportDue(connected + std::chrono::seconds(315)));
    activity.validLoginData = true;
    assert(!activity.anonymousReportDue(connected + std::chrono::hours(1)));
    assert(Callsign::inspectLoginMessage(loginPacket(":")).reason == "empty-callsign");
    assert(Callsign::inspectLoginMessage(loginPacket("   :   ")).reason == "whitespace-only-callsign");
    assert(Callsign::inspectLoginMessage(loginPacket("DL1ABC")).reason == "missing-separator");
    const auto mismatched = Callsign::inspectLoginMessage(loginPacket("DL1ABC:SECRET-NOT-FOR-LOGS"));
    assert(mismatched.reason == "callsign-mismatch" && mismatched.username == "DL1ABC");
    assert(Callsign::normalize(" \tdj0abr/p\r\n") == "DJ0ABR/P");
    assert(Callsign::normalize("SWL") == "SWL");
    for (const std::string text : {"", "   ", "D1", "///", "1234", "DL 1ABC", "DL1ABC,DL2ABC"})
        assert(Callsign::normalize(text).empty());
    assert(Callsign::normalize(std::string(33, 'A')).empty());
    assert(Callsign::fromLoginMessage(loginPacket(" dj0abr/p : DJ0ABR/P ")) == "DJ0ABR/P");
    for (const std::string text : {":", "   :   ", "DJ0ABR", "DJ0ABR:other", "DJ0ABR:DJ0ABR:extra"})
        assert(Callsign::fromLoginMessage(loginPacket(text)).empty());
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.f, 256.f, 65.5f}) {
        auto packet = loginPacket("DJ0ABR:DJ0ABR"); packet[1] = invalid;
        assert(Callsign::fromLoginMessage(packet).empty());
    }

    auto& manager = ClientManager::getInstance();
    std::ostringstream journal;
    auto* oldOutput = std::cout.rdbuf(journal.rdbuf());
    manager.startProcessing();
    for (int id : {1, 2, 3}) event(id, 0);
    login(1, " dj0abr/p : dj0abr/p ");
    waitFor([&] { return manager.getNumberOfLoggedInClients() == 1; });
    barrier(1);
    login(1, "DJ0ABR/P:DJ0ABR/P"); // Periodic login heartbeat must not be logged again.
    barrier(1);
    // A connected, unnamed browser does not block the one logged-in operator.
    event(2, 2, {1, 40});
    event(1, 2, {1, 20});
    barrier(1);
    assert(bandChanges == 1);
    for (const std::string text : {":", "   :   ", "DJ0ABR", "///:///", "DL1ABC:wrong"}) login(2, text);
    barrier(1);
    assert(manager.getNumberOfLoggedInClients() == 1);

    login(2, "dl1abc:dl1abc");
    waitFor([&] { return manager.getNumberOfLoggedInClients() == 2; });
    event(1, 2, {1, 40}); event(2, 2, {1, 80});
    barrier(1); barrier(2);
    assert(bandChanges == 1); // Two valid logins lock band changes on the server.
    waitFor([&] {
        ClientTXData data;
        while (websocketTXQueue.pop(data)) {
            if (data.data[0] != 4) continue;
            std::string users;
            for (std::size_t i = 1; i < data.data.size() && data.data[i]; ++i)
                users += static_cast<char>(data.data[i]);
            std::istringstream input(users); std::set<std::string> names; std::string name;
            while (std::getline(input, name, ',')) names.insert(name);
            if (names == std::set<std::string>{"DJ0ABR/P", "DL1ABC"}) return true;
        }
        return false;
    });

    // NR controls traverse the real manager/client queues, independently per user.
    auto nrState = [&](int id, float command, unsigned wanted, unsigned effective) {
        event(id, 2, {5, command});
        waitFor([&] {
            ClientTXData data;
            while (websocketTXQueue.pop(data))
                if (data.clientId == id && data.data[0] == 8 && data.data[1] == 1 &&
                    data.data[2] == wanted && data.data[3] == effective) return true;
            return false;
        });
    };
    nrState(1, -1, 0, 0); nrState(2, -1, 0, 0);
    nrState(1, 75, 75, 75); nrState(2, -1, 0, 0);
    for (float invalid : {-2.f, 101.f, .5f, std::numeric_limits<float>::infinity(),
                          std::numeric_limits<float>::quiet_NaN()}) nrState(1, invalid, 75, 75);
    event(1, 2, {2, 2}); nrState(1, -1, 75, 0);
    event(1, 2, {2, 1}); nrState(1, -1, 75, 75);
    nrState(1, 0, 0, 0); nrState(2, 100, 100, 100);

    login(2, "   :   ");
    waitFor([&] { return manager.getNumberOfLoggedInClients() == 1; });
    event(2, 2, {1, 80}); event(1, 2, {1, 40});
    barrier(1);
    assert(bandChanges == 2); // Clearing a callsign releases the lock.
    login(1, "DJ0ABR"); // Missing delimiter must log out rather than throw.
    waitFor([&] { return manager.getNumberOfLoggedInClients() == 0; });
    login(3, "SWL:SWL");
    waitFor([&] { return manager.getNumberOfLoggedInClients() == 1; });
    event(3, 1);
    waitFor([&] { return manager.getNumberOfLoggedInClients() == 0; });

    keeprunning = false;
    manager.stopProcessing();
    std::cout.rdbuf(oldOutput);
    const auto output = journal.str();
    std::cout << output;
    const std::string accepted = "[Client #1] login accepted callsign=\"DJ0ABR/P\" logged_in=1";
    assert(output.find(accepted) != std::string::npos);
    assert(output.find(accepted, output.find(accepted) + 1) == std::string::npos);
    assert(output.find("[Client #2] login accepted callsign=\"DL1ABC\" logged_in=2") != std::string::npos);
    assert(output.find("reason=whitespace-only-callsign logged_in=1") != std::string::npos);
    assert(output.find("reason=missing-separator logged_in=0") != std::string::npos);
    assert(output.find("reason=connection-closed logged_in=0") != std::string::npos);
    assert(output.find("SECRET-NOT-FOR-LOGS") == std::string::npos);
    std::cout << "Callsign validation, anonymous clients, user list, server band lock, per-listener NR controls and logout passed.\n";
}
