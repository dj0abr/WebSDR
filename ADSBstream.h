#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

class ADSBstream {
public:
    ADSBstream(std::string host = "127.0.0.1", int port = 30047);
    ~ADSBstream();

    void stop();

private:
    void threadFn();
    int  connectSocket();
    void closeSocket();

    void appendPayloadBytes(const char* data, size_t len);
    void trySendFullChunks();

    void printStats(uint64_t bytes_delta, uint64_t lines_delta);

private:
    std::string host_;
    int port_;

    std::atomic<bool> running_{true};
    std::thread th_;

    int sockfd_ = -1;
    std::atomic<bool> connected_{false};

    // Stats
    std::atomic<uint64_t> total_bytes_{0};
    std::atomic<uint64_t> total_lines_{0};

    // Chunking buffer: sammelt rohe JSON-Bytes, bis wir volle 1024 Nutzbytes haben
    std::vector<uint8_t> payload_buf_;
};
