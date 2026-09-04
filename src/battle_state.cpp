#include "battle_runtime.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace gamebattle::runtime {
namespace {

inline constexpr std::size_t kAttributeCount =
    static_cast<std::size_t>(Attribute::damage_reduction_bp) + 1;

bool valid_trigger(Trigger trigger) {
    return static_cast<std::uint8_t>(trigger) <=
           static_cast<std::uint8_t>(Trigger::round_end);
}

std::int64_t saturating_multiply(std::int64_t left, std::int64_t right) {
    if (left == 0 || right == 0) {
        return 0;
    }
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    if (left > 0) {
        if (right > 0 && left > maximum / right) {
            return maximum;
        }
        if (right < 0 && right < minimum / left) {
            return minimum;
        }
    } else {
        if (right > 0 && left < minimum / right) {
            return minimum;
        }
        if (right < 0 && left < maximum / right) {
            return maximum;
        }
    }
    return left * right;
}

std::int64_t attribute_value(const Stats& stats, Attribute attribute) {
    switch (attribute) {
    case Attribute::attack: return stats.attack;
    case Attribute::defense: return stats.defense;
    case Attribute::speed: return stats.speed;
    case Attribute::crit_rate_bp: return stats.crit_rate_bp;
    case Attribute::crit_damage_bp: return stats.crit_damage_bp;
    case Attribute::hit_rate_bp: return stats.hit_rate_bp;
    case Attribute::dodge_rate_bp: return stats.dodge_rate_bp;
    case Attribute::damage_bonus_bp: return stats.damage_bonus_bp;
    case Attribute::damage_reduction_bp: return stats.damage_reduction_bp;
    }
    return 0;
}

void set_attribute_value(Stats& stats, Attribute attribute, std::int64_t value) {
    const auto basis_points_value = [&] {
        return static_cast<BasisPoints>(std::clamp<std::int64_t>(
            value, std::numeric_limits<BasisPoints>::min(),
            std::numeric_limits<BasisPoints>::max()));
    };
    switch (attribute) {
    case Attribute::attack:
        stats.attack = std::max<std::int64_t>(0, value);
        break;
    case Attribute::defense:
        stats.defense = std::max<std::int64_t>(0, value);
        break;
    case Attribute::speed:
        stats.speed = std::max<std::int64_t>(0, value);
        break;
    case Attribute::crit_rate_bp:
        stats.crit_rate_bp = basis_points_value();
        break;
    case Attribute::crit_damage_bp:
        stats.crit_damage_bp = basis_points_value();
        break;
    case Attribute::hit_rate_bp:
        stats.hit_rate_bp = basis_points_value();
        break;
    case Attribute::dodge_rate_bp:
        stats.dodge_rate_bp = basis_points_value();
        break;
    case Attribute::damage_bonus_bp:
        stats.damage_bonus_bp = basis_points_value();
        break;
    case Attribute::damage_reduction_bp:
        stats.damage_reduction_bp = basis_points_value();
        break;
    }
}

void validate_request(const BattleRequest& request) {
    if (request.max_rounds < 1 || request.max_rounds > 10000) {
        throw std::invalid_argument("max_rounds must be between 1 and 10000");
    }
    if (request.max_events < 100 || request.max_events > 1'000'000) {
        throw std::invalid_argument("max_events must be between 100 and 1000000");
    }
    if (request.attacker.units.empty() || request.defender.units.empty()) {
        throw std::invalid_argument("both formations must contain at least one unit");
    }
    if (request.attacker.units.size() > 256 || request.defender.units.size() > 256) {
        throw std::invalid_argument("a formation cannot contain more than 256 units");
    }

    const auto valid_probability = [](BasisPoints value) {
        return value >= 0 && value <= kBasisPoints;
    };
    const auto valid_modifier = [](std::int64_t value) {
        return value >= -1'000'000 && value <= 1'000'000;
    };
    const auto valid_flat = [](std::int64_t value) {
        return value >= -1'000'000'000'000LL && value <= 1'000'000'000'000LL;
    };
    std::unordered_map<std::uint32_t, const BuffSpec*> buff_definitions;
    std::unordered_set<const BuffSpec*> validating_buffs;
    std::unordered_set<const BuffSpec*> validated_buffs;
    std::function<void(const Effect&, std::size_t)> validate_effect;
    std::function<void(const std::shared_ptr<const BuffSpec>&, std::size_t)>
        validate_buff;

    validate_buff = [&](const std::shared_ptr<const BuffSpec>& definition,
                        std::size_t depth) {
        if (definition == nullptr) {
            throw std::invalid_argument("add_buff effect requires a buff definition");
        }
        if (depth > kMaxTriggerDepth) {
            throw std::invalid_argument("buff definition nesting is too deep");
        }
        const auto* buff = definition.get();
        if (validated_buffs.contains(buff)) {
            return;
        }
        if (validating_buffs.contains(buff)) {
            throw std::invalid_argument(
                "buff reaction add_buff graph contains an ownership cycle");
        }
        if (buff->id == 0 || buff->modifiers.size() > 256 ||
            buff->reactions.size() > 128 ||
            (!buff->lifetime.permanent &&
             (buff->lifetime.duration < 1 || buff->lifetime.duration > 10000)) ||
            (buff->lifetime.permanent && buff->lifetime.duration != 0) ||
            !valid_trigger(buff->lifetime.decrement_on) ||
            buff->stacking.max_stacks < 1 ||
            buff->stacking.max_stacks > 1000 ||
            static_cast<std::uint8_t>(buff->stacking.mode) >
                static_cast<std::uint8_t>(StackPolicy::refresh) ||
            static_cast<std::uint8_t>(buff->stacking.refresh) >
                static_cast<std::uint8_t>(RefreshPolicy::keep) ||
            (buff->stacking.mode == StackPolicy::refresh &&
             buff->stacking.max_stacks != 1)) {
            throw std::invalid_argument("buff policies are outside supported bounds");
        }
        const auto [known, inserted] = buff_definitions.emplace(buff->id, buff);
        if (!inserted && known->second != buff) {
            throw std::invalid_argument(
                "one battle request contains conflicting definitions for a buff id");
        }
        for (const auto& modifier : buff->modifiers) {
            if (static_cast<std::uint8_t>(modifier.attribute) >
                    static_cast<std::uint8_t>(Attribute::damage_reduction_bp) ||
                static_cast<std::uint8_t>(modifier.operation) >
                    static_cast<std::uint8_t>(ModifierOperation::scale_bp) ||
                (modifier.operation == ModifierOperation::add &&
                 !valid_flat(modifier.value)) ||
                (modifier.operation == ModifierOperation::scale_bp &&
                 !valid_modifier(modifier.value))) {
                throw std::invalid_argument(
                    "buff attribute modifier is outside supported bounds");
            }
        }

        validating_buffs.insert(buff);
        for (const auto& reaction : buff->reactions) {
            if (!valid_trigger(reaction.trigger) ||
                static_cast<std::uint8_t>(reaction.source) >
                    static_cast<std::uint8_t>(EffectSource::applier) ||
                static_cast<std::uint8_t>(reaction.stack_scaling) >
                    static_cast<std::uint8_t>(StackScaling::per_stack) ||
                !valid_probability(reaction.chance_bp) ||
                reaction.max_triggers_per_round < 0 ||
                reaction.max_triggers_per_round > 10000 ||
                reaction.effects.empty() || reaction.effects.size() > 64) {
                throw std::invalid_argument(
                    "buff reaction is outside supported bounds");
            }
            for (const auto& effect : reaction.effects) {
                validate_effect(effect, depth + 1);
            }
        }
        validating_buffs.erase(buff);
        validated_buffs.insert(buff);
    };

    validate_effect = [&](const Effect& effect, std::size_t depth) {
        if (effect.target_count < 1 || effect.target_count > 256 ||
            effect.attack_bp < 0 || effect.attack_bp > 1'000'000 ||
            !valid_flat(effect.flat) ||
            static_cast<std::uint8_t>(effect.kind) >
                static_cast<std::uint8_t>(EffectKind::direct_damage) ||
            static_cast<std::uint8_t>(effect.target) >
                static_cast<std::uint8_t>(TargetRule::all_allies)) {
            throw std::invalid_argument(
                "effect target_count, attack_bp, or flat value is outside supported bounds");
        }
        if (effect.kind == EffectKind::add_buff) {
            validate_buff(effect.buff, depth + 1);
        } else if (effect.buff != nullptr) {
            throw std::invalid_argument(
                "only add_buff effects may contain a buff definition");
        }
        if (effect.kind == EffectKind::remove_buff) {
            if (effect.remove_buff_id == 0) {
                throw std::invalid_argument(
                    "remove_buff effect requires a non-zero buff id");
            }
        } else if (effect.remove_buff_id != 0) {
            throw std::invalid_argument(
                "only remove_buff effects may contain a remove buff id");
        }
    };

    std::unordered_map<UnitId, const UnitConfig*> configs;
    for (const auto* formation : {&request.attacker, &request.defender}) {
        for (const auto& unit : formation->units) {
            if (unit.id == 0 || !configs.emplace(unit.id, &unit).second) {
                throw std::invalid_argument(
                    "unit ids must be non-zero and unique across both sides");
            }
            if (unit.position < 0 || unit.position > 1000) {
                throw std::invalid_argument("unit position must be between 0 and 1000");
            }
            if (unit.level < 1 || unit.level > 1'000'000) {
                throw std::invalid_argument("unit level must be between 1 and 1000000");
            }
            if (unit.final_stats.hp <= 0 || unit.final_stats.hp > 1'000'000'000'000LL ||
                unit.final_stats.attack < 0 || unit.final_stats.attack > 1'000'000'000'000LL ||
                unit.final_stats.defense < 0 || unit.final_stats.defense > 1'000'000'000'000LL ||
                unit.final_stats.speed < 0 || unit.final_stats.speed > 1'000'000'000'000LL) {
                throw std::invalid_argument(
                    "hp/attack/defense/speed attributes are outside supported bounds");
            }
            const auto& stats = unit.final_stats;
            if (!valid_probability(stats.crit_rate_bp) ||
                stats.crit_damage_bp < kBasisPoints || stats.crit_damage_bp > 1'000'000 ||
                stats.hit_rate_bp < 0 || stats.hit_rate_bp > 100'000 ||
                stats.dodge_rate_bp < 0 || stats.dodge_rate_bp > 100'000 ||
                !valid_modifier(stats.damage_bonus_bp) ||
                !valid_modifier(stats.damage_reduction_bp)) {
                throw std::invalid_argument("rate attributes are outside supported bounds");
            }
            if (unit.skills.size() > 128 || unit.passives.size() > 128) {
                throw std::invalid_argument(
                    "a unit cannot contain more than 128 skills or passives");
            }

            std::unordered_set<std::uint32_t> skill_ids;
            for (const auto& skill : unit.skills) {
                if (skill.id == 0 || !skill_ids.insert(skill.id).second ||
                    !valid_probability(skill.chance_bp) || skill.effects.empty() ||
                    skill.effects.size() > 64) {
                    throw std::invalid_argument(
                        "skill id, chance, or effect count is invalid");
                }
                for (const auto& effect : skill.effects) {
                    validate_effect(effect, 0);
                }
            }

            std::unordered_set<std::uint32_t> passive_ids;
            for (const auto& passive : unit.passives) {
                if (passive.id == 0 || !passive_ids.insert(passive.id).second ||
                    !valid_probability(passive.chance_bp) ||
                    passive.max_triggers_per_round < 0 ||
                    passive.max_triggers_per_round > 10000 || passive.effects.empty() ||
                    passive.effects.size() > 64) {
                    throw std::invalid_argument(
                        "passive id, chance, trigger limit, or effect count is invalid");
                }
                for (const auto& effect : passive.effects) {
                    validate_effect(effect, 0);
                }
            }
        }
    }

    if (request.initial_conditions.unit_states.size() > configs.size()) {
        throw std::invalid_argument(
            "initial_conditions contains more unit states than the formations");
    }
    std::unordered_set<UnitId> initialized;
    for (const auto& initial : request.initial_conditions.unit_states) {
        const auto config = configs.find(initial.unit_id);
        if (initial.unit_id == 0 || config == configs.end()) {
            throw std::invalid_argument(
                "initial_conditions references a unit that is not in either formation");
        }
        if (!initialized.insert(initial.unit_id).second) {
            throw std::invalid_argument(
                "initial_conditions contains duplicate unit ids");
        }
        if (initial.current_hp < 0 ||
            initial.current_hp > config->second->final_stats.hp) {
            throw std::invalid_argument(
                "initial current_hp must be between zero and the unit's final_stats.hp");
        }
    }
}

} // namespace

