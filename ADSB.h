#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <string>
#include <boost/lockfree/spsc_queue.hpp>
#include "global.h"

class ADSB {
public:
    static ADSB& getInstance();

    bool startIQStreamFIFO(const char* fifo_path, float gain);
    void stopIQStreamFIFO();
    bool startReadsb(const std::string& readsbPath,
                     const std::string& fifoPath,
                     int jsonPort,
                     float jsonIntervalSec);
    void stopReadsb();

private:
    ADSB();
    ~ADSB();
    void worker();
    void handleADSBsamples(const liquid_float_complex* samples,std::uint32_t count);

    std::thread ADBSworkerThread;

    // constants
    float fs_  = 2.4e6f;
    float fs_out_ = 2.0e6f;

    // ===
    void fifoWriterThreadFn();
    bool ensure_fifo_writer_open();
    void close_fifo_writer();

    // Stream state
    std::atomic<bool> fifo_on_{false};
    std::string fifo_path_;
    float iq_gain_ = 1.0f;

    // FIFO fds
    int fifo_wfd_ = -1;
    int fifo_rfd_ = -1; // dummy reader (keepalive)

    // Writer thread
    std::thread fifo_writer_thread_;

    // Queue: bytes (UC8 interleaved)
    static constexpr size_t FIFO_Q_SAMPLES = 1 << 19; // 524288 IQ-Paare (~0.22s bei 2.4Msps)
    boost::lockfree::spsc_queue<uint16_t, boost::lockfree::capacity<FIFO_Q_SAMPLES>> fifo_q16_;

    // Stats
    std::atomic<uint64_t> fifo_bytes_enqueued_{0};
    std::atomic<uint64_t> fifo_bytes_written_{0};
    std::atomic<uint64_t> fifo_bytes_dropped_{0};

    pid_t readsb_pid_ = -1;
};
