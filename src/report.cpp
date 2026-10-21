#include "gamebattle/report.hpp"

#include "battle_runtime.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gamebattle::report {
namespace {

// Field numbers and enum values below follow proto/battle_client.proto.

// Writes protobuf fields in the order they are called, leaving out zero
// values as proto3 does.
class Writer {
public:
    void varint(std::uint64_t value) {
        while (value >= 0x80) {
            out_.push_back(static_cast<char>((value & 0x7F) | 0x80));
            value >>= 7;
        }
        out_.push_back(static_cast<char>(value));
    }

    void uint(std::uint32_t field, std::uint64_t value) {
        if (value != 0) {
            key(field, 0);
            varint(value);
        }
    }

    // int32, int64 and enums: negative values take ten bytes, as in protobuf.
    void sint(std::uint32_t field, std::int64_t value) {
        uint(field, static_cast<std::uint64_t>(value));
    }

    void boolean(std::uint32_t field, bool value) {
        uint(field, value ? 1 : 0);
    }

    void message(std::uint32_t field, const Writer& child) {
        key(field, 2);
        varint(child.out_.size());
        out_ += child.out_;
    }

    void clear() { out_.clear(); }
    std::string take() { return std::move(out_); }

private:
    void key(std::uint32_t field, std::uint32_t wire_type) {
        varint((static_cast<std::uint64_t>(field) << 3) | wire_type);
    }

    std::string out_;
};

template <std::size_t N>
int index_of(const std::array<std::string_view, N>& names, std::string_view name) {
    for (std::size_t index = 0; index < N; ++index) {
        if (names[index] == name) {
            return static_cast<int>(index) + 1;
        }
    }
    return 0;  // *_UNSPECIFIED: clients skip values they don't know
}

constexpr std::array<std::string_view, 17> kEventTypes{
    "initiative", "action_start", "action_end", "skill", "passive", "damage",
    "direct_damage", "miss", "heal", "death", "buff_add", "buff_remove",
    "buff_reaction", "buff_expire", "chain", "negate", "fizzle"};
constexpr std::array<std::string_view, 5> kPhases{
    "battle", "round_start", "first_side", "second_side", "round_end"};
constexpr std::array<std::string_view, 7> kReasons{
    "initial_state", "battle_start", "round_start", "all_units_defeated",
    "round_end", "max_rounds", "event_limit"};

enum EventType : int {
    kInitiative = 1, kActionStart, kActionEnd, kSkill, kPassive, kDamage,
    kDirectDamage, kMiss, kHeal, kDeath, kBuffAdd, kBuffRemove, kBuffReaction,
    kBuffExpire, kChain, kNegate, kFizzle
};

int side_value(Side side) { return side == Side::attacker ? 1 : 2; }

int winner_value(Winner winner) {
    switch (winner) {
    case Winner::attacker: return 1;
    case Winner::defender: return 2;
    case Winner::draw: return 3;
    }
    return 0;
}

struct UnitChange {
    UnitId unit{0};
    std::int64_t damage{0};
    std::int64_t heal{0};
    std::uint32_t hits{0};
    std::uint32_t crits{0};
    std::uint32_t misses{0};
    std::int64_t hp{0};
    bool died{false};
};

struct EffectCount {
    int type{0};
    UnitId unit{0};
    std::uint32_t source_id{0};
    std::uint32_t count{0};
    std::int64_t value{0};
};

struct EffectKey {
    int type;
    UnitId unit;
    std::uint32_t source_id;
    bool operator==(const EffectKey&) const = default;
};

struct EffectKeyHash {
    std::size_t operator()(const EffectKey& key) const noexcept {
        std::uint64_t hash = key.unit * 0x9e3779b97f4a7c15ULL;
        hash ^= (static_cast<std::uint64_t>(key.source_id) << 8) ^
                static_cast<std::uint64_t>(key.type);
        return static_cast<std::size_t>(hash ^ (hash >> 29));
    }
};

class Step {
public:
    void start(const Event& event, int phase, bool action) {
        round_ = event.round;
        phase_ = phase;
        action_ = action;
        side_ = action ? side_value(event.side) : 0;
        actor_ = action ? event.actor : 0;
        skill_id_ = 0;
        event_count_ = 0;
        units_.clear();
        effects_.clear();
        effect_index_.clear();
        open_ = true;
    }

    bool open() const { return open_; }
    bool action() const { return action_; }
    bool continues(const Event& event, int phase) const {
        return open_ && !action_ && round_ == event.round && phase_ == phase;
    }

