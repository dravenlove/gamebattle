#include "gamebattle/wire.hpp"

#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gamebattle::wire {
namespace {

using term::Value;

constexpr std::size_t kMaxEmbeddedSpecDepth = 32;

template <typename T>
T checked_int(std::int64_t value, std::string_view path) {
    if (value < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
        value > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
        throw term::DecodeError(std::string(path) + " is outside the supported integer range");
    }
    return static_cast<T>(value);
}

std::uint64_t nonnegative_u64(std::int64_t value, std::string_view path) {
    if (value < 0) {
        throw term::DecodeError(std::string(path) + " must not be negative");
    }
    return static_cast<std::uint64_t>(value);
}

UnitKind parse_kind(std::string_view value) {
    if (value == "hero") return UnitKind::hero;
    if (value == "beauty") return UnitKind::beauty;
    if (value == "pet") return UnitKind::pet;
    if (value == "artifact" || value == "divine_weapon") return UnitKind::artifact;
    throw term::DecodeError("unit.kind must be hero, beauty, pet, or artifact");
}

TargetRule parse_target(std::string_view value) {
    if (value == "self") return TargetRule::self;
    if (value == "trigger_unit") return TargetRule::trigger_unit;
    if (value == "enemy_front") return TargetRule::enemy_front;
    if (value == "enemy_lowest_hp") return TargetRule::enemy_lowest_hp;
    if (value == "ally_lowest_hp") return TargetRule::ally_lowest_hp;
    if (value == "all_enemies") return TargetRule::all_enemies;
    if (value == "all_allies") return TargetRule::all_allies;
    throw term::DecodeError("effect.target has an unsupported value");
}

Trigger parse_trigger(std::string_view value) {
    if (value == "battle_start") return Trigger::battle_start;
    if (value == "round_start") return Trigger::round_start;
    if (value == "before_action") return Trigger::before_action;
    if (value == "on_attack") return Trigger::on_attack;
    if (value == "on_hit") return Trigger::on_hit;
    if (value == "on_damaged") return Trigger::on_damaged;
    if (value == "unit_death") return Trigger::unit_death;
    if (value == "after_action") return Trigger::after_action;
    if (value == "round_end") return Trigger::round_end;
    throw term::DecodeError("passive.trigger has an unsupported value");
}

EffectKind parse_effect_kind(std::string_view value) {
    if (value == "damage") return EffectKind::damage;
    if (value == "heal") return EffectKind::heal;
    if (value == "add_buff") return EffectKind::add_buff;
    if (value == "remove_buff") return EffectKind::remove_buff;
    if (value == "direct_damage") return EffectKind::direct_damage;
    throw term::DecodeError(
        "effect.type must be damage, direct_damage, heal, add_buff, or remove_buff");
}

Attribute parse_attribute(std::string_view value) {
    if (value == "attack") return Attribute::attack;
    if (value == "defense") return Attribute::defense;
    if (value == "speed") return Attribute::speed;
    if (value == "crit_rate_bp") return Attribute::crit_rate_bp;
    if (value == "crit_damage_bp") return Attribute::crit_damage_bp;
    if (value == "hit_rate_bp") return Attribute::hit_rate_bp;
    if (value == "dodge_rate_bp") return Attribute::dodge_rate_bp;
    if (value == "damage_bonus_bp") return Attribute::damage_bonus_bp;
    if (value == "damage_reduction_bp") return Attribute::damage_reduction_bp;
    throw term::DecodeError("buff.modifier.attribute has an unsupported value");
}

ModifierOperation parse_modifier_operation(std::string_view value) {
    if (value == "add") return ModifierOperation::add;
    if (value == "scale_bp") return ModifierOperation::scale_bp;
    throw term::DecodeError(
        "buff.modifier.operation must be add or scale_bp");
}

StackPolicy parse_stack_policy(std::string_view value) {
    if (value == "stack") return StackPolicy::stack;
    if (value == "refresh") return StackPolicy::refresh;
    throw term::DecodeError("buff.stacking.policy must be stack or refresh");
}

RefreshPolicy parse_refresh_policy(std::string_view value) {
    if (value == "reset") return RefreshPolicy::reset;
    if (value == "extend") return RefreshPolicy::extend;
    if (value == "keep") return RefreshPolicy::keep;
    throw term::DecodeError(
        "buff.stacking.refresh must be reset, extend, or keep");
}

EffectSource parse_effect_source(std::string_view value) {
    if (value == "owner") return EffectSource::owner;
    if (value == "applier") return EffectSource::applier;
    throw term::DecodeError("buff.reaction.source must be owner or applier");
}

StackScaling parse_stack_scaling(std::string_view value) {
    if (value == "once") return StackScaling::once;
    if (value == "per_stack") return StackScaling::per_stack;
    throw term::DecodeError(
        "buff.reaction.stack_scaling must be once or per_stack");
}

StackKeyPolicy parse_stack_key_policy(std::string_view value) {
    if (value == "by_buff") return StackKeyPolicy::by_buff;
    if (value == "by_buff_and_source") {
        return StackKeyPolicy::by_buff_and_source;
    }
    throw term::DecodeError(
        "buff.stacking.key must be by_buff or by_buff_and_source");
}

EventLogLevel parse_event_log_level(std::string_view value) {
    if (value == "result_only") return EventLogLevel::result_only;
    if (value == "summary") return EventLogLevel::summary;
    if (value == "full") return EventLogLevel::full;
    throw term::DecodeError(
        "log_level must be result_only, summary, or full");
}

BasisPoints basis_points(const Value& object, std::string_view key, BasisPoints default_value) {
    return checked_int<BasisPoints>(term::get_int(object, key, default_value), key);
}

void require_only_fields(
    const Value& value,
    std::string_view path,
    std::initializer_list<std::string_view> supported) {
    for (const auto& [key, unused] : term::as_object(value, path)) {
        static_cast<void>(unused);
        bool found = false;
        for (const auto candidate : supported) {
            if (key == candidate) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw term::DecodeError(
                std::string(path) + " contains unsupported field '" + key + "'");
        }
    }
}

const Value& require_field(
    const Value& value,
    std::string_view key,
    std::string_view path) {
    const auto* field = term::find(value, key);
    if (field == nullptr) {
        throw term::DecodeError(
            std::string(path) + " requires field '" + std::string(key) + "'");
    }
    return *field;
}

void check_embedded_depth(std::size_t depth) {
    if (depth > kMaxEmbeddedSpecDepth) {
        throw term::DecodeError(
            "embedded buff/effect nesting exceeds the supported depth of 32");
    }
}

Effect parse_effect(const Value& value, std::size_t depth);

std::vector<Effect> parse_effects(const Value& object, std::size_t depth) {
    check_embedded_depth(depth);
    const auto* effects = term::find(object, "effects");
    if (effects == nullptr) {
        return {};
    }
    std::vector<Effect> result;
    for (const auto& effect : term::as_list(*effects, "effects")) {
        result.push_back(parse_effect(effect, depth));
    }
    return result;
}

AttributeModifier parse_modifier(const Value& value) {
    require_only_fields(
        value, "buff.modifier", {"attribute", "operation", "value"});
    const auto* attribute = term::find(value, "attribute");
    const auto* operation = term::find(value, "operation");
    const auto* amount = term::find(value, "value");
    if (attribute == nullptr || operation == nullptr || amount == nullptr) {
        throw term::DecodeError(
            "each buff modifier requires attribute, operation, and value");
    }
    return AttributeModifier{
        .attribute = parse_attribute(
            term::as_string(*attribute, "buff.modifier.attribute")),
        .operation = parse_modifier_operation(
            term::as_string(*operation, "buff.modifier.operation")),
        .value = term::as_int(*amount, "buff.modifier.value")
    };
}

BuffSpec parse_buff(const Value& value, std::size_t depth) {
    check_embedded_depth(depth);
    require_only_fields(
        value, "buff",
        {"id", "name", "lifetime", "stacking", "modifiers", "reactions"});
    BuffSpec buff;
    buff.id = checked_int<std::uint32_t>(
        term::as_int(require_field(value, "id", "buff"), "buff.id"),
        "buff.id");
    buff.name = term::as_string(
        require_field(value, "name", "buff"), "buff.name");

    const auto& lifetime = require_field(value, "lifetime", "buff");
    require_only_fields(
        lifetime, "buff.lifetime", {"type", "duration", "decrement_on"});
    const auto type = term::as_string(
        require_field(lifetime, "type", "buff.lifetime"),
        "buff.lifetime.type");
    if (type == "finite") {
        buff.lifetime.permanent = false;
    } else if (type == "permanent") {
        buff.lifetime.permanent = true;
    } else {
        throw term::DecodeError(
            "buff.lifetime.type must be finite or permanent");
    }
    buff.lifetime.duration = checked_int<std::int32_t>(
        term::as_int(
            require_field(lifetime, "duration", "buff.lifetime"),
            "buff.lifetime.duration"),
        "buff.lifetime.duration");
    buff.lifetime.decrement_on = parse_trigger(term::as_string(
        require_field(lifetime, "decrement_on", "buff.lifetime"),
        "buff.lifetime.decrement_on"));
    if ((buff.lifetime.permanent && buff.lifetime.duration != 0) ||
        (!buff.lifetime.permanent && buff.lifetime.duration < 1)) {
        throw term::DecodeError(
            "permanent buffs require duration 0; finite buffs require duration >= 1");
    }

    const auto& stacking = require_field(value, "stacking", "buff");
    require_only_fields(
        stacking, "buff.stacking",
        {"max_stacks", "policy", "refresh", "key"});
    buff.stacking.max_stacks = checked_int<std::int32_t>(
        term::as_int(
            require_field(stacking, "max_stacks", "buff.stacking"),
            "buff.stacking.max_stacks"),
        "buff.stacking.max_stacks");
    buff.stacking.mode = parse_stack_policy(term::as_string(
        require_field(stacking, "policy", "buff.stacking"),
        "buff.stacking.policy"));
    buff.stacking.refresh = parse_refresh_policy(term::as_string(
        require_field(stacking, "refresh", "buff.stacking"),
        "buff.stacking.refresh"));
    buff.stacking.key = parse_stack_key_policy(term::as_string(
        require_field(stacking, "key", "buff.stacking"),
        "buff.stacking.key"));
    if (buff.stacking.mode == StackPolicy::refresh &&
        buff.stacking.max_stacks != 1) {
        throw term::DecodeError(
            "buff.stacking policy refresh requires max_stacks 1");
    }

    const auto& modifiers = require_field(value, "modifiers", "buff");
    for (const auto& modifier : term::as_list(modifiers, "buff.modifiers")) {
        buff.modifiers.push_back(parse_modifier(modifier));
    }

    const auto& reactions = require_field(value, "reactions", "buff");
    for (const auto& item : term::as_list(reactions, "buff.reactions")) {
        require_only_fields(
            item, "buff.reaction",
            {"trigger", "source", "stack_scaling", "chance_bp",
             "max_triggers_per_round", "priority", "effects"});
        BuffReaction reaction;
        reaction.trigger = parse_trigger(
            term::get_string(item, "trigger", "round_end"));
        reaction.priority = checked_int<std::int32_t>(
            term::as_int(
                require_field(item, "priority", "buff.reaction"),
                "buff.reaction.priority"),
            "buff.reaction.priority");
        reaction.source = parse_effect_source(
            term::get_string(item, "source", "owner"));
        reaction.stack_scaling = parse_stack_scaling(
            term::get_string(item, "stack_scaling", "once"));
        reaction.chance_bp = basis_points(item, "chance_bp", 10000);
        reaction.max_triggers_per_round = checked_int<std::int32_t>(
            term::get_int(item, "max_triggers_per_round"),
            "buff.reaction.max_triggers_per_round");
        reaction.effects = parse_effects(item, depth + 1);
        if (reaction.effects.empty()) {
            throw term::DecodeError(
                "each buff reaction must contain at least one effect");
        }
        buff.reactions.push_back(std::move(reaction));
    }
    return buff;
}

Effect parse_effect(const Value& value, std::size_t depth) {
    check_embedded_depth(depth);
    term::as_object(value, "effect");
    Effect effect;
    effect.kind = parse_effect_kind(term::get_string(value, "type", "damage"));
    const std::string default_target = effect.kind == EffectKind::heal ? "ally_lowest_hp" : "enemy_front";
    effect.target = parse_target(term::get_string(value, "target", default_target));
    effect.target_count = checked_int<std::int32_t>(term::get_int(value, "target_count", 1),
                                                    "effect.target_count");
    effect.attack_bp = basis_points(value, "attack_bp", effect.kind == EffectKind::damage ? 10000 : 0);
    effect.flat = term::get_int(value, "flat");
    if (effect.kind == EffectKind::add_buff) {
        const auto* buff = term::find(value, "buff");
        if (buff == nullptr) {
            throw term::DecodeError("add_buff effect requires a buff map");
        }
        effect.buff = std::make_shared<BuffSpec>(parse_buff(*buff, depth + 1));
    } else if (effect.kind == EffectKind::remove_buff) {
        effect.remove_buff_id = checked_int<std::uint32_t>(
            term::get_int(value, "buff_id"), "effect.buff_id");
    }
    return effect;
}

Skill parse_skill(const Value& value) {
    term::as_object(value, "skill");
    Skill skill;
    skill.id = checked_int<std::uint32_t>(term::get_int(value, "id"), "skill.id");
    skill.name = term::get_string(value, "name");
    skill.chance_bp = basis_points(value, "chance_bp", 10000);
    skill.priority = checked_int<std::int32_t>(term::get_int(value, "priority"), "skill.priority");
    skill.effects = parse_effects(value, 0);
    if (skill.effects.empty()) {
        throw term::DecodeError("each skill must contain at least one effect");
    }
    return skill;
}

Passive parse_passive(const Value& value) {
    require_only_fields(
        value, "passive",
        {"id", "name", "trigger", "priority", "chance_bp",
         "max_triggers_per_round", "effects"});
    Passive passive;
    passive.id = checked_int<std::uint32_t>(term::get_int(value, "id"), "passive.id");
    passive.name = term::get_string(value, "name");
    passive.trigger = parse_trigger(term::get_string(value, "trigger", "on_damaged"));
    passive.priority = checked_int<std::int32_t>(
        term::as_int(
            require_field(value, "priority", "passive"),
            "passive.priority"),
        "passive.priority");
    passive.chance_bp = basis_points(value, "chance_bp", 10000);
    passive.max_triggers_per_round = checked_int<std::int32_t>(
        term::get_int(value, "max_triggers_per_round"), "passive.max_triggers_per_round");
    passive.effects = parse_effects(value, 0);
    if (passive.effects.empty()) {
        throw term::DecodeError("each passive must contain at least one effect");
    }
    return passive;
}

Stats parse_stats(const Value& value) {
    term::as_object(value, "final_stats");
    Stats stats;
    const auto* hp = term::find(value, "hp");
    const auto* attack = term::find(value, "attack");
    const auto* defense = term::find(value, "defense");
    const auto* speed = term::find(value, "speed");
    if (hp == nullptr || attack == nullptr || defense == nullptr || speed == nullptr) {
        throw term::DecodeError("final_stats requires hp, attack, defense, and speed");
    }
    stats.hp = term::as_int(*hp, "final_stats.hp");
    stats.attack = term::as_int(*attack, "final_stats.attack");
    stats.defense = term::as_int(*defense, "final_stats.defense");
    stats.speed = term::as_int(*speed, "final_stats.speed");
    stats.crit_rate_bp = basis_points(value, "crit_rate_bp", 0);
    stats.crit_damage_bp = basis_points(value, "crit_damage_bp", 15000);
    stats.hit_rate_bp = basis_points(value, "hit_rate_bp", 10000);
    stats.dodge_rate_bp = basis_points(value, "dodge_rate_bp", 0);
    stats.damage_bonus_bp = basis_points(value, "damage_bonus_bp", 0);
    stats.damage_reduction_bp = basis_points(value, "damage_reduction_bp", 0);
    return stats;
}

std::vector<std::uint32_t> parse_config_ids(const Value& object,
                                            std::string_view key) {
    std::vector<std::uint32_t> result;
    const auto* configured = term::find(object, key);
    if (configured == nullptr) {
        return result;
    }
    for (const auto& item : term::as_list(*configured, key)) {
        result.push_back(checked_int<std::uint32_t>(term::as_int(item, key), key));
    }
    return result;
}

UnitConfig parse_unit(const Value& value, const ConfigStore* configs) {
    term::as_object(value, "unit");
    UnitConfig unit;
    unit.id = nonnegative_u64(term::get_int(value, "id"), "unit.id");
    unit.kind = parse_kind(term::get_string(value, "kind", "hero"));
    unit.position = checked_int<std::int32_t>(term::get_int(value, "position"), "unit.position");
    unit.level = checked_int<std::int32_t>(term::get_int(value, "level", 1), "unit.level");
    const bool hero_defaults = unit.kind == UnitKind::hero;
    unit.can_act = term::get_bool(value, "can_act", hero_defaults);
    unit.targetable = term::get_bool(value, "targetable", hero_defaults);

    if (const auto* growth = term::find(value, "growth_levels")) {
        for (const auto& [key, item] : term::as_object(*growth, "growth_levels")) {
            unit.growth_levels.emplace(key, checked_int<std::int32_t>(term::as_int(item, key), key));
        }
    }
    const auto* stats = term::find(value, "final_stats");
    if (stats == nullptr) {
        throw term::DecodeError("unit.final_stats is required");
    }
    unit.final_stats = parse_stats(*stats);

    const auto* embedded_skills = term::find(value, "skills");
    const auto* configured_skills = term::find(value, "skill_ids");
    if (embedded_skills != nullptr && configured_skills != nullptr) {
        throw term::DecodeError("unit cannot contain both skills and skill_ids");
    }
    if (embedded_skills != nullptr) {
        for (const auto& skill : term::as_list(*embedded_skills, "skills")) {
            unit.skills.push_back(parse_skill(skill));
        }
    }
    const auto* embedded_passives = term::find(value, "passives");
    const auto* configured_passives = term::find(value, "passive_ids");
    if (embedded_passives != nullptr && configured_passives != nullptr) {
        throw term::DecodeError("unit cannot contain both passives and passive_ids");
    }
    if (embedded_passives != nullptr) {
        for (const auto& passive : term::as_list(*embedded_passives, "passives")) {
            unit.passives.push_back(parse_passive(passive));
        }
    }

    if (configured_skills != nullptr || configured_passives != nullptr) {
        if (configs == nullptr) {
            throw term::DecodeError(
                "skill_ids/passive_ids require a loaded battle config pack");
        }
        try {
            if (configured_skills != nullptr) {
                for (const auto id : parse_config_ids(value, "skill_ids")) {
                    unit.skills.push_back(configs->require_skill(id));
                }
            }
            if (configured_passives != nullptr) {
                for (const auto id : parse_config_ids(value, "passive_ids")) {
                    unit.passives.push_back(configs->require_passive(id));
                }
            }
        } catch (const std::out_of_range& error) {
            throw term::DecodeError(std::string("unit loadout: ") + error.what());
        }
    }
    return unit;
}

Formation parse_formation(const Value& value, std::string_view path,
                          const ConfigStore* configs) {
    term::as_object(value, path);
    Formation formation;
    formation.name = term::get_string(value, "formation", "default");
    formation.initiative_bonus = term::get_int(value, "initiative_bonus");
    const auto* units = term::find(value, "units");
    if (units == nullptr) {
        throw term::DecodeError(std::string(path) + ".units is required");
    }
    for (const auto& unit : term::as_list(*units, "units")) {
        formation.units.push_back(parse_unit(unit, configs));
    }
    return formation;
}

BattleInitialConditions parse_initial_conditions(const Value& value) {
    term::as_object(value, "initial_conditions");
    BattleInitialConditions conditions;
    conditions.source_battle_id = nonnegative_u64(
        term::get_int(value, "source_battle_id"),
        "initial_conditions.source_battle_id");

    const auto first_side = term::get_string(value, "first_side", "automatic");
    if (first_side == "attacker") {
        conditions.forced_first_side = Side::attacker;
    } else if (first_side == "defender") {
        conditions.forced_first_side = Side::defender;
    } else if (first_side != "automatic") {
        throw term::DecodeError(
            "initial_conditions.first_side must be automatic, attacker, or defender");
    }

    if (const auto* states = term::find(value, "unit_states")) {
        for (const auto& item : term::as_list(*states, "initial_conditions.unit_states")) {
            term::as_object(item, "initial_conditions.unit_state");
            const auto* unit_id = term::find(item, "unit_id");
            const auto* current_hp = term::find(item, "current_hp");
            if (unit_id == nullptr || current_hp == nullptr) {
                throw term::DecodeError(
                    "each initial unit state requires unit_id and current_hp");
            }
            conditions.unit_states.push_back(UnitInitialState{
                .unit_id = nonnegative_u64(
                    term::as_int(*unit_id, "initial_conditions.unit_id"),
                    "initial_conditions.unit_id"),
                .current_hp = term::as_int(
                    *current_hp, "initial_conditions.current_hp")
            });
        }
    }
    return conditions;
}

Value integer(std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("result integer exceeds signed 64-bit ETF adapter limit");
    }
    return Value(static_cast<std::int64_t>(value));
}

