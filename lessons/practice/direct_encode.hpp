#pragma once

// Streams a BattleResult straight into ETF bytes, byte-identical to
// term::encode(wire::encode_result(result)) but without building a Value tree.
// See lesson 21 and encode_bench.cpp, which checks the byte-for-byte equality.

#include "gamebattle/engine.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace practice {

inline const char* etf_side_name(gamebattle::Side side) {
    return side == gamebattle::Side::attacker ? "attacker" : "defender";
}

inline const char* etf_winner_name(gamebattle::Winner winner) {
    switch (winner) {
    case gamebattle::Winner::attacker: return "attacker";
    case gamebattle::Winner::defender: return "defender";
    case gamebattle::Winner::draw: return "draw";
    }
    return "draw";
}

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

inline std::vector<std::uint8_t> encode_result_direct(const gamebattle::BattleResult& result) {
    EtfWriter writer(64 + result.events.size() * 160 + result.units.size() * 80);
    writer.version();
    writer.map_header(10);
    writer.field_u64("battle_id", result.battle_id);
    writer.field_u64("seed", result.seed);
    writer.field_u64("source_battle_id", result.source_battle_id);
    writer.field_atom("winner", etf_winner_name(result.winner));
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
            writer.field_atom("side", etf_side_name(event.side));
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
            writer.field_atom("side", etf_side_name(unit.side));
            writer.field("initial_hp", unit.initial_hp);
            writer.field("hp", unit.hp);
            writer.field("max_hp", unit.max_hp);
            writer.field_atom("alive", unit.alive ? "true" : "false");
        }
        writer.nil();
    }
    return writer.take();
}

} // namespace practice
