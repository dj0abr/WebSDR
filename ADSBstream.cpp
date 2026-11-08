#include "ADSBstream.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

// Deine WebSocket Schnittstelle
#include "WebSocketServer.h"

ADSBstream::ADSBstream(std::string host, int port)
    : host_(std::move(host)), port_(port)
{
    payload_buf_.reserve(1024 * 8); // bisschen Headroom
    th_ = std::thread(&ADSBstream::threadFn, this);
}

ADSBstream::~ADSBstream() {
    stop();
}

void ADSBstream::stop()
{
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;

    closeSocket();
    if (th_.joinable()) th_.join();
}

int ADSBstream::connectSocket()
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port_);

    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        errno = EINVAL;
        return -1;
    }

    if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

void ADSBstream::closeSocket()
{
    int fd = sockfd_;
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        sockfd_ = -1;
    }
    connected_.store(false, std::memory_order_relaxed);
}

void ADSBstream::appendPayloadBytes(const char* data, size_t len)
{
    // Rohbytes in den Buffer (ASCII JSON)
    payload_buf_.insert(payload_buf_.end(),
                        reinterpret_cast<const uint8_t*>(data),
                        reinterpret_cast<const uint8_t*>(data) + len);
}

void ADSBstream::trySendFullChunks()
{
    // Solange wir >= 1024 Nutzbytes haben, sende volle Frames
    while (payload_buf_.size() >= 1024) {
        // Frame bauen
        std::array<float, 1025> frame{};
        frame[0] = 6.0f; // Message Type ID

        // 1024 Bytes Nutzdaten
        for (size_t i = 0; i < 1024; i++) {
            frame[i + 1] = (float)payload_buf_[i];
        }

        // Senden
        WebSocketServer& wss = WebSocketServer::getInstance();
        wss.sendDataToClient(frame);

        // Verbrauchte Bytes entfernen
        payload_buf_.erase(payload_buf_.begin(), payload_buf_.begin() + 1024);
    }
}

void ADSBstream::printStats(uint64_t bytes_delta, uint64_t lines_delta)
{
   return;

    double lps = (double)lines_delta;
    double bps = (double)bytes_delta;
    double avg = (lines_delta > 0) ? (bps / lps) : 0.0;

    std::fprintf(stderr,
        "[ADSBstream] connected=%d  lines/s=%.0f  bytes/s=%.0f  avg_line=%.1f  total_lines=%llu  total_MB=%.2f  payload_buf=%zu\n",
        connected_.load(std::memory_order_relaxed) ? 1 : 0,
        lps, bps, avg,
        (unsigned long long)total_lines_.load(std::memory_order_relaxed),
        (double)total_bytes_.load(std::memory_order_relaxed) / (1024.0 * 1024.0),
        payload_buf_.size());
}

void ADSBstream::threadFn()
{
    using clock = std::chrono::steady_clock;

    std::string acc;
    acc.reserve(64 * 1024);

    auto last_tick = clock::now();
    uint64_t bytes_tick = 0;
    uint64_t lines_tick = 0;

    constexpr size_t BUF_SZ = 32 * 1024;
    char buf[BUF_SZ];

    while (running_.load(std::memory_order_relaxed)) {

        // connect/reconnect
        if (sockfd_ < 0) {
            int fd = connectSocket();
            if (fd < 0) {
                connected_.store(false, std::memory_order_relaxed);

                auto now = clock::now();
                if (now - last_tick >= std::chrono::seconds(1)) {
                    printStats(bytes_tick, lines_tick);
                    bytes_tick = 0;
                    lines_tick = 0;
                    last_tick = now;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            sockfd_ = fd;
            connected_.store(true, std::memory_order_relaxed);
        }

        ssize_t r = ::recv(sockfd_, buf, BUF_SZ, 0);
        if (r == 0) { closeSocket(); continue; }
        if (r < 0) {
            if (errno == EINTR) continue;
            closeSocket();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        bytes_tick += (uint64_t)r;
        total_bytes_.fetch_add((uint64_t)r, std::memory_order_relaxed);

        acc.append(buf, (size_t)r);

        // NDJSON Lines extrahieren
        for (;;) {
            size_t pos = acc.find('\n');
            if (pos == std::string::npos) break;

            // line ohne '\n'
            std::string line = acc.substr(0, pos);
            acc.erase(0, pos + 1);

            lines_tick++;
            total_lines_.fetch_add(1, std::memory_order_relaxed);

            // -> in Payload-Buffer, plus '\n' (hilft später beim Splitten im Browser)
            appendPayloadBytes(line.data(), line.size());
            const char nl = '\n';
            appendPayloadBytes(&nl, 1);

            // volle WS-Chunks raushauen
            trySendFullChunks();
        }

        auto now = clock::now();
        if (now - last_tick >= std::chrono::seconds(1)) {
            printStats(bytes_tick, lines_tick);
            bytes_tick = 0;
            lines_tick = 0;
            last_tick = now;
        }

        // Schutz gegen unendlich wachsenden acc (sollte nicht passieren)
        if (acc.size() > 2 * 1024 * 1024) acc.clear();

        // Schutz gegen unendlich wachsenden payload_buf_ (falls WS nicht sendet)
        // (eigentlich unwahrscheinlich, aber sicher ist sicher)
        if (payload_buf_.size() > 1024 * 1024 * 8) { // 8 MiB
            // Drop oldest
            payload_buf_.erase(payload_buf_.begin(), payload_buf_.begin() + 1024 * 1024);
        }
    }

    closeSocket();
}
