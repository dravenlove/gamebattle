// Lesson 21: three ways to turn a BattleResult into ETF bytes.
//
//   A. current engine path: wire::encode_result builds a term::Value tree with
//      Value::object({...}) initializer lists, then term::encode writes it.
//   B. same tree, built without initializer lists, so nothing is deep-copied.
//   C. no tree at all: write ETF bytes straight from the BattleResult.
//
// All three must produce byte-identical output; the program checks that first.
//
//   encode_bench [battles]

#include "direct_encode.hpp"
#include "sample_battle.hpp"

#include "gamebattle/engine.hpp"
#include "gamebattle/term.hpp"
#include "gamebattle/wire.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using gamebattle::term::Value;

// ---------- B: build the Value tree with moves instead of initializer-list copies ----------

template <typename... Fields>
Value object_of(Fields&&... fields) {
    Value::Object object;
    object.reserve(sizeof...(fields));
    (object.emplace_back(std::forward<Fields>(fields)), ...);  // fold expression: one emplace per field
    return Value::object(std::move(object));
}

Value int_value(std::int64_t value) { return Value(value); }

Value checked_u64(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("result integer exceeds signed 64-bit ETF adapter limit");
    }
    return Value(static_cast<std::int64_t>(value));
}

const char* side_name(gamebattle::Side side) { return side == gamebattle::Side::attacker ? "attacker" : "defender"; }

const char* winner_name(gamebattle::Winner winner) {
    switch (winner) {
    case gamebattle::Winner::attacker: return "attacker";
    case gamebattle::Winner::defender: return "defender";
    case gamebattle::Winner::draw: return "draw";
    }
    return "draw";
}

Value encode_result_moved(const gamebattle::BattleResult& result) {
    using Field = std::pair<std::string, Value>;
    Value::List events;
    events.reserve(result.events.size());
    for (const auto& event : result.events) {
        events.push_back(object_of(
            Field{"seq", int_value(event.seq)}, Field{"round", int_value(event.round)},
            Field{"phase", Value::atom(event.phase)}, Field{"type", Value::atom(event.type)},
            Field{"side", Value::atom(side_name(event.side))}, Field{"actor", checked_u64(event.actor)},
            Field{"target", checked_u64(event.target)}, Field{"source_id", int_value(event.source_id)},
            Field{"value", int_value(event.value)}, Field{"hp_before", int_value(event.hp_before)},
            Field{"hp_after", int_value(event.hp_after)},
            Field{"critical", Value::atom(event.critical ? "true" : "false")}));
    }
    Value::List units;
    units.reserve(result.units.size());
    for (const auto& unit : result.units) {
        units.push_back(object_of(
            Field{"id", checked_u64(unit.id)}, Field{"side", Value::atom(side_name(unit.side))},
            Field{"initial_hp", int_value(unit.initial_hp)}, Field{"hp", int_value(unit.hp)},
            Field{"max_hp", int_value(unit.max_hp)},
            Field{"alive", Value::atom(unit.alive ? "true" : "false")}));
    }
    return object_of(
        Field{"battle_id", checked_u64(result.battle_id)}, Field{"seed", checked_u64(result.seed)},
        Field{"source_battle_id", checked_u64(result.source_battle_id)},
        Field{"winner", Value::atom(winner_name(result.winner))}, Field{"reason", Value::atom(result.reason)},
        Field{"rounds", int_value(result.rounds)},
        Field{"attacker_initiative", checked_u64(result.attacker_initiative)},
        Field{"defender_initiative", checked_u64(result.defender_initiative)},
        Field{"events", Value::list(std::move(events))},   // moved, not copied
        Field{"units", Value::list(std::move(units))});
}

// ---------- measurement ----------

volatile std::size_t g_sink = 0;

template <typename Encoder>
double microseconds_per_battle(const std::vector<gamebattle::BattleResult>& results, Encoder encoder) {
    const auto start = std::chrono::steady_clock::now();
    for (const auto& result : results) {
        g_sink = g_sink + encoder(result).size();
    }
    const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start);
    return elapsed.count() / static_cast<double>(results.size());
}

} // namespace

int main(int argc, char** argv) {
    const int battles = argc > 1 ? std::atoi(argv[1]) : 1000;
    std::vector<gamebattle::BattleResult> results;
    results.reserve(static_cast<std::size_t>(battles));
    for (int index = 0; index < battles; ++index) {
        results.push_back(gamebattle::Engine{}.simulate(practice::sample_battle(index + 1, 5000 + index)));
    }

    const auto encode_a = [](const gamebattle::BattleResult& result) {
        return gamebattle::term::encode(gamebattle::wire::encode_result(result));
    };
    const auto encode_b = [](const gamebattle::BattleResult& result) {
        return gamebattle::term::encode(encode_result_moved(result));
    };
    const auto encode_c = [](const gamebattle::BattleResult& result) { return practice::encode_result_direct(result); };

    for (const auto& result : results) {
        const auto expected = encode_a(result);
        if (encode_b(result) != expected || encode_c(result) != expected) {
            std::cerr << "encoders disagree on battle " << result.battle_id << '\n';
            return 1;
        }
    }
    std::cout << "all " << battles << " results: A, B and C produce byte-identical ETF\n\n";

    const double a = microseconds_per_battle(results, encode_a);
    const double b = microseconds_per_battle(results, encode_b);
    const double c = microseconds_per_battle(results, encode_c);
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "A  initializer_list tree + term::encode : " << std::setw(8) << a << " us/battle\n";
    std::cout << "B  moved tree + term::encode            : " << std::setw(8) << b << " us/battle  (x"
              << a / b << ")\n";
    std::cout << "C  direct streaming writer              : " << std::setw(8) << c << " us/battle  (x"
              << a / c << ")\n";
    return 0;
}
