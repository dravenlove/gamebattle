// Lesson 22: fuzz the ETF entry point that both the Port and the NIF expose.
//
// Build with sanitizers so memory errors and undefined behaviour become crashes:
//   -fsanitize=address,undefined
//
//   term_fuzz [iterations] [seed]
//
// LLVMFuzzerTestOneInput is the standard libFuzzer entry point. Where libFuzzer
// is available, compile with -DGAMEBATTLE_LIBFUZZER -fsanitize=fuzzer instead of
// using the small mutation driver below.

#include "request_codec.hpp"
#include "sample_battle.hpp"

#include "gamebattle/term.hpp"
#include "gamebattle/wire.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <span>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    // handle_etf must turn every input into an {ok, _} or {error, _} reply.
    const auto reply = gamebattle::wire::handle_etf(input);
    const auto decoded = gamebattle::term::decode(reply);  // our own reply must always decode
    static_cast<void>(decoded);
    return 0;
}

#ifndef GAMEBATTLE_LIBFUZZER

namespace {

std::vector<std::vector<std::uint8_t>> seed_corpus() {
    using gamebattle::term::Value;
    std::vector<std::vector<std::uint8_t>> corpus;
    corpus.push_back(gamebattle::term::encode(practice::encode_request(practice::sample_battle(1, 42))));
    corpus.push_back(gamebattle::term::encode(Value::atom("ping")));
    corpus.push_back(gamebattle::term::encode(Value::tuple({Value::atom("load_config"), Value::binary("/nonexistent.gbcfg")})));
    corpus.push_back(gamebattle::term::encode(Value::list({Value(std::int64_t{1}), Value(std::int64_t{-5'000'000'000LL}), Value(1.5)})));
    return corpus;
}

std::vector<std::uint8_t> mutate(std::vector<std::uint8_t> input, std::mt19937_64& rng,
                                 const std::vector<std::vector<std::uint8_t>>& corpus) {
    auto pick = [&rng](std::size_t bound) {
        return static_cast<std::size_t>(rng() % (bound == 0 ? 1 : bound));
    };
    static constexpr std::uint8_t kInteresting[] = {0x00, 0x01, 0x7f, 0x80, 0xff, 97, 98, 104, 106, 107, 108, 109, 110, 116, 119};
    const int rounds = 1 + static_cast<int>(pick(4));
    for (int round = 0; round < rounds; ++round) {
        switch (pick(6)) {
        case 0:  // flip a bit
            if (!input.empty()) input[pick(input.size())] ^= static_cast<std::uint8_t>(1U << pick(8));
            break;
        case 1:  // overwrite with an interesting byte (ETF tags, boundaries)
            if (!input.empty()) input[pick(input.size())] = kInteresting[pick(std::size(kInteresting))];
            break;
        case 2:  // truncate
            input.resize(pick(input.size() + 1));
            break;
        case 3:  // insert a random byte
            input.insert(input.begin() + static_cast<std::ptrdiff_t>(pick(input.size() + 1)),
                         static_cast<std::uint8_t>(rng()));
            break;
        case 4: {  // splice a slice of another corpus entry
            const auto& other = corpus[pick(corpus.size())];
            if (!other.empty()) {
                const auto from = pick(other.size());
                const auto length = pick(other.size() - from) + 1;
                input.insert(input.begin() + static_cast<std::ptrdiff_t>(pick(input.size() + 1)),
                             other.begin() + static_cast<std::ptrdiff_t>(from),
                             other.begin() + static_cast<std::ptrdiff_t>(from + length));
            }
            break;
        }
        default:  // set a 4-byte big-endian length/count field to an extreme
            if (input.size() >= 4) {
                const auto at = pick(input.size() - 3);
                const std::uint32_t extremes[] = {0, 1, 0x7fffffffU, 0xffffffffU, 1'000'000U};
                const auto value = extremes[pick(std::size(extremes))];
                for (int shift = 0; shift < 4; ++shift) {
                    input[at + static_cast<std::size_t>(shift)] = static_cast<std::uint8_t>(value >> (24 - 8 * shift));
                }
            }
            break;
        }
    }
    return input;
}

std::string error_type(const std::vector<std::uint8_t>& reply) {
    const auto value = gamebattle::term::decode(reply);
    const auto& tuple = std::get<gamebattle::term::Value::TupleValue>(value.data).value;
    const auto status = gamebattle::term::as_string(tuple[0], "status");
    if (status == "ok") return "ok";
    const auto* type = gamebattle::term::find(tuple[1], "type");
    return type == nullptr ? "error" : "error:" + gamebattle::term::as_string(*type, "type");
}

} // namespace

int main(int argc, char** argv) {
    const long iterations = argc > 1 ? std::atol(argv[1]) : 20000;
    const auto seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1ULL;
    std::mt19937_64 rng(seed);
    const auto corpus = seed_corpus();
    std::map<std::string, long> outcomes;

    const auto start = std::chrono::steady_clock::now();
    for (long iteration = 0; iteration < iterations; ++iteration) {
        const auto input = mutate(corpus[static_cast<std::size_t>(rng() % corpus.size())], rng, corpus);
        LLVMFuzzerTestOneInput(input.data(), input.size());
        if (iteration % 64 == 0) {
            ++outcomes[error_type(gamebattle::wire::handle_etf(input))];
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "iterations: " << iterations << " in " << seconds << " s, no crash\n";
    std::cout << "sampled outcomes:\n";
    for (const auto& [type, count] : outcomes) {
        std::cout << "  " << type << ": " << count << '\n';
    }
    return 0;
}

#endif
