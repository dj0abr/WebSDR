#include "FT8DecoderProcess.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
namespace {
struct FD {
    int value = -1;
    ~FD() { close(); }
    void close() { if (value >= 0) ::close(value); value = -1; }
};
void pipe(FD& read, FD& write) {
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC)) throw std::runtime_error(std::strerror(errno));
    read.value = fds[0]; write.value = fds[1];
    for (auto* fd : {&read, &write}) {
        if (fd->value < 3) {
            const int copy = fcntl(fd->value, F_DUPFD_CLOEXEC, 3);
            if (copy < 0) throw std::runtime_error(std::strerror(errno));
            fd->close(); fd->value = copy;
        }
    }
}
struct Child {
    pid_t pid = -1;
    bool reaped = false;
    ~Child() {
        if (pid > 0) {
            ::kill(-pid, SIGKILL);
            if (!reaped) while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }
};
std::string utcLabel(std::int64_t seconds) {
    const std::time_t value = seconds;
    std::tm utc{};
    if (!gmtime_r(&value, &utc)) throw std::runtime_error("invalid UTC slot");
    char label[16];
    std::strftime(label, sizeof(label), "%H%M%S", &utc);
    return label;
}
void drain(FD& fd, std::string& output) {
    char buffer[8192];
    for (;;) {
        const auto n = ::read(fd.value, buffer, sizeof(buffer));
        if (n > 0) {
            if (output.size() + n > 256 * 1024)
                throw std::runtime_error("decoder output exceeds 256 KiB");
            output.append(buffer, n);
        } else if (n == 0) { fd.close(); return; }
        else if (errno == EINTR) continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        else throw std::runtime_error(std::strerror(errno));
    }
}
}

FT8DecoderProcess::FT8DecoderProcess(std::string executable,
        std::chrono::milliseconds timeout, ResultSink resultSink)
    : executable_(executable.empty() ? executablePath() : std::move(executable)),
      timeout_(timeout), resultSink_(std::move(resultSink)), thread_(&FT8DecoderProcess::worker, this) {}

FT8DecoderProcess::~FT8DecoderProcess() { stop(); }

std::string FT8DecoderProcess::executablePath() {
    std::array<char, 4096> path{};
    const auto n = ::readlink("/proc/self/exe", path.data(), path.size());
    if (n < 0 || static_cast<std::size_t>(n) == path.size())
        throw std::runtime_error("cannot locate ft8-decoder beside receiver");
    std::string executable(path.data(), n);
    return executable.substr(0, executable.find_last_of('/') + 1) + "ft8-decoder";
}

bool FT8DecoderProcess::enqueue(FT8Slot&& slot) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock || stopping_ || queue_.size() >= 2) {
        std::fprintf(stderr, "[FT8] Decoder busy; dropping UTC slot %s\n",
                     utcLabel(slot.utcSeconds).c_str());
        return false;
    }
    queue_.push_back(std::move(slot));
    ready_.notify_one();
    return true;
}

void FT8DecoderProcess::stop() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; queue_.clear(); }
    ready_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void FT8DecoderProcess::worker() {
    // A failed stdin write must never terminate the SDR application.
    sigset_t signals;
    sigemptyset(&signals); sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    while (!stopping_) {
        FT8Slot slot;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) break;
            slot = std::move(queue_.front()); queue_.pop_front();
        }
        try {
            std::istringstream lines(decode(slot, executable_, timeout_, stopping_));
            FT8DecodedSlot result;
            result.utcSeconds = slot.utcSeconds;
            result.dialHz = slot.dialHz;
            std::string line;
            while (std::getline(lines, line)) {
                if (line.empty()) continue;
                std::fprintf(stdout, "[FT8 %.6f MHz] %s\n", slot.dialHz / 1e6, line.c_str());
                result.lines.push_back(std::move(line));
            }
            std::fflush(stdout);
            if (resultSink_ && !stopping_) resultSink_(std::move(result));
        } catch (const std::exception& e) {
            if (!stopping_) std::fprintf(stderr, "[FT8 %s %.6f MHz] %s\n",
                utcLabel(slot.utcSeconds).c_str(), slot.dialHz / 1e6, e.what());
        }
    }
}