std::string side_name(Side side) { return side == Side::attacker ? "attacker" : "defender"; }

std::string winner_name(Winner winner) {
    switch (winner) {
    case Winner::attacker: return "attacker";
    case Winner::defender: return "defender";
    case Winner::draw: return "draw";
    }
    return "draw";
}

Value error_value(std::string type, std::string message) {
    return Value::tuple({
        Value::atom("error"),
        Value::object({
            {"type", Value::atom(std::move(type))},
            {"message", Value::binary(message)}
        })
    });
}

std::filesystem::path path_from_utf8(std::string_view text) {
    const auto* begin = reinterpret_cast<const char8_t*>(text.data());
    return std::filesystem::path(std::u8string(begin, begin + text.size()));
}

} // namespace

BattleRequest parse_request(const Value& value, const ConfigStore* configs) {
    require_only_fields(
        value, "request",
        {"battle_id", "seed", "max_rounds", "max_execution_steps",
         "max_logged_events", "log_level", "attacker", "defender",
         "initial_conditions"});
    BattleRequest request;
    request.battle_id = nonnegative_u64(term::get_int(value, "battle_id"), "battle_id");
    request.seed = nonnegative_u64(term::get_int(value, "seed", 1), "seed");
    request.max_rounds = checked_int<std::int32_t>(term::get_int(value, "max_rounds", 50),
                                                   "max_rounds");
    request.max_execution_steps = checked_int<std::int32_t>(
        term::get_int(value, "max_execution_steps", 100000),
        "max_execution_steps");
    request.max_logged_events = checked_int<std::int32_t>(
        term::get_int(value, "max_logged_events", 10000),
        "max_logged_events");
    request.event_log_level = parse_event_log_level(
        term::get_string(value, "log_level", "full"));
    const auto* attacker = term::find(value, "attacker");
    const auto* defender = term::find(value, "defender");
    if (attacker == nullptr || defender == nullptr) {
        throw term::DecodeError("request requires attacker and defender formation maps");
    }
    request.attacker = parse_formation(*attacker, "attacker", configs);
    request.defender = parse_formation(*defender, "defender", configs);
    if (const auto* initial = term::find(value, "initial_conditions")) {
        request.initial_conditions = parse_initial_conditions(*initial);
    }
    return request;
}

