// Lesson 17: run many battles concurrently on a thread pool and prove the
// results are identical to a single-threaded run.
//
//   battle_thread_pool [battles]

#include "result_hash.hpp"
#include "sample_battle.hpp"
#include "thread_pool.hpp"

#include "gamebattle/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::vector<std::uint64_t> run_serial(const std::vector<gamebattle::BattleRequest>& requests) {
    std::vector<std::uint64_t> hashes;
    hashes.reserve(requests.size());
    for (const auto& request : requests) {
        hashes.push_back(practice::result_hash(gamebattle::Engine{}.simulate(request)));
    }
    return hashes;
}

std::vector<std::uint64_t> run_pooled(const std::vector<gamebattle::BattleRequest>& requests,
                                      std::size_t threads) {
    practice::ThreadPool pool(threads);
    std::vector<std::future<std::uint64_t>> futures;
    futures.reserve(requests.size());
    for (const auto& request : requests) {
        // Capture by reference is safe: requests outlives every future we wait on below.
        futures.push_back(pool.submit([&request] {
            return practice::result_hash(gamebattle::Engine{}.simulate(request));
        }));
    }
    std::vector<std::uint64_t> hashes;
    hashes.reserve(futures.size());
    for (auto& future : futures) {
        hashes.push_back(future.get());  // results come back in submission order
    }
    return hashes;
}

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t battles = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 2000;
    std::vector<gamebattle::BattleRequest> requests;
    requests.reserve(battles);
    for (std::size_t index = 0; index < battles; ++index) {
        requests.push_back(practice::sample_battle(index + 1, 1000 + index));
    }

    std::cout << "hardware_concurrency = " << std::thread::hardware_concurrency()
              << ", battles = " << battles << '\n';

    auto start = Clock::now();
    const auto expected = run_serial(requests);
    const double serial_seconds = seconds_since(start);
    std::cout << "serial        : " << serial_seconds << " s\n";

    for (const std::size_t threads : {1U, 2U, 4U}) {
        start = Clock::now();
        const auto hashes = run_pooled(requests, threads);
        const double elapsed = seconds_since(start);
        const bool identical = hashes == expected;
        std::cout << "pool " << threads << " thread" << (threads > 1 ? "s" : " ")
                  << ": " << elapsed << " s  speedup x" << serial_seconds / elapsed
                  << "  results identical: " << (identical ? "yes" : "NO") << '\n';
        if (!identical) {
            return 1;
        }
    }

    // An exception thrown inside a task is stored in the future and rethrown by get().
    practice::ThreadPool pool(1);
    auto broken = practice::sample_battle(9, 9);
    broken.max_rounds = 0;
    auto future = pool.submit([broken] { return gamebattle::Engine{}.simulate(broken).rounds; });
    try {
        static_cast<void>(future.get());
        std::cout << "invalid request was not rejected\n";
        return 1;
    } catch (const std::invalid_argument& error) {
        std::cout << "exception crossed threads via future: " << error.what() << '\n';
    }
    return 0;
}
