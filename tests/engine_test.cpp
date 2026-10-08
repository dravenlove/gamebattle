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
    request.max_events = 2000;

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
            {"refresh", Value::atom("extend")}
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
                    {"refresh", Value::atom("reset")}
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
            {"refresh", Value::atom("reset")}
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
                {"refresh", Value::atom("reset")}
            })},
            {"modifiers", Value::list({})},
            {"reactions", Value::list({Value::object({
                {"trigger", Value::atom("round_end")},
                {"source", Value::atom("owner")},
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
        request.max_events = 1000;

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
    request.max_events = 1000;

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
    request.max_events = 1000;

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

// Chain tests use 100% hit, 0% crit and 100% trigger chances, so they consume
// no random numbers and every damage number below is exact.
gamebattle::Skill big_hit() {
    gamebattle::Skill skill;
    skill.id = 5101;
    skill.name = "big_hit";
    skill.priority = 10;
    skill.effects.push_back(gamebattle::Effect{
        .kind = gamebattle::EffectKind::damage,
        .target = gamebattle::TargetRule::enemy_front,
        .target_count = 1,
        .attack_bp = 30000
    });
    return skill;
}

gamebattle::Effect negate_effect() {
    gamebattle::Effect effect;
    effect.kind = gamebattle::EffectKind::negate;
    effect.target = gamebattle::TargetRule::trigger_unit;
    effect.attack_bp = 0;
    return effect;
}

gamebattle::Passive response(std::uint32_t id, gamebattle::Trigger trigger,
                             gamebattle::Effect effect) {
    gamebattle::Passive passive;
    passive.id = id;
    passive.name = "response";
    passive.trigger = trigger;
    passive.effects.push_back(std::move(effect));
    return passive;
}

// Attacker 5001 (speed 200, attack 100) acts first with big_hit (300 damage);
// defender 6001 has 5000 hp and no skills. One round only.
gamebattle::BattleRequest chain_request() {
    gamebattle::BattleRequest request;
    request.battle_id = 93001;
    request.seed = 7;
    request.max_rounds = 1;
    request.max_events = 1000;
    auto attacker = hero(5001, 1, 200, 1000, 100, 0);
    attacker.skills.push_back(big_hit());
    request.attacker.units.push_back(std::move(attacker));
    request.defender.units.push_back(hero(6001, 1, 100, 5000, 50, 0));
    return request;
}

const gamebattle::Event* find_event(const gamebattle::BattleResult& result,
                                    const std::string& type,
                                    gamebattle::UnitId actor) {
    for (const auto& event : result.events) {
        if (event.type == type && event.actor == actor) {
            return &event;
        }
    }
    return nullptr;
}

std::size_t count_events(const gamebattle::BattleResult& result,
                         const std::string& type, std::uint32_t source_id) {
    std::size_t count = 0;
    for (const auto& event : result.events) {
        count += event.type == type && event.source_id == source_id ? 1 : 0;
    }
    return count;
}

void test_chain_negate_and_counter_negate() {
    // The defender answers the skill with a negate: the skill does nothing.
    auto request = chain_request();
    request.defender.units[0].passives.push_back(
        response(7601, gamebattle::Trigger::enemy_activate, negate_effect()));
    const auto negated = gamebattle::Engine{}.simulate(request);
    const auto* chain = find_event(negated, "chain", 6001);
    const auto* negate = find_event(negated, "negate", 6001);
    assert(chain != nullptr && chain->target == 5001 && chain->source_id == 7601 &&
           chain->value == 2);
    assert(negate != nullptr && negate->target == 5001 && negate->source_id == 5101 &&
           negate->value == 1 && chain->seq < negate->seq);
    assert(count_events(negated, "damage", 5101) == 0);

    // An attacker-side ally answers the negate with its own negate. Links
    // resolve 3 -> 2 -> 1: link 3 cancels link 2, so the skill (link 1) hits.
    auto ally = hero(5002, 2, 150, 1000, 100, 0);
    ally.passives.push_back(
        response(7602, gamebattle::Trigger::enemy_activate, negate_effect()));
    request.attacker.units.push_back(std::move(ally));
    const auto countered = gamebattle::Engine{}.simulate(request);
    const auto* first = find_event(countered, "chain", 6001);
    const auto* second = find_event(countered, "chain", 5002);
    const auto* cancel = find_event(countered, "negate", 5002);
    assert(first != nullptr && first->value == 2);
    assert(second != nullptr && second->target == 6001 && second->value == 3);
    assert(cancel != nullptr && cancel->target == 6001 && cancel->source_id == 7601 &&
           cancel->value == 2);
    assert(find_event(countered, "negate", 6001) == nullptr);
    assert(count_events(countered, "damage", 5101) == 1);
    const auto* hit = find_event(countered, "damage", 5001);
    assert(hit != nullptr && hit->source_id == 5101 && hit->value == 300);
    assert(first->seq < second->seq && second->seq < cancel->seq &&
           cancel->seq < hit->seq);

    const auto again = gamebattle::Engine{}.simulate(request);
    assert(gamebattle::term::encode(gamebattle::wire::encode_result(countered)) ==
           gamebattle::term::encode(gamebattle::wire::encode_result(again)));
}

void test_chain_ally_support_resolves_first() {
    // An ally answers the skill with an attack buff on the caster. Its link
    // resolves before the skill, so the skill hits with the buffed attack.
    auto rally = std::make_shared<gamebattle::BuffSpec>();
    rally->id = 8601;
    rally->name = "rally";
    rally->modifiers.push_back({.attribute = gamebattle::Attribute::attack,
                                .operation = gamebattle::ModifierOperation::add,
                                .value = 1000});
    gamebattle::Effect add_rally;
    add_rally.kind = gamebattle::EffectKind::add_buff;
    add_rally.target = gamebattle::TargetRule::trigger_unit;
    add_rally.attack_bp = 0;
    add_rally.buff = rally;

    auto request = chain_request();
    auto ally = hero(5002, 2, 150, 1000, 100, 0);
    ally.passives.push_back(
        response(7603, gamebattle::Trigger::ally_activate, std::move(add_rally)));
    request.attacker.units.push_back(std::move(ally));
    const auto result = gamebattle::Engine{}.simulate(request);

    const auto* chain = find_event(result, "chain", 5002);
    const auto* buffed = find_event(result, "buff_add", 5002);
    const auto* hit = find_event(result, "damage", 5001);
    assert(chain != nullptr && chain->target == 5001 && chain->value == 2);
    assert(buffed != nullptr && buffed->target == 5001);
    assert(hit != nullptr && hit->source_id == 5101 && hit->value == 3300);
    assert(chain->seq < buffed->seq && buffed->seq < hit->seq);
}

void test_chain_fizzles_when_caster_dies() {
    // The defender's response kills the caster before the skill resolves.
    auto request = chain_request();
    request.defender.units[0].passives.push_back(response(
        7604, gamebattle::Trigger::enemy_activate,
        gamebattle::Effect{.kind = gamebattle::EffectKind::damage,
                           .target = gamebattle::TargetRule::trigger_unit,
                           .target_count = 1,
                           .attack_bp = 1'000'000}));
    const auto result = gamebattle::Engine{}.simulate(request);
    const auto* death = find_event(result, "death", 6001);
    const auto* fizzle = find_event(result, "fizzle", 5001);
    assert(death != nullptr && death->target == 5001);
    assert(fizzle != nullptr && fizzle->source_id == 5101 && fizzle->value == 1 &&
           death->seq < fizzle->seq);
    assert(count_events(result, "damage", 5101) == 0);
    assert(result.winner == gamebattle::Winner::defender);
}

void test_chain_rules_are_validated() {
    const auto rejected = [](const gamebattle::BattleRequest& request) {
        try {
            static_cast<void>(gamebattle::Engine{}.simulate(request));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    auto negate_skill = chain_request();
    negate_skill.attacker.units[0].skills[0].effects.push_back(negate_effect());
    assert(rejected(negate_skill));

    auto negate_on_hit = chain_request();
    negate_on_hit.attacker.units[0].passives.push_back(
        response(7605, gamebattle::Trigger::on_hit, negate_effect()));
    assert(rejected(negate_on_hit));

    const auto with_buff = [](gamebattle::Trigger reaction_trigger,
                              gamebattle::Trigger decrement_on) {
        auto buff = std::make_shared<gamebattle::BuffSpec>();
        buff->id = 8602;
        buff->name = "response_buff";
        buff->lifetime.decrement_on = decrement_on;
        gamebattle::BuffReaction reaction;
        reaction.trigger = reaction_trigger;
        reaction.effects.push_back(gamebattle::Effect{});
        buff->reactions.push_back(std::move(reaction));
        gamebattle::Effect add;
        add.kind = gamebattle::EffectKind::add_buff;
        add.target = gamebattle::TargetRule::self;
        add.buff = std::move(buff);
        auto request = chain_request();
        request.attacker.units[0].passives.push_back(
            response(7606, gamebattle::Trigger::battle_start, std::move(add)));
        return request;
    };
    assert(!rejected(with_buff(gamebattle::Trigger::round_end,
                               gamebattle::Trigger::round_end)));
    assert(rejected(with_buff(gamebattle::Trigger::enemy_activate,
                              gamebattle::Trigger::round_end)));
    assert(rejected(with_buff(gamebattle::Trigger::round_end,
                              gamebattle::Trigger::ally_activate)));
}

void test_wire_parses_chain_names() {
    using gamebattle::term::Value;
    const auto unit = [](std::int64_t id, std::int64_t speed, Value::List skills,
                         Value::List passives) {
        return Value::object({
            {"id", Value(id)},
            {"kind", Value::atom("hero")},
            {"position", Value(std::int64_t{1})},
            {"final_stats", Value::object({
                {"hp", Value(std::int64_t{5000})},
                {"attack", Value(std::int64_t{100})},
                {"defense", Value(std::int64_t{0})},
                {"speed", Value(speed)}
            })},
            {"skills", Value::list(std::move(skills))},
            {"passives", Value::list(std::move(passives))}
        });
    };
    Value skill = Value::object({
        {"id", Value(std::int64_t{5101})},
        {"name", Value::binary("big_hit")},
        {"effects", Value::list({Value::object({
            {"type", Value::atom("damage")},
            {"attack_bp", Value(std::int64_t{30000})}
        })})}
    });
    Value counter = Value::object({
        {"id", Value(std::int64_t{7601})},
        {"name", Value::binary("counter_spell")},
        {"trigger", Value::atom("enemy_activate")},
        {"effects", Value::list({Value::object({
            {"type", Value::atom("negate")},
            {"target", Value::atom("trigger_unit")}
        })})}
    });
    const auto request = Value::object({
        {"battle_id", Value(std::int64_t{93002})},
        {"seed", Value(std::int64_t{7})},
        {"max_rounds", Value(std::int64_t{1})},
        {"attacker", Value::object({
            {"formation", Value::atom("test")},
            {"units", Value::list({unit(5001, 200, Value::List{std::move(skill)}, {})})}
        })},
        {"defender", Value::object({
            {"formation", Value::atom("test")},
            {"units", Value::list({unit(6001, 100, {}, Value::List{std::move(counter)})})}
        })}
    });
    const auto parsed = gamebattle::wire::parse_request(request);
    const auto& passive = parsed.defender.units.front().passives.front();
    assert(passive.trigger == gamebattle::Trigger::enemy_activate);
    assert(passive.effects.front().kind == gamebattle::EffectKind::negate);
    const auto result = gamebattle::Engine{}.simulate(parsed);
    assert(find_event(result, "negate", 6001) != nullptr);
}

} // namespace

int main() {
    test_wire_generic_buff_schema_and_reject_legacy_fields();
    test_wire_embedded_buff_depth_limit();
    test_core_rejects_invalid_buff_definitions();
    test_generic_modifier_composition();
    test_refresh_and_lifetime_are_deterministic();
    test_engine_flow_and_determinism();
    test_initial_hp_carryover();
    test_initially_defeated_and_invalid_carryover();
    test_term_codec_and_wire_errors();
    test_chain_negate_and_counter_negate();
    test_chain_ally_support_resolves_first();
    test_chain_fizzles_when_caster_dies();
    test_chain_rules_are_validated();
    test_wire_parses_chain_names();
    std::cout << "all gamebattle tests passed\n";
    return 0;
}
