// The worker pool (core/worker_pool.hpp): the sim's independent work split
// over the spare cores, each index done once, the caller back only when all
// are.

#include <catch2/catch_test_macros.hpp>

#include "core/worker_pool.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

using osc::WorkerPool;

TEST_CASE("The worker pool does each index once, and returns when all are done", "[pool]") {
    auto& pool = WorkerPool::instance();
    for (const size_t count : {size_t{1}, size_t{7}, size_t{64}, size_t{1000}, size_t{4097}}) {
        std::vector<int> hits(count, 0);
        pool.for_each(count, 16, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) ++hits[i];
        });
        CHECK(std::all_of(hits.begin(), hits.end(), [](int h) { return h == 1; }));
    }
}

TEST_CASE("A slot per index gives the same results however the work is shared", "[pool]") {
    auto& pool = WorkerPool::instance();
    const auto run = [&] {
        std::vector<unsigned> out(5000);
        pool.for_each(out.size(), 32, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) out[i] = static_cast<unsigned>(i * 2654435761u);
        });
        return out;
    };
    const auto shared = run();
    pool.set_enabled(false); // the caller's thread alone
    CHECK(pool.workers() == 0);
    const auto alone = run();
    pool.set_enabled(true);
    CHECK(shared == alone);
}

TEST_CASE("A for_each inside a for_each's work runs on its caller", "[pool]") {
    auto& pool = WorkerPool::instance();
    std::atomic<int> inner{0};
    pool.for_each(8, 1, [&](size_t, size_t) {
        pool.for_each(10, 1,
                      [&](size_t begin, size_t end) { inner += static_cast<int>(end - begin); });
    });
    CHECK(inner == 80);
}

TEST_CASE("Two threads' for_each calls take turns", "[pool]") {
    auto& pool = WorkerPool::instance();
    std::atomic<long> sum{0};
    const auto add = [&] {
        for (int round = 0; round < 50; ++round)
            pool.for_each(200, 8, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) sum += static_cast<long>(i);
            });
    };
    std::thread other(add);
    add();
    other.join();
    CHECK(sum == 2L * 50 * (199L * 200 / 2));
}

TEST_CASE("A throwing pool chunk finishes the job before propagating, and the pool stays usable",
          "[pool][exceptions]") {
    auto& pool = WorkerPool::instance();
    std::atomic<int> chunks{0};
    CHECK_THROWS_AS(pool.for_each(64, 1,
                                  [&](size_t begin, size_t) {
                                      ++chunks;
                                      if (begin == 0) throw std::runtime_error("chunk failed");
                                  }),
                    std::runtime_error);
    CHECK(chunks == 64);
    std::atomic<int> after{0};
    pool.for_each(64, 1, [&](size_t begin, size_t end) { after += static_cast<int>(end - begin); });
    CHECK(after == 64);
    pool.set_enabled(false);
    CHECK_THROWS_AS(
        pool.for_each(64, 1, [](size_t, size_t) { throw std::runtime_error("inline failed"); }),
        std::runtime_error);
    pool.set_enabled(true);
    pool.for_each(64, 1, [&](size_t begin, size_t end) { after += static_cast<int>(end - begin); });
    CHECK(after == 128);
}
