#include "gamebattle/engine.hpp"
#include "gamebattle/term.hpp"
#include "gamebattle/wire.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

gamebattle::UnitConfig hero(gamebattle::UnitId id, int position, std::int64_t speed,
                            std::int64_t hp, std::int64_t attack, std::int64_t defense) {
    gamebattle::UnitConfig unit;
    unit.id = id;
    unit.kind = gamebattle::UnitKind::hero;
    unit.position = position;
    unit.final_stats.hp = hp;
    unit.final_stats.attack = attack;
    unit.final_stats.defense = defense;
    unit.final_stats.speed = speed;
    return unit;
}

gamebattle::BattleRequest sample_request() {
    gamebattle::BattleRequest request;
    request.battle_id = 90001;
    request.seed = 20260902;
    request.max_rounds = 20;
    request.max_execution_steps = 2000;
    request.max_logged_events = 2000;

    auto fast = hero(1001, 1, 130, 900, 170, 20);
    gamebattle::Skill skill;
    skill.id = 501;
    skill.name = "flame_slash";
    skill.priority = 10;
    skill.effects.push_back(gamebattle::Effect{
        .kind = gamebattle::EffectKind::damage,
        .target = gamebattle::TargetRule::enemy_front,
        .target_count = 1,
        .attack_bp = 12000
    });
    fast.skills.push_back(skill);

    gamebattle::Passive poison;
    poison.id = 701;
    poison.name = "poison_blade";
    poison.trigger = gamebattle::Trigger::on_hit;
    poison.max_triggers_per_round = 1;
    gamebattle::Effect add_poison;
    add_poison.kind = gamebattle::EffectKind::add_buff;
    add_poison.target = gamebattle::TargetRule::trigger_unit;
    auto poison_buff = std::make_shared<gamebattle::BuffSpec>();
    poison_buff->id = 801;
    poison_buff->name = "poison";
    poison_buff->lifetime.duration = 2;
    poison_buff->lifetime.decrement_on = gamebattle::Trigger::round_end;
    poison_buff->stacking.max_stacks = 3;
    poison_buff->stacking.mode = gamebattle::StackPolicy::stack;
    poison_buff->stacking.refresh = gamebattle::RefreshPolicy::reset;
    gamebattle::BuffReaction poison_tick;
    poison_tick.trigger = gamebattle::Trigger::round_end;
    poison_tick.source = gamebattle::EffectSource::applier;
    poison_tick.stack_scaling = gamebattle::StackScaling::per_stack;
    poison_tick.effects.push_back(gamebattle::Effect{
        .kind = gamebattle::EffectKind::direct_damage,
        .target = gamebattle::TargetRule::self,
        .target_count = 1,
        .attack_bp = 0,
        .flat = 25
    });
    poison_buff->reactions.push_back(std::move(poison_tick));
    add_poison.buff = std::move(poison_buff);
    poison.effects.push_back(add_poison);
    fast.passives.push_back(poison);

    request.attacker.units.push_back(std::move(fast));
    request.attacker.units.push_back(hero(1002, 2, 90, 1000, 140, 30));
    request.defender.units.push_back(hero(2001, 1, 100, 1300, 155, 35));
    request.defender.units.push_back(hero(2002, 2, 80, 900, 125, 25));
    return request;
}

gamebattle::term::Value request_term_with_buff(gamebattle::term::Value buff) {
    using gamebattle::term::Value;
    const auto stats = [](std::int64_t hp, std::int64_t attack,
                          std::int64_t defense, std::int64_t speed) {
        return Value::object({
            {"hp", Value(hp)},
            {"attack", Value(attack)},
            {"defense", Value(defense)},
            {"speed", Value(speed)}
        });
    };
    const auto unit = [&](std::int64_t id, std::int64_t position,
                          Value final_stats, Value::List passives) {
        return Value::object({
            {"id", Value(id)},
            {"kind", Value::atom("hero")},
            {"position", Value(position)},
            {"final_stats", std::move(final_stats)},
            {"passives", Value::list(std::move(passives))}
        });
    };

    Value add_buff = Value::object({
        {"type", Value::atom("add_buff")},
        {"target", Value::atom("self")},
        {"buff", std::move(buff)}
    });
    Value passive = Value::object({
        {"id", Value(std::int64_t{7001})},
        {"name", Value::binary("embedded_buff")},
        {"trigger", Value::atom("battle_start")},
        {"priority", Value(std::int64_t{0})},
        {"effects", Value::list({std::move(add_buff)})}
    });
    return Value::object({
        {"battle_id", Value(std::int64_t{92001})},
        {"seed", Value(std::int64_t{42})},
        {"attacker", Value::object({
            {"formation", Value::atom("test")},
            {"units", Value::list({
                unit(3001, 1, stats(1000, 100, 0, 100),
                     Value::List{std::move(passive)})
            })}
        })},
        {"defender", Value::object({
            {"formation", Value::atom("test")},
            {"units", Value::list({
                unit(4001, 1, stats(1000, 100, 0, 90), {})
            })}
        })}
    });
}

