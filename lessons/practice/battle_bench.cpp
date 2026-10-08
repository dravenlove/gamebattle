// Lesson 21: measure where the time goes on the full Port path.
//
//   battle_bench [iterations]
//   battle_bench --dump-request FILE      write the sample request as ETF bytes

#include "request_codec.hpp"
#include "sample_battle.hpp"

#include "gamebattle/engine.hpp"
#include "gamebattle/term.hpp"
#include "gamebattle/wire.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Prevents the optimizer from deleting work whose result is otherwise unused.
volatile std::uint64_t g_sink = 0;

struct Stage {
    const char* name;
    double total_ns{0};
};

double ns_since(Clock::time_point start) {
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

int dump_request(const char* path) {
    const auto bytes = gamebattle::term::encode(practice::encode_request(practice::sample_battle(1, 42)));
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::cout << "wrote " << bytes.size() << " bytes to " << path << '\n';
    return out ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string_view(argv[1]) == "--dump-request") {
        return dump_request(argv[2]);
    }
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 3000;

    std::vector<std::vector<std::uint8_t>> encoded;
    encoded.reserve(static_cast<std::size_t>(iterations));
    for (int index = 0; index < iterations; ++index) {
        encoded.push_back(gamebattle::term::encode(
            practice::encode_request(practice::sample_battle(index + 1, 5000 + index))));
    }

    for (int index = 0; index < 50; ++index) {  // warm caches and the allocator
        g_sink = g_sink + gamebattle::wire::handle_etf(encoded[static_cast<std::size_t>(index) % encoded.size()]).size();
    }

    Stage decode{"term::decode"}, parse{"wire::parse_request"}, simulate{"Engine::simulate"},
        encode{"encode_result + term::encode"};
    std::uint64_t events = 0;
    for (const auto& bytes : encoded) {
        auto start = Clock::now();
        const auto value = gamebattle::term::decode(bytes);
        decode.total_ns += ns_since(start);

        start = Clock::now();
        const auto request = gamebattle::wire::parse_request(value);
        parse.total_ns += ns_since(start);

        start = Clock::now();
        const auto result = gamebattle::Engine{}.simulate(request);
        simulate.total_ns += ns_since(start);

        start = Clock::now();
        const auto reply = gamebattle::term::encode(gamebattle::wire::encode_result(result));
        encode.total_ns += ns_since(start);

        events += result.events.size();
        g_sink = g_sink + reply.size();
    }

    const double total_ns = decode.total_ns + parse.total_ns + simulate.total_ns + encode.total_ns;
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "iterations: " << iterations << ", request bytes: " << encoded.front().size()
              << ", avg events/battle: " << static_cast<double>(events) / iterations << "\n\n";
    std::cout << std::left << std::setw(32) << "stage" << std::right << std::setw(12) << "us/battle"
              << std::setw(10) << "share" << '\n';
    for (const Stage* stage : {&decode, &parse, &simulate, &encode}) {
        std::cout << std::left << std::setw(32) << stage->name << std::right << std::setw(12)
                  << stage->total_ns / iterations / 1000.0 << std::setw(9)
                  << 100.0 * stage->total_ns / total_ns << "%\n";
    }
    std::cout << std::left << std::setw(32) << "total" << std::right << std::setw(12)
              << total_ns / iterations / 1000.0 << '\n';
    std::cout << "\nsimulate: " << simulate.total_ns / static_cast<double>(events)
              << " ns/event, single-thread throughput "
              << 1e9 * iterations / total_ns << " battles/s (full path)\n";
    return 0;
}
