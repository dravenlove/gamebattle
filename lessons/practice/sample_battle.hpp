#pragma once

// A 5v5 request that exercises skills, on-hit poison, counter attacks and an
// attack buff, so practice programs measure a realistic amount of work.
//
// Only the front unit of each side carries the buff passives, and each side
// uses its own buff ids. Inline ETF requests that put the same buff id on two
// units are currently rejected by the engine (see lesson 22), so this keeps
// the request valid on both the direct and the ETF path.

#include "gamebattle/engine.hpp"

#include <cstdint>
#include <memory>
#include <utility>

namespace practice {

inline gamebattle::UnitConfig make_hero(gamebattle::UnitId id, std::int32_t position,
                                        std::int64_t hp, std::int64_t attack,
                                        std::int64_t defense, std::int64_t speed) {
    gamebattle::UnitConfig unit;
    unit.id = id;
    unit.kind = gamebattle::UnitKind::hero;
    unit.position = position;
    unit.final_stats.hp = hp;
    unit.final_stats.attack = attack;
    unit.final_stats.defense = defense;
    unit.final_stats.speed = speed;
    unit.final_stats.crit_rate_bp = 1500;
    unit.final_stats.dodge_rate_bp = 500;
    return unit;
}

inline std::shared_ptr<const gamebattle::BuffSpec> poison_buff(std::uint32_t id) {
    auto buff = std::make_shared<gamebattle::BuffSpec>();
    buff->id = id;
    buff->name = "poison";
    buff->lifetime.duration = 2;
    buff->lifetime.decrement_on = gamebattle::Trigger::round_end;
    buff->stacking.max_stacks = 3;
    gamebattle::BuffReaction tick;
    tick.trigger = gamebattle::Trigger::round_end;
    tick.source = gamebattle::EffectSource::applier;
    tick.stack_scaling = gamebattle::StackScaling::per_stack;
    tick.effects.push_back(gamebattle::Effect{
        .kind = gamebattle::EffectKind::direct_damage,
        .target = gamebattle::TargetRule::self,
        .target_count = 1,
        .attack_bp = 0,
        .flat = 25,
        .buff = nullptr});
    buff->reactions.push_back(std::move(tick));
    return buff;
}

inline std::shared_ptr<const gamebattle::BuffSpec> rally_buff(std::uint32_t id) {
    auto buff = std::make_shared<gamebattle::BuffSpec>();
    buff->id = id;
    buff->name = "rally";
    buff->lifetime.duration = 3;
    buff->stacking.mode = gamebattle::StackPolicy::refresh;
    buff->modifiers.push_back(gamebattle::AttributeModifier{
        .attribute = gamebattle::Attribute::attack,
        .operation = gamebattle::ModifierOperation::scale_bp,
        .value = 1500});
    return buff;
}

// poison/rally may be null: then only the skill and the counter passive are added.
inline void equip(gamebattle::UnitConfig& unit,
                  const std::shared_ptr<const gamebattle::BuffSpec>& poison,
                  const std::shared_ptr<const gamebattle::BuffSpec>& rally) {
    gamebattle::Skill slash;
    slash.id = 501;
    slash.name = "flame_slash";
    slash.chance_bp = 3500;
    slash.priority = 10;
    slash.effects.push_back(gamebattle::Effect{
        .kind = gamebattle::EffectKind::damage,
        .target = gamebattle::TargetRule::all_enemies,
        .target_count = 256,
        .attack_bp = 6000,
        .buff = nullptr});
    unit.skills.push_back(std::move(slash));

    if (poison != nullptr) {
        gamebattle::Passive venom;
        venom.id = 701;
        venom.name = "venom";
        venom.trigger = gamebattle::Trigger::on_hit;
        venom.chance_bp = 4000;
        venom.max_triggers_per_round = 2;
        gamebattle::Effect add_poison;
        add_poison.kind = gamebattle::EffectKind::add_buff;
        add_poison.target = gamebattle::TargetRule::trigger_unit;
        add_poison.buff = poison;
        venom.effects.push_back(std::move(add_poison));
        unit.passives.push_back(std::move(venom));
    }

    gamebattle::Passive counter;
    counter.id = 703;
    counter.name = "counter";
    counter.trigger = gamebattle::Trigger::on_damaged;
    counter.chance_bp = 2500;
    counter.max_triggers_per_round = 1;
    counter.effects.push_back(gamebattle::Effect{
        .kind = gamebattle::EffectKind::damage,
        .target = gamebattle::TargetRule::trigger_unit,
        .target_count = 1,
        .attack_bp = 5000,
        .buff = nullptr});
    unit.passives.push_back(std::move(counter));

    if (rally == nullptr) {
        return;
    }
    gamebattle::Passive rally_up;
    rally_up.id = 702;
    rally_up.name = "rally";
    rally_up.trigger = gamebattle::Trigger::battle_start;
    gamebattle::Effect add_rally;
    add_rally.kind = gamebattle::EffectKind::add_buff;
    add_rally.target = gamebattle::TargetRule::all_allies;
    add_rally.target_count = 256;
    add_rally.buff = rally;
    rally_up.effects.push_back(std::move(add_rally));
    unit.passives.push_back(std::move(rally_up));
}

inline gamebattle::BattleRequest sample_battle(std::uint64_t battle_id, std::uint64_t seed) {
    const auto attacker_poison = poison_buff(801);
    const auto attacker_rally = rally_buff(802);
    const auto defender_poison = poison_buff(811);
    const auto defender_rally = rally_buff(812);
    const std::shared_ptr<const gamebattle::BuffSpec> none;

    gamebattle::BattleRequest request;
    request.battle_id = battle_id;
    request.seed = seed;
    request.max_rounds = 30;
    request.max_events = 20000;
    for (std::int32_t slot = 0; slot < 5; ++slot) {
        auto attacker = make_hero(1001 + static_cast<gamebattle::UnitId>(slot), slot + 1,
                                  4200 + slot * 150, 260 + slot * 10, 70, 110 + slot * 3);
        auto defender = make_hero(2001 + static_cast<gamebattle::UnitId>(slot), slot + 1,
                                  4300 + slot * 140, 255 + slot * 11, 72, 108 + slot * 3);
        const bool front = slot == 0;
        equip(attacker, front ? attacker_poison : none, front ? attacker_rally : none);
        equip(defender, front ? defender_poison : none, front ? defender_rally : none);
        request.attacker.units.push_back(std::move(attacker));
        request.defender.units.push_back(std::move(defender));
    }
    return request;
}

} // namespace practice