Side other(Side side) {
    return side == Side::attacker ? Side::defender : Side::attacker;
}

std::int64_t saturating_add(std::int64_t left, std::int64_t right) {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return left + right;
}

std::int64_t scale(std::int64_t value, std::int64_t basis_points) {
    return saturating_add(
        saturating_multiply(value / kBasisPoints, basis_points),
        saturating_multiply(value % kBasisPoints, basis_points) /
            kBasisPoints);
}

Random::Random(std::uint64_t seed) : state_(seed) {}

std::uint64_t Random::next() {
    state_ += 0x9e3779b97f4a7c15ULL;
    auto value = state_;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

bool Random::roll(BasisPoints chance_bp) {
    if (chance_bp <= 0) {
        return false;
    }
    if (chance_bp >= kBasisPoints) {
        return true;
    }
    return static_cast<std::int64_t>(next() % kBasisPoints) < chance_bp;
}

BattleState::BattleState(const BattleRequest& request_value)
    : request(request_value),
      random(request_value.seed),
      result{.battle_id = request_value.battle_id,
             .seed = request_value.seed,
             .source_battle_id = request_value.initial_conditions.source_battle_id} {
    validate_request(request);
    add_formation(request.attacker, Side::attacker);
    add_formation(request.defender, Side::defender);
    apply_initial_conditions();
}

void BattleState::add_formation(const Formation& formation, Side side) {
    for (const auto& source_config : formation.units) {
        RuntimeUnit runtime;
        runtime.config = source_config;
        std::stable_sort(runtime.config.skills.begin(), runtime.config.skills.end(),
                         [](const Skill& left, const Skill& right) {
                             return left.priority > right.priority;
                         });
        for (std::size_t index = 0; index < runtime.config.passives.size(); ++index) {
            const auto trigger = static_cast<std::size_t>(runtime.config.passives[index].trigger);
            runtime.passives_by_trigger.at(trigger).push_back(index);
        }
        runtime.side = side;
        runtime.initial_hp = source_config.final_stats.hp;
        runtime.hp = source_config.final_stats.hp;
        runtime.cached_stats = source_config.final_stats;
        unit_index.emplace(source_config.id, units.size());
        units.push_back(std::move(runtime));
    }
}

void BattleState::apply_initial_conditions() {
    for (const auto& initial : request.initial_conditions.unit_states) {
        const auto index = unit_index.at(initial.unit_id);
        units[index].initial_hp = initial.current_hp;
        units[index].hp = initial.current_hp;
    }
}

Stats BattleState::effective_stats(std::size_t unit_position) {
    auto& unit = units.at(unit_position);
    if (!unit.stats_dirty) {
        return unit.cached_stats;
    }

    Stats stats = unit.config.final_stats;
    std::array<std::int64_t, kAttributeCount> additions{};
    std::array<std::int64_t, kAttributeCount> scale_deltas{};
    for (const auto& active : unit.buffs) {
        if (active.definition == nullptr) {
            continue;
        }
        for (const auto& modifier : active.definition->modifiers) {
            const auto attribute = static_cast<std::size_t>(modifier.attribute);
            const auto value = saturating_multiply(modifier.value, active.stacks);
            auto& total = modifier.operation == ModifierOperation::add
                              ? additions.at(attribute)
                              : scale_deltas.at(attribute);
            total = saturating_add(total, value);
        }
    }
    for (std::size_t index = 0; index < kAttributeCount; ++index) {
        const auto attribute = static_cast<Attribute>(index);
        const auto after_add = saturating_add(
            attribute_value(stats, attribute), additions[index]);
        const auto after_scale = scale(
            after_add, saturating_add(kBasisPoints, scale_deltas[index]));
        set_attribute_value(stats, attribute, after_scale);
    }
    unit.cached_stats = stats;
    unit.stats_dirty = false;
    return stats;
}

void BattleState::mark_stats_dirty(std::size_t unit_position) {
    units.at(unit_position).stats_dirty = true;
}

std::uint64_t BattleState::initiative(Side side, std::int64_t bonus) const {
    std::int64_t total = bonus;
    for (const auto& unit : units) {
        if (unit.side == side && unit.alive() && unit.config.can_act) {
            total = saturating_add(total, unit.config.final_stats.speed);
        }
    }
    return static_cast<std::uint64_t>(std::max<std::int64_t>(0, total));
}

std::vector<std::size_t> BattleState::acting_order(Side side) {
    std::vector<std::size_t> order;
    for (std::size_t index = 0; index < units.size(); ++index) {
        const auto& unit = units[index];
        if (unit.side == side && unit.alive() && unit.config.can_act) {
            order.push_back(index);
        }
    }
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        const auto left_speed = effective_stats(left).speed;
        const auto right_speed = effective_stats(right).speed;
        if (left_speed != right_speed) {
            return left_speed > right_speed;
        }
        if (units[left].config.position != units[right].config.position) {
            return units[left].config.position < units[right].config.position;
        }
        return units[left].config.id < units[right].config.id;
    });
    return order;
}