Value encode_result(const BattleResult& result) {
    Value::List events;
    events.reserve(result.events.size());
    for (const auto& event : result.events) {
        events.push_back(Value::object({
            {"seq", Value(static_cast<std::int64_t>(event.seq))},
            {"event_id", integer(event.event_id)},
            {"parent_event_id", integer(event.parent_event_id)},
            {"depth", Value(static_cast<std::int64_t>(event.depth))},
            {"round", Value(static_cast<std::int64_t>(event.round))},
            {"phase", Value::atom(event.phase)},
            {"type", Value::atom(event.type)},
            {"side", Value::atom(side_name(event.side))},
            {"actor", integer(event.actor)},
            {"target", integer(event.target)},
            {"source_id", Value(static_cast<std::int64_t>(event.source_id))},
            {"value", Value(event.value)},
            {"hp_before", Value(event.hp_before)},
            {"hp_after", Value(event.hp_after)},
            {"critical", Value::atom(event.critical ? "true" : "false")}
        }));
    }

    Value::List units;
    units.reserve(result.units.size());
    for (const auto& unit : result.units) {
        units.push_back(Value::object({
            {"id", integer(unit.id)},
            {"side", Value::atom(side_name(unit.side))},
            {"initial_hp", Value(unit.initial_hp)},
            {"hp", Value(unit.hp)},
            {"max_hp", Value(unit.max_hp)},
            {"alive", Value::atom(unit.alive ? "true" : "false")}
        }));
    }

    return Value::object({
        {"battle_id", integer(result.battle_id)},
        {"seed", integer(result.seed)},
        {"source_battle_id", integer(result.source_battle_id)},
        {"winner", Value::atom(winner_name(result.winner))},
        {"reason", Value::atom(result.reason)},
        {"rounds", Value(static_cast<std::int64_t>(result.rounds))},
        {"attacker_initiative", integer(result.attacker_initiative)},
        {"defender_initiative", integer(result.defender_initiative)},
        {"execution_steps", integer(result.execution_steps)},
        {"total_event_count", integer(result.total_event_count)},
        {"logged_event_count", integer(result.logged_event_count)},
        {"events_truncated", Value::atom(
            result.events_truncated ? "true" : "false")},
        {"events", Value::list(std::move(events))},
        {"units", Value::list(std::move(units))}
    });
}

