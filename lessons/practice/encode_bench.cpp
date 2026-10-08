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

// ---------- C: stream ETF bytes directly, mirroring term.cpp's Writer choices ----------

class EtfWriter {
public:
    explicit EtfWriter(std::size_t expected_bytes) { out_.reserve(expected_bytes); }

    void version() { out_.push_back(131); }
    void map_header(std::uint32_t count) { out_.push_back(116); u32(count); }
    void list_header(std::uint32_t count) { out_.push_back(108); u32(count); }
    void nil() { out_.push_back(106); }

    void atom(std::string_view text) {
        if (text.size() <= 255) {
            out_.push_back(119);
            out_.push_back(static_cast<std::uint8_t>(text.size()));
        } else {
            out_.push_back(118);
            out_.push_back(static_cast<std::uint8_t>(text.size() >> 8U));
            out_.push_back(static_cast<std::uint8_t>(text.size()));
        }
        out_.insert(out_.end(), text.begin(), text.end());
    }

    void integer(std::int64_t value) {
        if (value >= 0 && value <= 255) {
            out_.push_back(97);
            out_.push_back(static_cast<std::uint8_t>(value));
        } else if (value >= std::numeric_limits<std::int32_t>::min() &&
                   value <= std::numeric_limits<std::int32_t>::max()) {
            out_.push_back(98);
            u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
        } else {
            const std::uint64_t magnitude = value < 0
                ? (value == std::numeric_limits<std::int64_t>::min() ? (std::uint64_t{1} << 63U)
                                                                    : static_cast<std::uint64_t>(-value))
                : static_cast<std::uint64_t>(value);
            std::uint8_t count = 0;
            for (auto copy = magnitude; copy != 0; copy >>= 8U) ++count;
            out_.push_back(110);
            out_.push_back(count);
            out_.push_back(value < 0 ? 1 : 0);
            for (std::uint8_t index = 0; index < count; ++index) {
                out_.push_back(static_cast<std::uint8_t>(magnitude >> (index * 8U)));
            }
        }
    }

    void unsigned_integer(std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error("result integer exceeds signed 64-bit ETF adapter limit");
        }
        integer(static_cast<std::int64_t>(value));
    }

    void field(std::string_view key, std::int64_t value) { atom(key); integer(value); }
    void field_u64(std::string_view key, std::uint64_t value) { atom(key); unsigned_integer(value); }
    void field_atom(std::string_view key, std::string_view value) { atom(key); atom(value); }

    std::vector<std::uint8_t> take() { return std::move(out_); }

private:
    void u32(std::uint32_t value) {
        out_.push_back(static_cast<std::uint8_t>(value >> 24U));
        out_.push_back(static_cast<std::uint8_t>(value >> 16U));
        out_.push_back(static_cast<std::uint8_t>(value >> 8U));
        out_.push_back(static_cast<std::uint8_t>(value));
    }

    std::vector<std::uint8_t> out_;
};

std::vector<std::uint8_t> encode_result_direct(const gamebattle::BattleResult& result) {
    EtfWriter writer(64 + result.events.size() * 160 + result.units.size() * 80);
    writer.version();
    writer.map_header(10);
    writer.field_u64("battle_id", result.battle_id);
    writer.field_u64("seed", result.seed);
    writer.field_u64("source_battle_id", result.source_battle_id);
    writer.field_atom("winner", winner_name(result.winner));
    writer.field_atom("reason", result.reason);
    writer.field("rounds", result.rounds);
    writer.field_u64("attacker_initiative", result.attacker_initiative);
    writer.field_u64("defender_initiative", result.defender_initiative);

    writer.atom("events");
    if (result.events.empty()) {
        writer.nil();
    } else {
        writer.list_header(static_cast<std::uint32_t>(result.events.size()));
        for (const auto& event : result.events) {
            writer.map_header(12);
            writer.field("seq", event.seq);
            writer.field("round", event.round);
            writer.field_atom("phase", event.phase);
            writer.field_atom("type", event.type);
            writer.field_atom("side", side_name(event.side));
            writer.field_u64("actor", event.actor);
            writer.field_u64("target", event.target);
            writer.field("source_id", event.source_id);
            writer.field("value", event.value);
            writer.field("hp_before", event.hp_before);
            writer.field("hp_after", event.hp_after);
            writer.field_atom("critical", event.critical ? "true" : "false");
        }
        writer.nil();
    }

    writer.atom("units");
    if (result.units.empty()) {
        writer.nil();
    } else {
        writer.list_header(static_cast<std::uint32_t>(result.units.size()));
        for (const auto& unit : result.units) {
            writer.map_header(6);
            writer.field_u64("id", unit.id);
            writer.field_atom("side", side_name(unit.side));
            writer.field("initial_hp", unit.initial_hp);
            writer.field("hp", unit.hp);
            writer.field("max_hp", unit.max_hp);
            writer.field_atom("alive", unit.alive ? "true" : "false");
        }
        writer.nil();
    }
    return writer.take();
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
    const auto encode_c = [](const gamebattle::BattleResult& result) { return encode_result_direct(result); };

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
