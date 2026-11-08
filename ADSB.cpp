#include <sys/wait.h>
#include <signal.h>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <unordered_map>
#include <cstdint>
#include <array>
#include <numeric>
#include <cstring>
#include <iostream>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <vector>
#include <boost/lockfree/spsc_queue.hpp>
#include "liquid.h"
#include "ADSB.h"
#include "SDRQUEUE.h"

ADSB& ADSB::getInstance() {
    static ADSB inst;
    return inst;
}

ADSB::ADSB() {
    ADBSworkerThread = std::thread(&ADSB::worker, this);
}

ADSB::~ADSB() {
    stopIQStreamFIFO();

    if (ADBSworkerThread.joinable()) {
        ADBSworkerThread.join();
    }
}

void ADSB::worker() {
    SDRQUEUE::View v;
    while (keeprunning) {
        if (SDRQUEUE::tryPop(0, v)) {
            handleADSBsamples(v.data, v.count);
            SDRQUEUE::consume(0);
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}

static inline uint8_t f32_to_u8_offset(float v)
{
    if (v > 1.0f)  v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    float u = (v * 127.0f) + 127.5f;
    if (u < 0.0f)   u = 0.0f;
    if (u > 255.0f) u = 255.0f;
    return (uint8_t)lrintf(u);
}

void ADSB::handleADSBsamples(const liquid_float_complex* samples, std::uint32_t count)
{
    if (fifo_on_.load(std::memory_order_relaxed)) {
        float gain = iq_gain_;

        // push byteweise: I,Q,I,Q...
        // wenn Queue voll: droppen (aber NICHT blockieren!)
        for (uint32_t i = 0; i < count; i++) {
            uint8_t bI = f32_to_u8_offset(samples[i].real * gain);
            uint8_t bQ = f32_to_u8_offset(samples[i].imag * gain);

            uint16_t iq = (uint16_t)bI | ((uint16_t)bQ << 8); // low=I, high=Q

            if (fifo_q16_.push(iq)) {
                fifo_bytes_enqueued_.fetch_add(2, std::memory_order_relaxed);
            } else {
                fifo_bytes_dropped_.fetch_add(2, std::memory_order_relaxed);
            }
        }
    }
}

bool ADSB::startIQStreamFIFO(const char* fifo_path, float gain)
{
    fifo_path_ = fifo_path;
    iq_gain_ = gain;

    // FIFO anlegen (falls nicht vorhanden)
    if (mkfifo(fifo_path_.c_str(), 0666) < 0 && errno != EEXIST) {
        std::perror("mkfifo");
        return false;
    }

    // Dummy-Reader offen halten -> verhindert EOF/komisches Shutdown-Verhalten auf readsb-Seite
    if (fifo_rfd_ < 0) {
        fifo_rfd_ = open(fifo_path_.c_str(), O_RDONLY | O_NONBLOCK);
        if (fifo_rfd_ < 0) {
            std::perror("open fifo dummy read");
            // nicht fatal, aber weniger stabil
        }
    }

    // Queue leeren (optional)
    uint16_t tmp;
    while (fifo_q16_.pop(tmp)) {}

    fifo_bytes_enqueued_ = 0;
    fifo_bytes_written_  = 0;
    fifo_bytes_dropped_  = 0;

    fifo_on_.store(true, std::memory_order_release);

    // Writer-Thread starten (falls nicht läuft)
    if (!fifo_writer_thread_.joinable()) {
        fifo_writer_thread_ = std::thread(&ADSB::fifoWriterThreadFn, this);
    }

    std::fprintf(stderr, "IQ FIFO stream ON: %s gain=%.2f\n", fifo_path_.c_str(), iq_gain_);
    return true;
}

void ADSB::stopIQStreamFIFO()
{
    fifo_on_.store(false, std::memory_order_release);

    close_fifo_writer();

    if (fifo_writer_thread_.joinable()) {
        fifo_writer_thread_.join();
    }

    if (fifo_rfd_ >= 0) {
        close(fifo_rfd_);
        fifo_rfd_ = -1;
    }

    std::fprintf(stderr,
        "IQ FIFO stream OFF. enq=%lluB written=%lluB dropped=%lluB\n",
        (unsigned long long)fifo_bytes_enqueued_.load(),
        (unsigned long long)fifo_bytes_written_.load(),
        (unsigned long long)fifo_bytes_dropped_.load());
}

void ADSB::close_fifo_writer()
{
    if (fifo_wfd_ >= 0) {
        close(fifo_wfd_);
        fifo_wfd_ = -1;
    }
}

bool ADSB::ensure_fifo_writer_open()
{
    if (fifo_wfd_ >= 0) return true;

    // Nonblocking open: wenn readsb nicht läuft -> ENXIO
    int fd = open(fifo_path_.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENXIO) return false; // kein Reader
        std::perror("open fifo write");
        return false;
    }

    fifo_wfd_ = fd;
    std::fprintf(stderr, "FIFO writer connected\n");
    return true;
}

void ADSB::fifoWriterThreadFn()
{
    // kleiner Output-Buffer, damit write() effizient ist
    constexpr size_t OUT_CHUNK = 32 * 1024;
    uint8_t out[OUT_CHUNK];

    while (fifo_on_.load(std::memory_order_acquire)) {

        // Falls readsb nicht da ist: kurz schlafen und retry
        if (!ensure_fifo_writer_open()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Daten aus Queue holen (blockieren wollen wir nicht hart)
        size_t got = 0;
        uint16_t iq;
        while (got + 2 <= OUT_CHUNK && fifo_q16_.pop(iq)) {
            out[got++] = (uint8_t)(iq & 0xFF);        // I
            out[got++] = (uint8_t)((iq >> 8) & 0xFF); // Q
        }

        if (got == 0) {
            // nix zu schreiben -> kurz schlafen
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // schreiben (kann trotzdem EAGAIN/EPIPE liefern, sauber handeln)
        size_t left = got;
        uint8_t* p = out;

        while (left > 0 && fifo_on_.load(std::memory_order_relaxed)) {
            ssize_t wr = write(fifo_wfd_, p, left);

            if (wr > 0) {
                p += wr;
                left -= (size_t)wr;
                fifo_bytes_written_.fetch_add((uint64_t)wr, std::memory_order_relaxed);
                continue;
            }

            if (wr < 0 && errno == EINTR) continue;

            if (wr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Pipe voll: kurz warten, dann weiter
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (wr < 0 && errno == EPIPE) {
                // readsb weg -> writer schließen, reconnect in outer loop
                std::fprintf(stderr, "FIFO writer EPIPE (reader gone)\n");
                close_fifo_writer();
                break;
            }

            std::perror("write fifo");
            close_fifo_writer();
            break;
        }
    }

    close_fifo_writer();
}


static char* cstr_dup(const std::string& s) {
    // execvp braucht char*
    return ::strdup(s.c_str());
}

bool ADSB::startReadsb(const std::string& readsbPath,
                      const std::string& fifoPath,
                      int jsonPort,
                      float jsonIntervalSec)
{
    // falls noch läuft -> stoppen
    stopReadsb();

    // Optional: readsb-Logfile
    int logfd = ::open("readsb.log", O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (logfd < 0) {
        std::perror("open readsb.log");
        // nicht fatal, dann halt ohne log redirect
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("fork");
        if (logfd >= 0) ::close(logfd);
        return false;
    }

    if (pid == 0) {
        // Child: readsb starten

        // stdout/stderr ins Log umleiten (optional)
        if (logfd >= 0) {
            ::dup2(logfd, STDOUT_FILENO);
            ::dup2(logfd, STDERR_FILENO);
            ::close(logfd);
        }

        // Argumente bauen (Reihenfolge: device-type, dann device options)
        // Wichtig: jsonIntervalSec als String
        char intervalBuf[32];
        std::snprintf(intervalBuf, sizeof(intervalBuf), "%.3f", jsonIntervalSec);

        char portBuf[16];
        std::snprintf(portBuf, sizeof(portBuf), "%d", jsonPort);

        std::vector<std::string> args = {
            readsbPath,
            "--device-type", "ifile",
            "--ifile", fifoPath,
            "--iformat", "UC8",
            "--freq", "1090000000",
            "--throttle",

            "--net",
            "--net-bind-address", "127.0.0.1",
            "--net-json-port", portBuf,
            "--net-json-port-interval", intervalBuf,
            "--quiet"
        };

        // execvp-style argv
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& a : args) argv.push_back(cstr_dup(a));
        argv.push_back(nullptr);

        ::execvp(argv[0], argv.data());

        // Nur wenn exec fehlschlägt:
        std::perror("execvp(readsb)");
        _exit(127);
    }

    // Parent
    if (logfd >= 0) ::close(logfd);

    readsb_pid_ = pid;
    std::fprintf(stderr, "readsb started, pid=%d (json port %d)\n", (int)pid, jsonPort);
    return true;
}

void ADSB::stopReadsb()
{
    if (readsb_pid_ <= 0) return;

    // 1) SIGTERM
    kill(readsb_pid_, SIGTERM);

    // 2) warten bis beendet
    int status = 0;
    for (int i = 0; i < 50; i++) {           // 5s
        pid_t r = waitpid(readsb_pid_, &status, WNOHANG);
        if (r == readsb_pid_) {
            readsb_pid_ = -1;
            return;
        }
        usleep(100 * 1000);
    }

    // 3) hart beenden
    kill(readsb_pid_, SIGKILL);
    waitpid(readsb_pid_, &status, 0);
    readsb_pid_ = -1;
}