std::vector<std::uint8_t> Handler::handle_etf(
    std::span<const std::uint8_t> request) {
    try {
        const Value decoded = term::decode(request);
        if (const auto* atom = std::get_if<term::Atom>(&decoded.data); atom != nullptr && atom->value == "ping") {
            return term::encode(Value::tuple({Value::atom("ok"), Value::atom("pong")}));
        }
        if (const auto* tuple = std::get_if<Value::TupleValue>(&decoded.data);
            tuple != nullptr && tuple->value.size() == 2 &&
            term::as_string(tuple->value[0], "command") == "load_config") {
            try {
                const auto path_text = term::as_string(tuple->value[1], "config_path");
                auto next = std::make_shared<ConfigStore>(
                    ConfigStore::load_file(path_from_utf8(path_text)));
                {
                    std::unique_lock lock(config_mutex_);
                    configs_ = next;
                }
                return term::encode(Value::tuple({
                    Value::atom("ok"),
                    Value::object({
                        {"format_major", Value(static_cast<std::int64_t>(ConfigStore::format_major))},
                        {"format_minor", Value(static_cast<std::int64_t>(ConfigStore::format_minor))},
                        {"buffs", Value(static_cast<std::int64_t>(next->buff_count()))},
                        {"effects", Value(static_cast<std::int64_t>(next->effect_count()))},
                        {"skills", Value(static_cast<std::int64_t>(next->skill_count()))},
                        {"passives", Value(static_cast<std::int64_t>(next->passive_count()))}
                    })
                }));
            } catch (const std::exception& error) {
                return term::encode(error_value("config_load_failed", error.what()));
            }
        }
        std::shared_ptr<const ConfigStore> configs;
        {
            std::shared_lock lock(config_mutex_);
            configs = configs_;
        }
        const BattleRequest battle = parse_request(decoded, configs.get());
        const BattleResult result = Engine{}.simulate(battle);
        return term::encode(Value::tuple({Value::atom("ok"), encode_result(result)}));
    } catch (const term::DecodeError& error) {
        return term::encode(error_value("invalid_request", error.what()));
    } catch (const std::invalid_argument& error) {
        return term::encode(error_value("invalid_request", error.what()));
    } catch (const std::exception& error) {
        return term::encode(error_value("internal_error", error.what()));
    } catch (...) {
        return term::encode(error_value("internal_error", "unknown C++ exception"));
    }
}

std::vector<std::uint8_t> handle_etf(std::span<const std::uint8_t> request) {
    static Handler handler;
    return handler.handle_etf(request);
}

} // namespace gamebattle::wire