    void add(const Event& event, int type) {
        ++event_count_;
        switch (type) {
        case kActionStart:
        case kActionEnd:
            return;
        case kSkill:
            if (action_) {
                skill_id_ = event.source_id;
                return;
            }
            break;
        case kDamage:
        case kDirectDamage: {
            auto& change = unit(event.target);
            change.damage = runtime::saturating_add(change.damage, event.value);
            ++change.hits;
            change.crits += event.critical ? 1 : 0;
            change.hp = event.hp_after;
            return;
        }
        case kMiss: {
            auto& change = unit(event.target);
            ++change.misses;
            change.hp = event.hp_after;
            return;
        }
        case kHeal: {
            auto& change = unit(event.target);
            change.heal = runtime::saturating_add(change.heal, event.value);
            change.hp = event.hp_after;
            return;
        }
        case kDeath:
            unit(event.target).died = true;
            return;
        default:
            break;
        }
        const bool buff_change = type == kBuffAdd || type == kBuffRemove || type == kBuffExpire;
        const EffectKey key{type, buff_change ? event.target : event.actor, event.source_id};
        const auto [found, inserted] = effect_index_.try_emplace(key, effects_.size());
        if (inserted) {
            effects_.push_back(EffectCount{.type = type, .unit = key.unit,
                                           .source_id = event.source_id});
        }
        auto& effect = effects_[found->second];
        ++effect.count;
        effect.value = event.value;
    }

    void write(Writer& report, Writer& action, Writer& child) {
        action.clear();
        action.sint(1, round_);
        action.sint(2, phase_);
        action.sint(3, side_);
        action.uint(4, actor_);
        action.uint(5, skill_id_);
        for (const auto& change : units_) {
            child.clear();
            child.uint(1, change.unit);
            child.sint(2, change.damage);
            child.sint(3, change.heal);
            child.uint(4, change.hits);
            child.uint(5, change.crits);
            child.uint(6, change.misses);
            child.sint(7, change.hp);
            child.boolean(8, change.died);
            action.message(6, child);
        }
        for (const auto& effect : effects_) {
            child.clear();
            child.sint(1, effect.type);
            child.uint(2, effect.unit);
            child.uint(3, effect.source_id);
            child.uint(4, effect.count);
            child.sint(5, effect.value);
            action.message(7, child);
        }
        action.uint(8, event_count_);
        report.message(11, action);
        open_ = false;
    }

private:
    UnitChange& unit(UnitId id) {
        for (auto& change : units_) {
            if (change.unit == id) {
                return change;
            }
        }
        units_.push_back(UnitChange{.unit = id});
        return units_.back();
    }

    bool open_{false};
    bool action_{false};
    std::int32_t round_{0};
    int phase_{0};
    int side_{0};
    UnitId actor_{0};
    std::uint32_t skill_id_{0};
    std::uint32_t event_count_{0};
    std::vector<UnitChange> units_;
    std::vector<EffectCount> effects_;
    std::unordered_map<EffectKey, std::size_t, EffectKeyHash> effect_index_;
};

void write_actions(Writer& report, const std::vector<Event>& events) {
    Step step;
    Writer action;
    Writer child;
    for (const auto& event : events) {
        const int type = index_of(kEventTypes, event.type);
        const int phase = index_of(kPhases, event.phase);
        if (type == kActionStart) {
            if (step.open()) {
                step.write(report, action, child);
            }
            step.start(event, phase, true);
        } else if (!step.open() || (!step.action() && !step.continues(event, phase))) {
            if (step.open()) {
                step.write(report, action, child);
            }
            step.start(event, phase, false);
        }
        step.add(event, type);
        if (type == kActionEnd && step.action()) {
            step.write(report, action, child);
        }
    }
    if (step.open()) {
        step.write(report, action, child);
    }
}

void write_events(Writer& report, const std::vector<Event>& events) {
    Writer child;
    for (const auto& event : events) {
        child.clear();
        child.uint(1, event.seq);
        child.sint(2, event.round);
        child.sint(3, index_of(kPhases, event.phase));
        child.sint(4, index_of(kEventTypes, event.type));
        child.sint(5, side_value(event.side));
        child.uint(6, event.actor);
        child.uint(7, event.target);
        child.uint(8, event.source_id);
        child.sint(9, event.value);
        child.sint(10, event.hp_before);
        child.sint(11, event.hp_after);
        child.boolean(12, event.critical);
        report.message(10, child);
    }
}

} // namespace

std::string encode(const BattleResult& result, Detail detail) {
    Writer report;
    report.uint(1, result.battle_id);
    report.uint(2, result.seed);
    report.uint(3, result.source_battle_id);
    report.sint(4, winner_value(result.winner));
    report.sint(5, index_of(kReasons, result.reason));
    report.sint(6, result.rounds);
    report.uint(7, result.attacker_initiative);
    report.uint(8, result.defender_initiative);
    Writer unit;
    for (const auto& state : result.units) {
        unit.clear();
        unit.uint(1, state.id);
        unit.sint(2, side_value(state.side));
        unit.sint(3, state.initial_hp);
        unit.sint(4, state.hp);
        unit.sint(5, state.max_hp);
        unit.boolean(6, state.alive);
        report.message(9, unit);
    }
    if (detail == Detail::events) {
        write_events(report, result.events);
    } else if (detail == Detail::actions) {
        write_actions(report, result.events);
    }
    return report.take();
}

} // namespace gamebattle::report