void test_wire_generic_buff_schema_and_reject_legacy_fields() {
    using gamebattle::term::Value;
    Value v2 = Value::object({
        {"id", Value(std::int64_t{8101})},
        {"name", Value::binary("v2_buff")},
        {"lifetime", Value::object({
            {"type", Value::atom("finite")},
            {"duration", Value(std::int64_t{3})},
            {"decrement_on", Value::atom("after_action")}
        })},
        {"stacking", Value::object({
            {"max_stacks", Value(std::int64_t{4})},
            {"policy", Value::atom("stack")},
            {"refresh", Value::atom("extend")},
            {"key", Value::atom("by_buff")}
        })},
        {"modifiers", Value::list({
            Value::object({
                {"attribute", Value::atom("attack")},
                {"operation", Value::atom("add")},
                {"value", Value(std::int64_t{25})}
            }),
            Value::object({
                {"attribute", Value::atom("damage_bonus_bp")},
                {"operation", Value::atom("scale_bp")},
                {"value", Value(std::int64_t{1000})}
            })
        })},
        {"reactions", Value::list({Value::object({
            {"trigger", Value::atom("round_end")},
            {"source", Value::atom("applier")},
            {"stack_scaling", Value::atom("per_stack")},
            {"chance_bp", Value(std::int64_t{10000})},
            {"max_triggers_per_round", Value(std::int64_t{1})},
            {"priority", Value(std::int64_t{0})},
            {"effects", Value::list({Value::object({
                {"type", Value::atom("direct_damage")},
                {"target", Value::atom("self")},
                {"attack_bp", Value(std::int64_t{0})},
                {"flat", Value(std::int64_t{35})}
            })})}
        })})}
    });

    const auto request = request_term_with_buff(v2);
    const auto parsed_v2 = gamebattle::wire::parse_request(request);
    const auto v2_buff =
        parsed_v2.attacker.units.front().passives.front().effects.front().buff;
    assert(v2_buff != nullptr);
    assert(v2_buff->lifetime.duration == 3);
    assert(v2_buff->lifetime.decrement_on == gamebattle::Trigger::after_action);
    assert(v2_buff->stacking.max_stacks == 4);
    assert(v2_buff->stacking.mode == gamebattle::StackPolicy::stack);
    assert(v2_buff->stacking.refresh == gamebattle::RefreshPolicy::extend);
    assert(v2_buff->modifiers.size() == 2);
    assert(v2_buff->modifiers[0].attribute == gamebattle::Attribute::attack);
    assert(v2_buff->modifiers[1].operation ==
           gamebattle::ModifierOperation::scale_bp);
    assert(v2_buff->reactions.size() == 1);
    assert(v2_buff->reactions.front().source ==
           gamebattle::EffectSource::applier);
    assert(v2_buff->reactions.front().stack_scaling ==
           gamebattle::StackScaling::per_stack);
    assert(v2_buff->reactions.front().effects.front().kind ==
           gamebattle::EffectKind::direct_damage);

    gamebattle::wire::Handler handler;
    const auto response = gamebattle::term::decode(
        handler.handle_etf(gamebattle::term::encode(request)));
    const auto* response_tuple =
        std::get_if<Value::TupleValue>(&response.data);
    assert(response_tuple != nullptr && response_tuple->value.size() == 2);
    assert(gamebattle::term::as_string(response_tuple->value[0], "status") ==
           "ok");
    const auto* events = gamebattle::term::find(
        response_tuple->value[1], "events");
    assert(events != nullptr);
    bool found_reaction = false;
    bool found_direct_damage = false;
    for (const auto& event : gamebattle::term::as_list(*events, "events")) {
        const auto type = gamebattle::term::get_string(event, "type");
        const auto source_id = gamebattle::term::get_int(event, "source_id");
        found_reaction = found_reaction ||
            (type == "buff_reaction" && source_id == 8101);
        found_direct_damage = found_direct_damage ||
            (type == "direct_damage" && source_id == 8101);
    }
    assert(found_reaction);
    assert(found_direct_damage);

    Value legacy = Value::object({
        {"id", Value(std::int64_t{8102})},
        {"name", Value::binary("legacy_buff")},
        {"duration_rounds", Value(std::int64_t{2})},
        {"max_stacks", Value(std::int64_t{3})},
        {"tick", Value::atom("round_end")},
        {"tick_effect", Value::atom("damage")},
        {"tick_flat", Value(std::int64_t{20})},
        {"tick_attack_bp", Value(std::int64_t{1000})},
        {"attack_flat", Value(std::int64_t{15})},
        {"damage_reduction_bp", Value(std::int64_t{500})}
    });
    bool legacy_rejected = false;
    try {
        static_cast<void>(gamebattle::wire::parse_request(
            request_term_with_buff(std::move(legacy))));
    } catch (const gamebattle::term::DecodeError&) {
        legacy_rejected = true;
    }
    assert(legacy_rejected);

    bool incomplete_rejected = false;
    try {
        static_cast<void>(gamebattle::wire::parse_request(
            request_term_with_buff(Value::object({
                {"id", Value(std::int64_t{8103})},
                {"name", Value::binary("incomplete")},
                {"modifiers", Value::list({})},
                {"reactions", Value::list({})}
            }))));
    } catch (const gamebattle::term::DecodeError&) {
        incomplete_rejected = true;
    }
    assert(incomplete_rejected);

    bool invalid_refresh_rejected = false;
    try {
        static_cast<void>(gamebattle::wire::parse_request(
            request_term_with_buff(Value::object({
                {"id", Value(std::int64_t{8104})},
                {"name", Value::binary("invalid_refresh")},
                {"lifetime", Value::object({
                    {"type", Value::atom("finite")},
                    {"duration", Value(std::int64_t{2})},
                    {"decrement_on", Value::atom("round_end")}
                })},
                {"stacking", Value::object({
                    {"max_stacks", Value(std::int64_t{2})},
                    {"policy", Value::atom("refresh")},
                    {"refresh", Value::atom("reset")},
                    {"key", Value::atom("by_buff")}
                })},
                {"modifiers", Value::list({})},
                {"reactions", Value::list({})}
            }))));
    } catch (const gamebattle::term::DecodeError&) {
        invalid_refresh_rejected = true;
    }
    assert(invalid_refresh_rejected);
}

