#pragma once

#include "gamebattle/engine.hpp"
#include "gamebattle/term.hpp"
#include "gamebattle/wire.hpp"

#include <cstdint>

namespace practice {

// FNV-1a over the exact ETF bytes Erlang would receive: two results hash the
// same only if every event and every unit field is identical.
inline std::uint64_t result_hash(const gamebattle::BattleResult& result) {
    const auto bytes = gamebattle::term::encode(gamebattle::wire::encode_result(result));
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

} // namespace practice