std::optional<std::size_t> BattleState::find_unit(UnitId id) const {
    const auto found = unit_index.find(id);
    return found == unit_index.end() ? std::nullopt
                                     : std::optional<std::size_t>(found->second);
}

bool BattleState::side_defeated(Side side) const {
    bool has_hero = false;
    bool living_hero = false;
    bool living_actor = false;
    for (const auto& unit : units) {
        if (unit.side != side) {
            continue;
        }
        if (unit.config.kind == UnitKind::hero) {
            has_hero = true;
            living_hero = living_hero || unit.alive();
        }
        living_actor = living_actor || (unit.alive() && unit.config.can_act);
    }
    return has_hero ? !living_hero : !living_actor;
}

bool BattleState::finish_if_decided(std::string reason) {
    const bool attacker_dead = side_defeated(Side::attacker);
    const bool defender_dead = side_defeated(Side::defender);
    if (!attacker_dead && !defender_dead) {
        return false;
    }
    decided = true;
    result.winner = attacker_dead == defender_dead
                        ? Winner::draw
                        : (defender_dead ? Winner::attacker : Winner::defender);
    result.reason = std::move(reason);
    result.rounds = round;
    return true;
}

void BattleState::reset_round_trigger_counts() {
    for (auto& unit : units) {
        unit.passive_triggers.clear();
        for (auto& buff : unit.buffs) {
            std::fill(buff.reaction_triggers.begin(),
                      buff.reaction_triggers.end(), 0);
        }
    }
}

void BattleState::emit(std::string event_phase, std::string type, Side side,
                       UnitId actor, UnitId target, std::uint32_t source_id,
                       std::int64_t value, std::int64_t hp_before,
                       std::int64_t hp_after, bool critical) {
    if (result.events.size() >= static_cast<std::size_t>(request.max_events)) {
        event_limit = true;
        return;
    }
    result.events.push_back(Event{
        .seq = static_cast<std::uint32_t>(result.events.size() + 1),
        .round = round,
        .phase = std::move(event_phase),
        .type = std::move(type),
        .side = side,
        .actor = actor,
        .target = target,
        .source_id = source_id,
        .value = value,
        .hp_before = hp_before,
        .hp_after = hp_after,
        .critical = critical
    });
}

BattleResult BattleState::finish() {
    result.units.reserve(units.size());
    for (const auto& unit : units) {
        result.units.push_back(UnitResult{
            .id = unit.config.id,
            .side = unit.side,
            .initial_hp = unit.initial_hp,
            .hp = unit.hp,
            .max_hp = unit.config.final_stats.hp,
            .alive = unit.alive()
        });
    }
    return std::move(result);
}

} // namespace gamebattle::runtime
