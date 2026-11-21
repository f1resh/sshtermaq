#include "core/Threading.h"

#include "core/Log.h"

#include <algorithm>

namespace st {

ThreadPool::ThreadPool(size_t threads, size_t maxThreads, std::chrono::milliseconds idleExit)
    : min_(threads), max_(std::max(threads, maxThreads)), idleExit_(idleExit) {
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < min_; ++i) startWorker(false);
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_)
        if (w.thread.joinable()) w.thread.join();
}

void ThreadPool::startWorker(bool extra) {
    Worker& w = workers_.emplace_back();
    ++live_;
    ++idle_;  // free from the start: it takes the next job (counted before its thread even runs)
    w.thread = std::thread([this, self = &w, extra] { run(self, extra); });
}

void ThreadPool::submit(std::function<void()> job) {
    {
        std::lock_guard lock(mutex_);
        // Workers that ended: join them (they returned, or are about to).
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (it->done) {
                if (it->thread.joinable()) it->thread.join();
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
        jobs_.push_back(std::move(job));
        if (jobs_.size() > idle_ && live_ < max_) startWorker(true);  // nobody free to take it now
    }
    cv_.notify_one();
}

size_t ThreadPool::threadCount() {
    std::lock_guard lock(mutex_);
    return live_;
}

void ThreadPool::run(Worker* self, bool extra) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            const auto ready = [this] { return stop_ || !jobs_.empty(); };
            if (extra) {
                if (!cv_.wait_for(lock, idleExit_, ready) && live_ > min_) {
                    --idle_;
                    --live_;
                    self->done = true;  // joined by the next submit or the destructor
                    return;
                }
                cv_.wait(lock, ready);  // a regular worker again when the others went first
            } else {
                cv_.wait(lock, ready);
            }
            --idle_;
            if (stop_ && jobs_.empty()) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        try {
            job();
        } catch (const std::exception& e) {
            applog::error(std::string("ThreadPool job threw: ") + e.what());
        } catch (...) {
            applog::error("ThreadPool job threw unknown exception");
        }
        std::lock_guard lock(mutex_);
        ++idle_;
    }
}

ThreadPool& ThreadPool::shared() {
    static ThreadPool pool(4, 16);
    return pool;
}

Debouncer::~Debouncer() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        pending_ = nullptr;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Debouncer::schedule(std::chrono::milliseconds delay, std::function<void()> fn) {
    {
        std::lock_guard lock(mutex_);
        pending_ = std::move(fn);
        due_ = std::chrono::steady_clock::now() + delay;
        if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    }
    cv_.notify_all();
}

void Debouncer::cancel() {
    std::lock_guard lock(mutex_);
    pending_ = nullptr;
}

void Debouncer::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        cv_.wait(lock, [this] { return stop_ || pending_; });
        if (stop_) return;
        if (std::chrono::steady_clock::now() < due_) {
            cv_.wait_until(lock, due_);  // then look again: it may have moved, been replaced or cancelled
            continue;
        }
        auto fn = std::move(pending_);
        pending_ = nullptr;
        lock.unlock();
        try {
            fn();
        } catch (const std::exception& e) {
            applog::error(std::string("Debounced job threw: ") + e.what());
        }
        lock.lock();
    }
}

}  // namespace st