void test_wire_embedded_buff_depth_limit() {
    using gamebattle::term::Value;
    Value nested = Value::object({
        {"id", Value(std::int64_t{8200})},
        {"name", Value::binary("leaf")},
        {"lifetime", Value::object({
            {"type", Value::atom("permanent")},
            {"duration", Value(std::int64_t{0})},
            {"decrement_on", Value::atom("round_end")}
        })},
        {"stacking", Value::object({
            {"max_stacks", Value(std::int64_t{1})},
            {"policy", Value::atom("refresh")},
            {"refresh", Value::atom("reset")},
            {"key", Value::atom("by_buff")}
        })},
        {"modifiers", Value::list({})},
        {"reactions", Value::list({})}
    });
    for (std::int64_t index = 0; index < 40; ++index) {
        Value nested_effect = Value::object({
            {"type", Value::atom("add_buff")},
            {"target", Value::atom("self")},
            {"buff", std::move(nested)}
        });
        nested = Value::object({
            {"id", Value(8300 + index)},
            {"name", Value::binary("nested")},
            {"lifetime", Value::object({
                {"type", Value::atom("permanent")},
                {"duration", Value(std::int64_t{0})},
                {"decrement_on", Value::atom("round_end")}
            })},
            {"stacking", Value::object({
                {"max_stacks", Value(std::int64_t{1})},
                {"policy", Value::atom("refresh")},
                {"refresh", Value::atom("reset")},
                {"key", Value::atom("by_buff")}
            })},
            {"modifiers", Value::list({})},
            {"reactions", Value::list({Value::object({
                {"trigger", Value::atom("round_end")},
                {"source", Value::atom("owner")},
                {"priority", Value(std::int64_t{0})},
                {"effects", Value::list({std::move(nested_effect)})}
            })})}
        });
    }

    bool rejected = false;
    try {
        static_cast<void>(gamebattle::wire::parse_request(
            request_term_with_buff(std::move(nested))));
    } catch (const gamebattle::term::DecodeError&) {
        rejected = true;
    }
    assert(rejected);
}

