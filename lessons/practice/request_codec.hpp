#pragma once

// Inverse of wire::parse_request: turns a BattleRequest into the ETF map an
// Erlang caller would send, so practice programs can drive the full Port path.

#include "gamebattle/engine.hpp"
#include "gamebattle/term.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace practice {

using gamebattle::term::Value;

inline Value int_value(std::int64_t value) { return Value(value); }
inline Value bool_value(bool value) { return Value::atom(value ? "true" : "false"); }

inline const char* trigger_name(gamebattle::Trigger trigger) {
    using gamebattle::Trigger;
    switch (trigger) {
    case Trigger::battle_start: return "battle_start";
    case Trigger::round_start: return "round_start";
    case Trigger::before_action: return "before_action";
    case Trigger::on_attack: return "on_attack";
    case Trigger::on_hit: return "on_hit";
    case Trigger::on_damaged: return "on_damaged";
    case Trigger::unit_death: return "unit_death";
    case Trigger::after_action: return "after_action";
    case Trigger::round_end: return "round_end";
    case Trigger::enemy_activate: return "enemy_activate";
    case Trigger::ally_activate: return "ally_activate";
    }
    return "round_end";
}

inline const char* target_name(gamebattle::TargetRule rule) {
    using gamebattle::TargetRule;
    switch (rule) {
    case TargetRule::self: return "self";
    case TargetRule::trigger_unit: return "trigger_unit";
    case TargetRule::enemy_front: return "enemy_front";
    case TargetRule::enemy_lowest_hp: return "enemy_lowest_hp";
    case TargetRule::ally_lowest_hp: return "ally_lowest_hp";
    case TargetRule::all_enemies: return "all_enemies";
    case TargetRule::all_allies: return "all_allies";
    }
    return "enemy_front";
}

inline const char* effect_kind_name(gamebattle::EffectKind kind) {
    using gamebattle::EffectKind;
    switch (kind) {
    case EffectKind::damage: return "damage";
    case EffectKind::heal: return "heal";
    case EffectKind::add_buff: return "add_buff";
    case EffectKind::remove_buff: return "remove_buff";
    case EffectKind::direct_damage: return "direct_damage";
    case EffectKind::negate: return "negate";
    }
    return "damage";
}

inline const char* attribute_name(gamebattle::Attribute attribute) {
    using gamebattle::Attribute;
    switch (attribute) {
    case Attribute::attack: return "attack";
    case Attribute::defense: return "defense";
    case Attribute::speed: return "speed";
    case Attribute::crit_rate_bp: return "crit_rate_bp";
    case Attribute::crit_damage_bp: return "crit_damage_bp";
    case Attribute::hit_rate_bp: return "hit_rate_bp";
    case Attribute::dodge_rate_bp: return "dodge_rate_bp";
    case Attribute::damage_bonus_bp: return "damage_bonus_bp";
    case Attribute::damage_reduction_bp: return "damage_reduction_bp";
    }
    return "attack";
}

inline const char* unit_kind_name(gamebattle::UnitKind kind) {
    using gamebattle::UnitKind;
    switch (kind) {
    case UnitKind::hero: return "hero";
    case UnitKind::beauty: return "beauty";
    case UnitKind::pet: return "pet";
    case UnitKind::artifact: return "artifact";
    }
    return "hero";
}

inline Value encode_effects(const std::vector<gamebattle::Effect>& effects);

inline Value encode_buff(const gamebattle::BuffSpec& buff) {
    Value::List modifiers;
    for (const auto& modifier : buff.modifiers) {
        modifiers.push_back(Value::object({
            {"attribute", Value::atom(attribute_name(modifier.attribute))},
            {"operation", Value::atom(modifier.operation == gamebattle::ModifierOperation::add
                                          ? "add" : "scale_bp")},
            {"value", int_value(modifier.value)}}));
    }
    Value::List reactions;
    for (const auto& reaction : buff.reactions) {
        reactions.push_back(Value::object({
            {"trigger", Value::atom(trigger_name(reaction.trigger))},
            {"source", Value::atom(reaction.source == gamebattle::EffectSource::owner
                                       ? "owner" : "applier")},
            {"stack_scaling", Value::atom(reaction.stack_scaling == gamebattle::StackScaling::once
                                              ? "once" : "per_stack")},
            {"chance_bp", int_value(reaction.chance_bp)},
            {"max_triggers_per_round", int_value(reaction.max_triggers_per_round)},
            {"effects", encode_effects(reaction.effects)}}));
    }
    const char* refresh = "reset";
    if (buff.stacking.refresh == gamebattle::RefreshPolicy::extend) refresh = "extend";
    if (buff.stacking.refresh == gamebattle::RefreshPolicy::keep) refresh = "keep";
    return Value::object({
        {"id", int_value(buff.id)},
        {"name", Value::binary(buff.name)},
        {"lifetime", Value::object({
            {"type", Value::atom(buff.lifetime.permanent ? "permanent" : "finite")},
            {"duration", int_value(buff.lifetime.duration)},
            {"decrement_on", Value::atom(trigger_name(buff.lifetime.decrement_on))}})},
        {"stacking", Value::object({
            {"max_stacks", int_value(buff.stacking.max_stacks)},
            {"policy", Value::atom(buff.stacking.mode == gamebattle::StackPolicy::stack
                                       ? "stack" : "refresh")},
            {"refresh", Value::atom(refresh)}})},
        {"modifiers", Value::list(std::move(modifiers))},
        {"reactions", Value::list(std::move(reactions))}});
}