std::string FT8DecoderProcess::decode(const FT8Slot& slot,
        const std::string& executable, std::chrono::milliseconds timeout,
        const std::atomic<bool>& cancel) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                  "IEEE-754 float32 required");
    if (slot.audio.size() != 180000 || slot.utcSeconds % 15 != 0)
        throw std::runtime_error("expected a complete UTC-aligned 180000-sample slot");
    double peak = 0;
    for (float sample : slot.audio) {
        if (!std::isfinite(sample)) throw std::runtime_error("non-finite FT8 audio");
        peak = std::max(peak, std::abs(double(sample)));
    }
    // One linear gain for the whole slot, preserving relative signal strengths.
    const double gain = peak > 0 ? 0.8 / peak : 1;
    std::vector<unsigned char> bytes(slot.audio.size() * 4);
    for (std::size_t i = 0; i < slot.audio.size(); ++i) {
        const float sample = static_cast<float>(slot.audio[i] * gain);
        std::uint32_t bits;
        std::memcpy(&bits, &sample, 4);
        for (unsigned int b = 0; b < 4; ++b) bytes[4*i+b] = bits >> (8*b);
    }
    FD inRead, inWrite, outRead, outWrite, errRead, errWrite;
    pipe(inRead, inWrite); pipe(outRead, outWrite); pipe(errRead, errWrite);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_adddup2(&actions, inRead.value, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, outWrite.value, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errWrite.value, STDERR_FILENO);
    for (auto* fd : {&inRead, &inWrite, &outRead, &outWrite, &errRead, &errWrite})
        posix_spawn_file_actions_addclose(&actions, fd->value);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    std::string label = utcLabel(slot.utcSeconds);
    std::array<char*, 10> args{{const_cast<char*>(executable.c_str()),
        const_cast<char*>("--format"), const_cast<char*>("f32le"),
        const_cast<char*>("--output"), const_cast<char*>("text"),
        const_cast<char*>("--utc"), label.data(), const_cast<char*>("-"), nullptr}};
    Child child;
    const int error = posix_spawn(&child.pid, executable.c_str(), &actions,
                                  &attributes, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (error) { child.pid = -1; throw std::runtime_error(executable + ": " + std::strerror(error)); }
    inRead.close(); outWrite.close(); errWrite.close();
    for (auto* fd : {&inWrite, &outRead, &errRead})
        if (fcntl(fd->value, F_SETFL, O_NONBLOCK) < 0)
            throw std::runtime_error(std::strerror(errno));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t written = 0;
    std::string output, errors;
    int status = 0;
    bool exited = false;
    while (!exited || outRead.value >= 0 || errRead.value >= 0) {
        if (cancel) throw std::runtime_error("decoding cancelled");
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("decoder timeout");
        pollfd fds[3]{{inWrite.value, POLLOUT, 0}, {outRead.value, POLLIN, 0},
                      {errRead.value, POLLIN, 0}};
        const int polled = ::poll(fds, 3, 50);
        if (polled < 0 && errno != EINTR) throw std::runtime_error(std::strerror(errno));
        if (inWrite.value >= 0 && fds[0].revents) {
            const auto n = ::write(inWrite.value, bytes.data() + written, bytes.size() - written);
            if (n > 0) written += n;
            else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                inWrite.close();
            if (written == bytes.size()) inWrite.close(); // EOF starts decoding.
        }
        if (outRead.value >= 0 && fds[1].revents) drain(outRead, output);
        if (errRead.value >= 0 && fds[2].revents) drain(errRead, errors);
        if (!exited) {
            const auto waited = ::waitpid(child.pid, &status, WNOHANG);
            if (waited == child.pid) { exited = true; child.reaped = true; inWrite.close(); }
            else if (waited < 0 && errno != EINTR) throw std::runtime_error(std::strerror(errno));
        }
    }
    child.pid = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || written != bytes.size())
        throw std::runtime_error("decoder failed (status " + std::to_string(status) + "): " + errors);
    if (!errors.empty()) std::fprintf(stderr, "[FT8] %s", errors.c_str());
    return output;
}
