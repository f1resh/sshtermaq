#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

namespace st {

/** Marshals work onto the UI thread (implemented by the host window, platform/ui/HostWindow.h). */
class Dispatcher {
public:
    virtual ~Dispatcher() = default;
    /** Thread-safe. Queues fn to run on the UI thread. */
    virtual void post(std::function<void()> fn) = 0;
    virtual bool isUiThread() const = 0;
};

/**
 * Worker pool for blocking background work (bridge async handlers, imports, keygen...). It keeps `threads` workers
 * and, when a job would have to wait because all of them are busy, starts more, up to `maxThreads`: a few slow jobs
 * (SFTP calls on a connection that lost its network, big local copies) must not hold up everything else. The extra
 * workers end after `idleExit` without work.
 */
class ThreadPool {
public:
    explicit ThreadPool(size_t threads, size_t maxThreads = 0, std::chrono::milliseconds idleExit = std::chrono::minutes(1));
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> job);
    /** Workers running now (tests). */
    size_t threadCount();

    /** Process-wide pool (4 threads, up to 16 when busy), created on first use. */
    static ThreadPool& shared();

private:
    struct Worker {
        std::thread thread;
        bool done = false;  // returned (an extra worker that was idle too long): to be joined
    };
    void run(Worker* self, bool extra);
    void startWorker(bool extra);  // mutex_ held
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::list<Worker> workers_;
    size_t min_, max_;
    std::chrono::milliseconds idleExit_;
    size_t live_ = 0, idle_ = 0;
    bool stop_ = false;
};

/**
 * The newest of a burst of calls, run once they have been quiet for a while (saving the window size while it is being
 * resized). It waits on a thread of its own, started on first use and ended with the object, so it never holds a pool
 * worker; what is still pending then is dropped.
 */
class Debouncer {
public:
    Debouncer() = default;
    ~Debouncer();
    Debouncer(const Debouncer&) = delete;
    Debouncer& operator=(const Debouncer&) = delete;

    /** Replaces what is pending with `fn`, to run `delay` from now (unless replaced or cancelled before). */
    void schedule(std::chrono::milliseconds delay, std::function<void()> fn);
    /** Drops what is pending. */
    void cancel();

private:
    void run();
    std::mutex mutex_;
    std::condition_variable cv_;
    std::function<void()> pending_;
    std::chrono::steady_clock::time_point due_{};
    std::thread thread_;
    bool stop_ = false;
};

}  // namespace st
