#pragma once
// A program on a pseudo-terminal for the platform tests: its output collected by a reader thread, as the local
// terminal transport does it, with waits for text to appear and for the end.
#include "platform/sys/Pty.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace st::test {

class PtyRun {
public:
    ~PtyRun() {
        if (!started_) return;
        pty_.kill();
        if (!status_) finish();
        if (reader_.joinable()) reader_.join();
    }

    /** Starts it; false with the error in error(). */
    bool start(const sys::pty::Options& o) {
        started_ = pty_.start(o, error_);
        if (started_)
            reader_ = std::thread([this] {
                char buf[4096];
                while (const std::size_t n = pty_.read(buf, sizeof buf)) {
                    std::lock_guard lock(mutex_);
                    output_.append(buf, n);
                    cv_.notify_all();
                }
                std::lock_guard lock(mutex_);
                ended_ = true;
                cv_.notify_all();
            });
        return started_;
    }

    /** True once the output contains `text` (within `timeout`). */
    bool waitFor(const std::string& text, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return output_.find(text) != std::string::npos; });
    }

    /** Waits for the program to exit, then for the end of its output; the exit status. */
    sys::pty::ExitStatus finish() {
        if (!status_) {
            status_ = pty_.wait();
            pty_.finishOutput();
        }
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(10), [&] { return ended_; });
        return *status_;
    }

    bool outputEnded() {
        std::lock_guard lock(mutex_);
        return ended_;
    }
    std::string output() {
        std::lock_guard lock(mutex_);
        return output_;
    }
    const std::string& error() const { return error_; }
    sys::pty::Pty& pty() { return pty_; }

private:
    sys::pty::Pty pty_;
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::string output_, error_;
    bool started_ = false, ended_ = false;
    std::optional<sys::pty::ExitStatus> status_;
};

}  // namespace st::test
