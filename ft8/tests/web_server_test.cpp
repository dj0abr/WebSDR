// Run the real WebSocket transport without SDR hardware. Client commands are
// accepted by a test sink; broadcast handling and framing are production code.
#include "WebSocketServer.h"
#include <cstdlib>
#include <sstream>

const long unsigned int max_users = 20;

int main(int argc, char** argv) {
    auto& server=WebSocketServer::getInstance();
    std::uint32_t dial=5357000;
    server.setFT8Frequency(dial);
    auto batch=[&](unsigned count,std::uint32_t frequency) {
        FT8DecodedSlot result;
        result.utcSeconds=1700000010; result.dialHz=frequency;
        for (unsigned i=0;i<count;++i)
            result.lines.push_back("221330 -12 0.200 1500.000 ~ CQ K"+std::to_string(i)+"ABC FN42");
        return result;
    };
    if (argc>1 && std::string(argv[1])=="--queue-test") {
        for (int i=0;i<4;++i) if (!server.broadcastFT8(batch(1,dial))) return 1;
        return server.broadcastFT8(batch(1,dial))?1:0;
    }
    server.startServer([](const ClientInfo&) {return true;});
    std::string command;
    while (std::getline(std::cin,command)) {
        std::istringstream input(command);
        std::string action; input>>action;
        if (action=="emit") {
            unsigned count=0; std::uint32_t frequency=dial;
            input>>count;
            if (input>>frequency) {}
            if (!server.broadcastFT8(batch(count,frequency))) return 2;
        } else if (action=="unsafe") {
            auto result=batch(1,dial);
            result.lines[0]="221330 -12 0.200 1500.000 ~ <img src=x onerror=alert(1)>";
            server.broadcastFT8(result);
        } else if (action=="freq") {
            input>>dial; server.setFT8Frequency(dial);
        } else if (action=="legacy" || action=="target") {
            std::array<float,1025> data{};
            data[0]=action=="legacy"?0:3; data[1]=42.5;
            server.sendDataToClient(data,action=="legacy"?-1:1);
        } else if (action=="quit") {
            // This harness owns no SDR process. End the detached server thread
            // with the process, avoiding static teardown while its loop runs.
            std::_Exit(0);
        }
        std::cout<<"OK "<<action<<std::endl;
    }
    std::_Exit(0);
}