inline Value encode_effects(const std::vector<gamebattle::Effect>& effects) {
    Value::List result;
    for (const auto& effect : effects) {
        Value::Object fields{
            {"type", Value::atom(effect_kind_name(effect.kind))},
            {"target", Value::atom(target_name(effect.target))},
            {"target_count", int_value(effect.target_count)},
            {"attack_bp", int_value(effect.attack_bp)},
            {"flat", int_value(effect.flat)}};
        if (effect.kind == gamebattle::EffectKind::add_buff && effect.buff != nullptr) {
            fields.emplace_back("buff", encode_buff(*effect.buff));
        }
        if (effect.kind == gamebattle::EffectKind::remove_buff) {
            fields.emplace_back("buff_id", int_value(effect.remove_buff_id));
        }
        result.push_back(Value::object(std::move(fields)));
    }
    return Value::list(std::move(result));
}

inline Value encode_unit(const gamebattle::UnitConfig& unit) {
    const auto& s = unit.final_stats;
    Value::List skills;
    for (const auto& skill : unit.skills) {
        skills.push_back(Value::object({
            {"id", int_value(skill.id)},
            {"name", Value::binary(skill.name)},
            {"chance_bp", int_value(skill.chance_bp)},
            {"priority", int_value(skill.priority)},
            {"effects", encode_effects(skill.effects)}}));
    }
    Value::List passives;
    for (const auto& passive : unit.passives) {
        passives.push_back(Value::object({
            {"id", int_value(passive.id)},
            {"name", Value::binary(passive.name)},
            {"trigger", Value::atom(trigger_name(passive.trigger))},
            {"chance_bp", int_value(passive.chance_bp)},
            {"max_triggers_per_round", int_value(passive.max_triggers_per_round)},
            {"effects", encode_effects(passive.effects)}}));
    }
    return Value::object({
        {"id", int_value(static_cast<std::int64_t>(unit.id))},
        {"kind", Value::atom(unit_kind_name(unit.kind))},
        {"position", int_value(unit.position)},
        {"level", int_value(unit.level)},
        {"can_act", bool_value(unit.can_act)},
        {"targetable", bool_value(unit.targetable)},
        {"final_stats", Value::object({
            {"hp", int_value(s.hp)}, {"attack", int_value(s.attack)},
            {"defense", int_value(s.defense)}, {"speed", int_value(s.speed)},
            {"crit_rate_bp", int_value(s.crit_rate_bp)},
            {"crit_damage_bp", int_value(s.crit_damage_bp)},
            {"hit_rate_bp", int_value(s.hit_rate_bp)},
            {"dodge_rate_bp", int_value(s.dodge_rate_bp)},
            {"damage_bonus_bp", int_value(s.damage_bonus_bp)},
            {"damage_reduction_bp", int_value(s.damage_reduction_bp)}})},
        {"skills", Value::list(std::move(skills))},
        {"passives", Value::list(std::move(passives))}});
}

inline Value encode_formation(const gamebattle::Formation& formation) {
    Value::List units;
    for (const auto& unit : formation.units) {
        units.push_back(encode_unit(unit));
    }
    return Value::object({
        {"formation", Value::atom(formation.name.empty() ? "default" : formation.name)},
        {"initiative_bonus", int_value(formation.initiative_bonus)},
        {"units", Value::list(std::move(units))}});
}

inline Value encode_request(const gamebattle::BattleRequest& request) {
    return Value::object({
        {"battle_id", int_value(static_cast<std::int64_t>(request.battle_id))},
        {"seed", int_value(static_cast<std::int64_t>(request.seed))},
        {"max_rounds", int_value(request.max_rounds)},
        {"max_events", int_value(request.max_events)},
        {"attacker", encode_formation(request.attacker)},
        {"defender", encode_formation(request.defender)}});
}

} // namespace practice
