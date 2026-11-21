// core/Threading: the worker pool grows while all its workers are busy (and shrinks again), and the debouncer.
#include "core/Threading.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <future>
#include <vector>

using namespace st;
using namespace std::chrono_literals;

TEST_CASE("threads: with every worker blocked a new job still runs; the extra workers end when idle", "[core][threads]") {
    ThreadPool pool(4, 16, 150ms);
    CHECK(pool.threadCount() == 4);
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<int> started{0};
    for (int i = 0; i < 4; ++i)
        pool.submit([&, released] {
            ++started;
            released.wait();  // a slow job: an SFTP call on a dead connection
        });
    while (started < 4) std::this_thread::sleep_for(1ms);

    std::promise<void> ran;
    auto fifth = ran.get_future();
    const auto begin = std::chrono::steady_clock::now();
    pool.submit([&] { ran.set_value(); });
    REQUIRE(fifth.wait_for(2s) == std::future_status::ready);
    CHECK(std::chrono::steady_clock::now() - begin < 500ms);
    CHECK(pool.threadCount() == 5);

    // No more than the maximum.
    for (int i = 0; i < 30; ++i) pool.submit([released] { released.wait(); });
    std::this_thread::sleep_for(50ms);
    CHECK(pool.threadCount() == 16);

    release.set_value();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (pool.threadCount() > 4 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(20ms);
    CHECK(pool.threadCount() == 4);  // the extra workers went after 150 ms without work

    std::promise<void> again;
    auto later = again.get_future();
    pool.submit([&] { again.set_value(); });  // the pool still works (and joins the workers that ended)
    CHECK(later.wait_for(2s) == std::future_status::ready);
}

TEST_CASE("threads: the debouncer runs the newest call once things are quiet", "[core][threads]") {
    std::atomic<int> runs{0}, last{0};
    {
        Debouncer d;
        for (int i = 1; i <= 500; ++i) d.schedule(50ms, [&, i] {
            ++runs;
            last = i;
        });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (runs == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
        std::this_thread::sleep_for(100ms);
        CHECK(runs == 1);
        CHECK(last == 500);

        d.schedule(30ms, [&] { ++runs; });
        d.cancel();
        std::this_thread::sleep_for(100ms);
        CHECK(runs == 1);  // cancelled

        d.schedule(10s, [&] { ++runs; });
    }  // destroyed with a call pending: dropped, and the destructor does not wait 10 s
    CHECK(runs == 1);
}