void test_core_rejects_invalid_buff_definitions() {
    const auto request_with_buff = [](std::shared_ptr<const gamebattle::BuffSpec> buff) {
        gamebattle::BattleRequest request;
        request.battle_id = 92500;
        request.seed = 1;
        request.max_rounds = 1;
        request.max_execution_steps = 1000;
        request.max_logged_events = 1000;

        auto attacker = hero(4501, 1, 100, 1000, 100, 0);
        gamebattle::Passive passive;
        passive.id = 7501;
        passive.name = "apply_test_buff";
        passive.trigger = gamebattle::Trigger::battle_start;
        gamebattle::Effect effect;
        effect.kind = gamebattle::EffectKind::add_buff;
        effect.target = gamebattle::TargetRule::self;
        effect.buff = std::move(buff);
        passive.effects.push_back(std::move(effect));
        attacker.passives.push_back(std::move(passive));
        request.attacker.units.push_back(std::move(attacker));
        request.defender.units.push_back(hero(4502, 1, 90, 1000, 100, 0));
        return request;
    };
    const auto rejected = [&](const std::shared_ptr<const gamebattle::BuffSpec>& buff) {
        try {
            static_cast<void>(gamebattle::Engine{}.simulate(request_with_buff(buff)));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    auto invalid_permanent = std::make_shared<gamebattle::BuffSpec>();
    invalid_permanent->id = 8251;
    invalid_permanent->name = "invalid_permanent_duration";
    invalid_permanent->lifetime.permanent = true;
    invalid_permanent->lifetime.duration = 1;
    assert(rejected(invalid_permanent));

    auto invalid_refresh = std::make_shared<gamebattle::BuffSpec>();
    invalid_refresh->id = 8252;
    invalid_refresh->name = "invalid_refresh_stacks";
    invalid_refresh->stacking.mode = gamebattle::StackPolicy::refresh;
    invalid_refresh->stacking.max_stacks = 2;
    assert(rejected(invalid_refresh));

    auto invalid_scale = std::make_shared<gamebattle::BuffSpec>();
    invalid_scale->id = 8253;
    invalid_scale->name = "invalid_scale_modifier";
    invalid_scale->modifiers.push_back({
        .attribute = gamebattle::Attribute::attack,
        .operation = gamebattle::ModifierOperation::scale_bp,
        .value = 1'000'001
    });
    assert(rejected(invalid_scale));

    auto cyclic = std::make_shared<gamebattle::BuffSpec>();
    cyclic->id = 8254;
    cyclic->name = "cyclic_buff_definition";
    gamebattle::Effect add_self;
    add_self.kind = gamebattle::EffectKind::add_buff;
    add_self.target = gamebattle::TargetRule::self;
    add_self.buff = cyclic;
    gamebattle::BuffReaction reaction;
    reaction.trigger = gamebattle::Trigger::round_end;
    reaction.effects.push_back(std::move(add_self));
    cyclic->reactions.push_back(std::move(reaction));
    assert(rejected(cyclic));
    cyclic->reactions.clear(); // Break the deliberately-created shared_ptr cycle.
}

void test_generic_modifier_composition() {
    gamebattle::BattleRequest request;
    request.battle_id = 93001;
    request.seed = 7;
    request.max_rounds = 1;
    request.max_execution_steps = 1000;
    request.max_logged_events = 1000;

    auto attacker = hero(5001, 1, 200, 1000, 100, 0);
    gamebattle::Skill strike;
    strike.id = 5101;
    strike.name = "strike";
    strike.effects.push_back(gamebattle::Effect{
        .kind = gamebattle::EffectKind::damage,
        .target = gamebattle::TargetRule::enemy_front,
        .target_count = 1,
        .attack_bp = 10000
    });
    attacker.skills.push_back(std::move(strike));

    auto boost = std::make_shared<gamebattle::BuffSpec>();
    boost->id = 8104;
    boost->name = "composed_attack_boost";
    boost->lifetime.permanent = true;
    boost->lifetime.duration = 0;
    boost->stacking.mode = gamebattle::StackPolicy::refresh;
    boost->modifiers = {
        {
            .attribute = gamebattle::Attribute::attack,
            .operation = gamebattle::ModifierOperation::add,
            .value = 50
        },
        {
            .attribute = gamebattle::Attribute::attack,
            .operation = gamebattle::ModifierOperation::scale_bp,
            .value = 5000
        }
    };
    gamebattle::Passive apply_boost;
    apply_boost.id = 7101;
    apply_boost.name = "apply_boost";
    apply_boost.trigger = gamebattle::Trigger::battle_start;
    gamebattle::Effect add_boost;
    add_boost.kind = gamebattle::EffectKind::add_buff;
    add_boost.target = gamebattle::TargetRule::self;
    add_boost.buff = std::move(boost);
    apply_boost.effects.push_back(std::move(add_boost));
    attacker.passives.push_back(std::move(apply_boost));

    request.attacker.units.push_back(std::move(attacker));
    request.defender.units.push_back(hero(6001, 1, 100, 1000, 0, 0));
    const auto result = gamebattle::Engine{}.simulate(request);

    bool found_composed_damage = false;
    for (const auto& event : result.events) {
        if (event.type == "damage" && event.actor == 5001 &&
            event.target == 6001) {
            // (100 base attack + 50 additive) * (100% + 50%) = 225.
            assert(event.value == 225);
            found_composed_damage = true;
            break;
        }
    }
    assert(found_composed_damage);
}

void test_refresh_and_lifetime_are_deterministic() {
    gamebattle::BattleRequest request;
    request.battle_id = 93002;
    request.seed = 8;
    request.max_rounds = 2;
    request.max_execution_steps = 1000;
    request.max_logged_events = 1000;

    auto attacker = hero(5002, 1, 200, 10000, 1, 0);
    auto refreshable = std::make_shared<gamebattle::BuffSpec>();
    refreshable->id = 8105;
    refreshable->name = "refreshable";
    refreshable->lifetime.duration = 1;
    refreshable->lifetime.decrement_on = gamebattle::Trigger::round_end;
    refreshable->stacking.max_stacks = 1;
    refreshable->stacking.mode = gamebattle::StackPolicy::refresh;
    refreshable->stacking.refresh = gamebattle::RefreshPolicy::extend;

    gamebattle::Effect add_refreshable;
    add_refreshable.kind = gamebattle::EffectKind::add_buff;
    add_refreshable.target = gamebattle::TargetRule::self;
    add_refreshable.buff = refreshable;
    gamebattle::Passive apply_twice;
    apply_twice.id = 7102;
    apply_twice.name = "apply_twice";
    apply_twice.trigger = gamebattle::Trigger::battle_start;
    apply_twice.effects = {add_refreshable, add_refreshable};
    attacker.passives.push_back(std::move(apply_twice));

    request.attacker.units.push_back(std::move(attacker));
    request.defender.units.push_back(hero(6002, 1, 100, 10000, 1, 0));

    const auto first = gamebattle::Engine{}.simulate(request);
    const auto second = gamebattle::Engine{}.simulate(request);
    const auto first_bytes = gamebattle::term::encode(
        gamebattle::wire::encode_result(first));
    const auto second_bytes = gamebattle::term::encode(
        gamebattle::wire::encode_result(second));
    assert(first_bytes == second_bytes);

    std::int32_t additions = 0;
    bool expired_in_round_two = false;
    for (const auto& event : first.events) {
        if (event.type == "buff_add" && event.target == 5002) {
            ++additions;
            assert(event.value == 1); // refresh does not add a stack
        }
        if (event.type == "buff_expire" && event.source_id == 8105) {
            assert(event.round == 2);
            expired_in_round_two = true;
        }
    }
    assert(additions == 2);
    assert(expired_in_round_two);
}

const gamebattle::UnitResult& find_result_unit(
    const gamebattle::BattleResult& result, gamebattle::UnitId id);

void test_fatal_damage_runs_on_damaged_and_unit_death() {
    gamebattle::BattleRequest request;
    request.battle_id = 94001;
    request.seed = 1;
    request.max_rounds = 1;
    request.max_execution_steps = 10000;
    request.max_logged_events = 1000;

    request.attacker.units.push_back(hero(7001, 1, 200, 100, 200, 0));
    auto defender = hero(7002, 1, 100, 100, 1, 0);

    gamebattle::Passive fatal_counter;
    fatal_counter.id = 9101;
    fatal_counter.name = "fatal_counter";
    fatal_counter.trigger = gamebattle::Trigger::on_damaged;
    fatal_counter.priority = 100;
    fatal_counter.effects.push_back(gamebattle::Effect{
        .kind = gamebattle::EffectKind::direct_damage,
        .target = gamebattle::TargetRule::trigger_unit,
        .attack_bp = 0,
        .flat = 7
    });

    gamebattle::Passive death_burst;
    death_burst.id = 9102;
    death_burst.name = "death_burst";
    death_burst.trigger = gamebattle::Trigger::unit_death;
    death_burst.priority = 90;
    death_burst.effects.push_back(gamebattle::Effect{
        .kind = gamebattle::EffectKind::direct_damage,
        .target = gamebattle::TargetRule::enemy_front,
        .attack_bp = 0,
        .flat = 11
    });
    defender.passives = {fatal_counter, death_burst};
    request.defender.units.push_back(std::move(defender));

    const auto result = gamebattle::Engine{}.simulate(request);
    std::uint32_t hit_seq = 0;
    std::uint32_t damaged_passive_seq = 0;
    std::uint32_t death_seq = 0;
    std::uint32_t death_passive_seq = 0;
    std::uint64_t hit_event_id = 0;
    std::uint64_t damaged_passive_parent = 0;
    std::uint64_t death_event_id = 0;
    std::uint64_t death_passive_parent = 0;
    bool found_counter_damage = false;
    bool found_death_damage = false;
    for (const auto& event : result.events) {
        assert(event.event_id != 0);
        if (event.type == "damage" && event.actor == 7001 &&
            event.target == 7002) {
            hit_seq = event.seq;
            hit_event_id = event.event_id;
        }
        if (event.type == "passive" && event.source_id == 9101) {
            damaged_passive_seq = event.seq;
            damaged_passive_parent = event.parent_event_id;
        }
        if (event.type == "death" && event.target == 7002) {
            death_seq = event.seq;
            death_event_id = event.event_id;
        }
        if (event.type == "passive" && event.source_id == 9102) {
            death_passive_seq = event.seq;
            death_passive_parent = event.parent_event_id;
        }
        found_counter_damage = found_counter_damage ||
            (event.type == "direct_damage" && event.actor == 7002 &&
             event.target == 7001 && event.source_id == 9101 &&
             event.value == 7);
        found_death_damage = found_death_damage ||
            (event.type == "direct_damage" && event.actor == 7002 &&
             event.target == 7001 && event.source_id == 9102 &&
             event.value == 11);
    }
    assert(hit_seq > 0);
    assert(hit_seq < damaged_passive_seq);
    assert(damaged_passive_parent == hit_event_id);
    assert(damaged_passive_seq < death_seq);
    assert(death_seq < death_passive_seq);
    assert(death_passive_parent == death_event_id);
    assert(found_counter_damage);
    assert(found_death_damage);
    assert(find_result_unit(result, 7001).hp == 82);
}

void test_reaction_priority_is_global_and_descending() {
    gamebattle::BattleRequest request;
    request.battle_id = 94002;
    request.seed = 2;
    request.max_rounds = 1;
    request.max_execution_steps = 10000;
    request.max_logged_events = 1000;

    auto low_priority_unit = hero(7101, 1, 200, 1000, 1, 0);
    gamebattle::Passive low;
    low.id = 9201;
    low.name = "low";
    low.trigger = gamebattle::Trigger::battle_start;
    low.priority = -10;
    low.effects.push_back(gamebattle::Effect{
        .kind = gamebattle::EffectKind::direct_damage,
        .target = gamebattle::TargetRule::enemy_front,
        .attack_bp = 0,
        .flat = 1
    });
    low_priority_unit.passives.push_back(std::move(low));

    auto high_priority_unit = hero(7102, 2, 190, 1000, 1, 0);
    gamebattle::Passive high;
    high.id = 9202;
    high.name = "high";
    high.trigger = gamebattle::Trigger::battle_start;
    high.priority = 100;
    high.effects.push_back(gamebattle::Effect{
        .kind = gamebattle::EffectKind::direct_damage,
        .target = gamebattle::TargetRule::enemy_front,
        .attack_bp = 0,
        .flat = 1
    });
    high_priority_unit.passives.push_back(std::move(high));

    request.attacker.units.push_back(std::move(low_priority_unit));
    request.attacker.units.push_back(std::move(high_priority_unit));
    request.defender.units.push_back(hero(7103, 1, 100, 10000, 1, 0));

    const auto result = gamebattle::Engine{}.simulate(request);
    std::vector<std::uint32_t> battle_start_passives;
    for (const auto& event : result.events) {
        if (event.type == "passive" &&
            (event.source_id == 9201 || event.source_id == 9202)) {
            battle_start_passives.push_back(event.source_id);
        }
    }
    assert(battle_start_passives.size() == 2);
    assert(battle_start_passives[0] == 9202);
    assert(battle_start_passives[1] == 9201);
}

void test_buff_stack_key_keeps_appliers_independent() {
    gamebattle::BattleRequest request;
    request.battle_id = 94003;
    request.seed = 3;
    request.max_rounds = 1;
    request.max_execution_steps = 10000;
    request.max_logged_events = 2000;

    auto poison = std::make_shared<gamebattle::BuffSpec>();
    poison->id = 9301;
    poison->name = "per_source_poison";
    poison->lifetime.duration = 2;
    poison->lifetime.decrement_on = gamebattle::Trigger::round_end;
    poison->stacking.max_stacks = 3;
    poison->stacking.mode = gamebattle::StackPolicy::stack;
    poison->stacking.key = gamebattle::StackKeyPolicy::by_buff_and_source;
    gamebattle::BuffReaction tick;
    tick.trigger = gamebattle::Trigger::round_end;
    tick.priority = 0;
    tick.source = gamebattle::EffectSource::applier;
    tick.stack_scaling = gamebattle::StackScaling::per_stack;
    tick.effects.push_back(gamebattle::Effect{
        .kind = gamebattle::EffectKind::direct_damage,
        .target = gamebattle::TargetRule::self,
        .attack_bp = 10000,
        .flat = 0
    });
    poison->reactions.push_back(std::move(tick));

    const auto poison_applier = [&](gamebattle::UnitId id,
                                    std::int64_t attack,
                                    std::int32_t priority) {
        auto unit = hero(id, static_cast<int>(id - 7200),
                         200 - static_cast<std::int64_t>(id - 7201),
                         1000, attack, 0);
        gamebattle::Passive passive;
        passive.id = static_cast<std::uint32_t>(9400 + id - 7200);
        passive.name = "apply_poison";
        passive.trigger = gamebattle::Trigger::battle_start;
        passive.priority = priority;
        gamebattle::Effect effect;
        effect.kind = gamebattle::EffectKind::add_buff;
        effect.target = gamebattle::TargetRule::enemy_front;
        effect.buff = poison;
        passive.effects.push_back(std::move(effect));
        unit.passives.push_back(std::move(passive));
        return unit;
    };

    request.attacker.units.push_back(poison_applier(7201, 10, 100));
    request.attacker.units.push_back(poison_applier(7202, 20, 90));
    request.defender.units.push_back(hero(7203, 1, 100, 10000, 1, 0));

    const auto result = gamebattle::Engine{}.simulate(request);
    std::int32_t additions = 0;
    std::vector<std::pair<gamebattle::UnitId, std::int64_t>> ticks;
    for (const auto& event : result.events) {
        if (event.type == "buff_add" && event.target == 7203) {
            ++additions;
            assert(event.value == 1);
        }
        if (event.type == "direct_damage" && event.source_id == 9301) {
            ticks.emplace_back(event.actor, event.value);
        }
    }
    assert(additions == 2);
    assert(ticks.size() == 2);
    assert(ticks[0].first == 7201 && ticks[0].second == 10);
    assert(ticks[1].first == 7202 && ticks[1].second == 20);
}

void test_exhausted_reaction_does_not_consume_rng() {
    gamebattle::BattleRequest request;
    request.battle_id = 94004;
    request.seed = 6; // SplitMix rolls: 592, 3833, 1686, 7808.
    request.max_rounds = 1;
    request.max_execution_steps = 10000;
    request.max_logged_events = 2000;

    auto attacker = hero(7301, 1, 200, 10000, 1, 0);
    gamebattle::Skill double_hit;
    double_hit.id = 9501;
    double_hit.name = "double_hit";
    double_hit.effects = {
        gamebattle::Effect{
            .kind = gamebattle::EffectKind::damage,
            .target = gamebattle::TargetRule::enemy_front,
            .attack_bp = 0,
            .flat = 1
        },
        gamebattle::Effect{
            .kind = gamebattle::EffectKind::damage,
            .target = gamebattle::TargetRule::enemy_front,
            .attack_bp = 0,
            .flat = 1
        }
    };
    attacker.skills.push_back(std::move(double_hit));

    const auto probabilistic_passive = [](
        std::uint32_t id, std::int32_t priority,
        std::int32_t max_triggers) {
        gamebattle::Passive passive;
        passive.id = id;
        passive.name = "rng_probe";
        passive.trigger = gamebattle::Trigger::on_hit;
        passive.priority = priority;
        passive.chance_bp = 5000;
        passive.max_triggers_per_round = max_triggers;
        passive.effects.push_back(gamebattle::Effect{
            .kind = gamebattle::EffectKind::heal,
            .target = gamebattle::TargetRule::self,
            .attack_bp = 0,
            .flat = 0
        });
        return passive;
    };
    attacker.passives.push_back(probabilistic_passive(9601, 100, 1));
    attacker.passives.push_back(probabilistic_passive(9602, 0, 0));
    request.attacker.units.push_back(std::move(attacker));
    request.defender.units.push_back(hero(7302, 1, 100, 10000, 1, 0));

    const auto result = gamebattle::Engine{}.simulate(request);
    std::int32_t probe_triggers = 0;
    for (const auto& event : result.events) {
        if (event.type == "passive" && event.source_id == 9602) {
            ++probe_triggers;
        }
    }
    // On hit two, passive 9601 is already exhausted. The third random value
    // therefore belongs to 9602 and succeeds; consuming it before the limit
    // check would give 9602 the fourth value and only one trigger.
    assert(probe_triggers == 2);
}

void assert_same_outcome(const gamebattle::BattleResult& left,
                         const gamebattle::BattleResult& right) {
    assert(left.winner == right.winner);
    assert(left.reason == right.reason);
    assert(left.rounds == right.rounds);
    assert(left.execution_steps == right.execution_steps);
    assert(left.total_event_count == right.total_event_count);
    assert(left.units.size() == right.units.size());
    for (std::size_t index = 0; index < left.units.size(); ++index) {
        assert(left.units[index].id == right.units[index].id);
        assert(left.units[index].hp == right.units[index].hp);
        assert(left.units[index].alive == right.units[index].alive);
    }
}

void test_logging_budget_never_changes_combat() {
    auto full_request = sample_request();
    full_request.battle_id = 94005;
    full_request.max_execution_steps = 100000;
    full_request.max_logged_events = 10000;
    full_request.event_log_level = gamebattle::EventLogLevel::full;
    const auto full = gamebattle::Engine{}.simulate(full_request);
    assert(!full.events_truncated);
    assert(full.events.size() == full.total_event_count);

    auto capped_request = full_request;
    capped_request.max_logged_events = 3;
    const auto capped = gamebattle::Engine{}.simulate(capped_request);
    assert_same_outcome(full, capped);
    assert(capped.events.size() == 3);
    assert(capped.logged_event_count == 3);
    assert(capped.events_truncated);

    auto result_only_request = full_request;
    result_only_request.max_logged_events = 0;
    result_only_request.event_log_level =
        gamebattle::EventLogLevel::result_only;
    const auto result_only = gamebattle::Engine{}.simulate(result_only_request);
    assert_same_outcome(full, result_only);
    assert(result_only.events.empty());
    assert(result_only.logged_event_count == 0);
    assert(!result_only.events_truncated);

    auto summary_request = full_request;
    summary_request.event_log_level = gamebattle::EventLogLevel::summary;
    const auto summary = gamebattle::Engine{}.simulate(summary_request);
    assert_same_outcome(full, summary);
    assert(!summary.events.empty());
    assert(summary.events.size() < full.events.size());
    std::uint32_t previous_seq = 0;
    for (const auto& event : summary.events) {
        assert(event.seq > previous_seq);
        previous_seq = event.seq;
        assert(event.type == "initiative" || event.type == "skill" ||
               event.type == "passive" || event.type == "buff_reaction" ||
               event.type == "buff_add" || event.type == "buff_remove" ||
               event.type == "buff_expire" || event.type == "death");
    }
}

void test_execution_budget_stops_before_unlogged_mutation() {
    gamebattle::BattleRequest full_request;
    full_request.battle_id = 94006;
    full_request.seed = 4;
    full_request.max_rounds = 1;
    full_request.max_execution_steps = 100;
    full_request.max_logged_events = 1000;

    auto attacker = hero(7401, 1, 200, 1000, 1, 0);
    gamebattle::Skill many_hits;
    many_hits.id = 9701;
    many_hits.name = "many_hits";
    for (std::int32_t index = 0; index < 64; ++index) {
        many_hits.effects.push_back(gamebattle::Effect{
            .kind = gamebattle::EffectKind::damage,
            .target = gamebattle::TargetRule::enemy_front,
            .attack_bp = 0,
            .flat = 1
        });
    }
    attacker.skills.push_back(std::move(many_hits));
    full_request.attacker.units.push_back(std::move(attacker));
    full_request.defender.units.push_back(
        hero(7402, 1, 100, 100000, 1, 0));

    const auto full = gamebattle::Engine{}.simulate(full_request);
    assert(full.reason == "execution_limit");
    assert(full.winner == gamebattle::Winner::draw);
    assert(full.execution_steps == 100);
    std::int64_t logged_damage = 0;
    for (const auto& event : full.events) {
        if (event.type == "damage" && event.target == 7402) {
            logged_damage += event.value;
        }
    }
    assert(find_result_unit(full, 7402).hp == 100000 - logged_damage);

    auto result_only_request = full_request;
    result_only_request.max_logged_events = 0;
    result_only_request.event_log_level =
        gamebattle::EventLogLevel::result_only;
    const auto result_only = gamebattle::Engine{}.simulate(result_only_request);
    assert_same_outcome(full, result_only);
    assert(result_only.events.empty());
}

void test_engine_flow_and_determinism() {
    const auto request = sample_request();
    const auto first = gamebattle::Engine{}.simulate(request);
    const auto second = gamebattle::Engine{}.simulate(request);

    assert(first.battle_id == request.battle_id);
    assert(first.attacker_initiative == 220);
    assert(first.defender_initiative == 180);
    assert(!first.events.empty());
    assert(first.rounds > 0 && first.rounds <= request.max_rounds);

    bool found_first_actor = false;
    bool found_poison_add = false;
    bool found_poison_tick = false;
    bool found_second_poison_stack = false;
    bool found_scaled_poison_tick = false;
    for (const auto& event : first.events) {
        if (!found_first_actor && event.type == "action_start") {
            found_first_actor = true;
            assert(event.actor == 1001);
        }
        if (event.type == "buff_add") {
            found_poison_add = true;
            found_second_poison_stack =
                found_second_poison_stack || event.value == 2;
        }
        if (event.type == "direct_damage" && event.source_id == 801) {
            found_poison_tick = true;
            found_scaled_poison_tick =
                found_scaled_poison_tick || event.value == 50;
        }
    }
    assert(found_first_actor);
    assert(found_poison_add);
    assert(found_poison_tick);
    assert(found_second_poison_stack);
    assert(found_scaled_poison_tick);

    const auto first_bytes = gamebattle::term::encode(gamebattle::wire::encode_result(first));
    const auto second_bytes = gamebattle::term::encode(gamebattle::wire::encode_result(second));
    assert(first_bytes == second_bytes);
}

const gamebattle::UnitResult& find_result_unit(
    const gamebattle::BattleResult& result, gamebattle::UnitId id) {
    for (const auto& unit : result.units) {
        if (unit.id == id) {
            return unit;
        }
    }
    assert(false && "result unit was not found");
    return result.units.front();
}

void test_initial_hp_carryover() {
    auto request = sample_request();
    request.battle_id = 90002;
    request.initial_conditions.source_battle_id = 90001;
    request.initial_conditions.forced_first_side = gamebattle::Side::defender;
    request.initial_conditions.unit_states = {
        {.unit_id = 1001, .current_hp = 200},
        {.unit_id = 1002, .current_hp = 0}
    };

    const auto result = gamebattle::Engine{}.simulate(request);
    assert(result.source_battle_id == 90001);
    assert(result.attacker_initiative == 130); // Dead 1002 contributes no speed.
    assert(find_result_unit(result, 1001).initial_hp == 200);
    assert(find_result_unit(result, 1002).initial_hp == 0);
    assert(find_result_unit(result, 1002).hp == 0);
    assert(!find_result_unit(result, 1002).alive);
    assert(!result.events.empty());
    assert(result.events.front().type == "initiative");
    assert(result.events.front().side == gamebattle::Side::defender);
}

void test_initially_defeated_and_invalid_carryover() {
    auto defeated = sample_request();
    defeated.initial_conditions.unit_states = {
        {.unit_id = 1001, .current_hp = 0},
        {.unit_id = 1002, .current_hp = 0}
    };
    const auto result = gamebattle::Engine{}.simulate(defeated);
    assert(result.winner == gamebattle::Winner::defender);
    assert(result.reason == "initial_state");
    assert(result.rounds == 0);

    auto invalid = sample_request();
    invalid.initial_conditions.unit_states = {
        {.unit_id = 1001, .current_hp = 901}
    };
    bool rejected = false;
    try {
        static_cast<void>(gamebattle::Engine{}.simulate(invalid));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

gamebattle::term::Value simple_wire_request(
    std::int64_t max_execution_steps,
    std::int64_t max_logged_events,
    std::string log_level) {
    using gamebattle::term::Value;
    const auto unit = [](std::int64_t id, std::int64_t speed) {
        return Value::object({
            {"id", Value(id)},
            {"kind", Value::atom("hero")},
            {"position", Value(std::int64_t{1})},
            {"final_stats", Value::object({
                {"hp", Value(std::int64_t{100})},
                {"attack", Value(std::int64_t{10})},
                {"defense", Value(std::int64_t{0})},
                {"speed", Value(speed)}
            })}
        });
    };
    return Value::object({
        {"battle_id", Value(std::int64_t{95001})},
        {"seed", Value(std::int64_t{9})},
        {"max_rounds", Value(std::int64_t{1})},
        {"max_execution_steps", Value(max_execution_steps)},
        {"max_logged_events", Value(max_logged_events)},
        {"log_level", Value::atom(std::move(log_level))},
        {"attacker", Value::object({
            {"formation", Value::atom("test")},
            {"units", Value::list({unit(8001, 100)})}
        })},
        {"defender", Value::object({
            {"formation", Value::atom("test")},
            {"units", Value::list({unit(8002, 90)})}
        })}
    });
}

void test_wire_phase_one_contract() {
    using gamebattle::term::Value;
    const auto request = simple_wire_request(1000, 0, "result_only");
    const auto parsed = gamebattle::wire::parse_request(request);
    assert(parsed.max_execution_steps == 1000);
    assert(parsed.max_logged_events == 0);
    assert(parsed.event_log_level == gamebattle::EventLogLevel::result_only);

    gamebattle::wire::Handler handler;
    const auto response = gamebattle::term::decode(
        handler.handle_etf(gamebattle::term::encode(request)));
    const auto* tuple = std::get_if<Value::TupleValue>(&response.data);
    assert(tuple != nullptr && tuple->value.size() == 2);
    assert(gamebattle::term::as_string(tuple->value[0], "status") == "ok");
    const auto& result = tuple->value[1];
    assert(gamebattle::term::get_int(result, "execution_steps") > 0);
    assert(gamebattle::term::get_int(result, "total_event_count") > 0);
    assert(gamebattle::term::get_int(result, "logged_event_count") == 0);
    assert(!gamebattle::term::get_bool(result, "events_truncated"));
    assert(gamebattle::term::as_list(
               *gamebattle::term::find(result, "events"), "events").empty());

    const auto invalid_budget = gamebattle::term::decode(
        handler.handle_etf(gamebattle::term::encode(
            simple_wire_request(99, 100, "full"))));
    const auto* invalid_tuple =
        std::get_if<Value::TupleValue>(&invalid_budget.data);
    assert(invalid_tuple != nullptr && invalid_tuple->value.size() == 2);
    assert(gamebattle::term::as_string(
               invalid_tuple->value[0], "status") == "error");

    auto legacy = simple_wire_request(1000, 100, "full");
    auto* legacy_object = std::get_if<Value::ObjectValue>(&legacy.data);
    assert(legacy_object != nullptr);
    legacy_object->value.emplace_back(
        "max_events", Value(std::int64_t{1000}));
    const auto legacy_response = gamebattle::term::decode(
        handler.handle_etf(gamebattle::term::encode(legacy)));
    const auto* legacy_tuple =
        std::get_if<Value::TupleValue>(&legacy_response.data);
    assert(legacy_tuple != nullptr && legacy_tuple->value.size() == 2);
    assert(gamebattle::term::as_string(
               legacy_tuple->value[0], "status") == "error");
}

void test_term_codec_and_wire_errors() {
    const auto value = gamebattle::term::Value::object({
        {"answer", gamebattle::term::Value(std::int64_t{42})},
        {"enabled", gamebattle::term::Value::atom("true")},
        {"label", gamebattle::term::Value::binary("battle")},
        {"items", gamebattle::term::Value::list({
            gamebattle::term::Value(std::int64_t{-9}),
            gamebattle::term::Value(std::int64_t{5'000'000'000LL})
        })}
    });
    const auto encoded = gamebattle::term::encode(value);
    const auto decoded = gamebattle::term::decode(encoded);
    assert(gamebattle::term::get_int(decoded, "answer") == 42);
    assert(gamebattle::term::get_bool(decoded, "enabled"));
    assert(gamebattle::term::get_string(decoded, "label") == "battle");

    const auto ping = gamebattle::term::encode(gamebattle::term::Value::atom("ping"));
    const auto pong_bytes = gamebattle::wire::handle_etf(ping);
    const auto pong = gamebattle::term::decode(pong_bytes);
    const auto* tuple = std::get_if<gamebattle::term::Value::TupleValue>(&pong.data);
    assert(tuple != nullptr && tuple->value.size() == 2);

    const std::uint8_t invalid[] = {131, 255};
    const auto error_bytes = gamebattle::wire::handle_etf(invalid);
    const auto error = gamebattle::term::decode(error_bytes);
    const auto* error_tuple = std::get_if<gamebattle::term::Value::TupleValue>(&error.data);
    assert(error_tuple != nullptr && error_tuple->value.size() == 2);
    assert(gamebattle::term::as_string(error_tuple->value[0], "error") == "error");
}

} // namespace

int main() {
    test_wire_generic_buff_schema_and_reject_legacy_fields();
    test_wire_embedded_buff_depth_limit();
    test_core_rejects_invalid_buff_definitions();
    test_generic_modifier_composition();
    test_refresh_and_lifetime_are_deterministic();
    test_fatal_damage_runs_on_damaged_and_unit_death();
    test_reaction_priority_is_global_and_descending();
    test_buff_stack_key_keeps_appliers_independent();
    test_exhausted_reaction_does_not_consume_rng();
    test_logging_budget_never_changes_combat();
    test_execution_budget_stops_before_unlogged_mutation();
    test_engine_flow_and_determinism();
    test_initial_hp_carryover();
    test_initially_defeated_and_invalid_carryover();
    test_wire_phase_one_contract();
    test_term_codec_and_wire_errors();
    std::cout << "all gamebattle tests passed\n";
    return 0;
}
