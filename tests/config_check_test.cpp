#include "gamebattle/config_check.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using gamebattle::Effect;
using gamebattle::EffectKind;
using gamebattle::Passive;
using gamebattle::TargetRule;
using gamebattle::Trigger;
using gamebattle::config_check::Definitions;
using gamebattle::config_check::Options;
using gamebattle::config_check::Report;
using gamebattle::config_check::Severity;

Effect effect(EffectKind kind, TargetRule target = TargetRule::trigger_unit) {
    Effect result;
    result.kind = kind;
    result.target = target;
    result.attack_bp = kind == EffectKind::add_buff ? 0 : 5000;
    return result;
}

Passive passive(std::uint32_t id, Trigger trigger, std::int32_t cap,
                std::vector<Effect> effects, gamebattle::BasisPoints chance = 10000) {
    Passive result;
    result.id = id;
    result.name = "p" + std::to_string(id);
    result.trigger = trigger;
    result.max_triggers_per_round = cap;
    result.chance_bp = chance;
    result.effects = std::move(effects);
    return result;
}

std::size_t count(const Report& report, Severity severity) {
    std::size_t result = 0;
    for (const auto& finding : report.findings) {
        result += finding.severity == severity ? 1 : 0;
    }
    return result;
}

bool mentions(const Report& report, Severity severity, const std::string& text) {
    for (const auto& finding : report.findings) {
        if (finding.severity == severity && finding.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

Options static_only() {
    Options options;
    options.stress = false;
    return options;
}

void test_unlimited_counterattack_is_an_error() {
    Definitions definitions;
    definitions.passives.push_back(
        passive(801, Trigger::on_damaged, 0, {effect(EffectKind::damage)}));
    const auto report = gamebattle::config_check::check(definitions, static_only());
    assert(report.has_errors());
    assert(mentions(report, Severity::error,
                    "passive 801 \"p801\" (on_damaged, no limit) deals damage, which sets off "
                    "on_damaged"));
    assert(gamebattle::config_check::format(report).rfind("error: runaway loop", 0) == 0);
}

void test_a_limit_anywhere_in_the_loop_ends_it() {
    Definitions definitions;
    definitions.passives.push_back(
        passive(801, Trigger::on_damaged, 1, {effect(EffectKind::damage)}));
    // Unlimited, but its direct damage sets off only on_damaged: it closes a
    // loop only through 801, which is limited.
    definitions.passives.push_back(
        passive(802, Trigger::on_hit, 0, {effect(EffectKind::direct_damage)}));
    const auto report = gamebattle::config_check::check(definitions, static_only());
    assert(!report.has_errors());
    assert(count(report, Severity::note) == 1);
    assert(mentions(report, Severity::note, "passive 801"));
    assert(mentions(report, Severity::note, "passive 802"));
}

void test_triggers_that_cannot_repeat_are_not_loops() {
    Definitions definitions;
    // A unit dies once.
    definitions.passives.push_back(
        passive(801, Trigger::unit_death, 0, {effect(EffectKind::damage, TargetRule::all_enemies)}));
    // Responses join a chain at most once per unit; nothing deals damage to
    // set them off again.
    definitions.passives.push_back(
        passive(802, Trigger::enemy_activate, 0, {effect(EffectKind::damage)}));
    // Healing and buffs set off no trigger.
    definitions.passives.push_back(
        passive(803, Trigger::on_damaged, 0, {effect(EffectKind::heal, TargetRule::self)}));
    // Never fires.
    definitions.passives.push_back(
        passive(804, Trigger::on_hit, 0, {effect(EffectKind::damage)}, 0));
    const auto report = gamebattle::config_check::check(definitions, static_only());
    assert(!report.has_errors());
    assert(!mentions(report, Severity::note, "passive 802"));
    assert(!mentions(report, Severity::note, "passive 803"));
    assert(!mentions(report, Severity::note, "passive 804"));
}

void test_buff_reactions_are_found_through_add_buff() {
    auto thorns = std::make_shared<gamebattle::BuffSpec>();
    thorns->id = 901;
    thorns->name = "thorns";
    gamebattle::BuffReaction reflect;
    reflect.trigger = Trigger::on_damaged;
    reflect.effects.push_back(effect(EffectKind::damage));
    thorns->reactions.push_back(std::move(reflect));
    auto add = effect(EffectKind::add_buff, TargetRule::self);
    add.buff = thorns;

    Definitions definitions;
    definitions.passives.push_back(passive(801, Trigger::battle_start, 0, {add}));
    const auto report = gamebattle::config_check::check(definitions, static_only());
    assert(report.has_errors());
    assert(mentions(report, Severity::error, "buff 901 \"thorns\" reaction 1 (on_damaged, no limit)"));
}

void test_stress_battle_measures_the_cascade() {
    Definitions limited;
    limited.passives.push_back(
        passive(801, Trigger::on_damaged, 3, {effect(EffectKind::damage)}));
    Options options;
    options.battles = 1;
    options.rounds = 2;
    const auto fine = gamebattle::config_check::check(limited, options);
    assert(!fine.has_errors());
    assert(count(fine, Severity::warning) == 0);
    assert(mentions(fine, Severity::note, "stress battle (7v7, every unit with all 1 passive and 0 skills, nobody dies, 2 rounds)"));

    // A counterattack on every enemy: each one sets off seven more, so the
    // cascade outgrows max_events long before the trigger depth limit.
    Definitions runaway;
    runaway.passives.push_back(
        passive(801, Trigger::on_damaged, 0, {effect(EffectKind::damage, TargetRule::all_enemies)}));
    options.max_events = 20000;
    const auto broken = gamebattle::config_check::check(runaway, options);
    assert(mentions(broken, Severity::error, "reached max_events (20,000) in round 1"));
    assert(mentions(broken, Severity::error, "passive 801 x"));

    // Bounded, but each step sets off more events than the threshold.
    options.step_warning = 5;
    options.max_events = 200000;
    const auto busy = gamebattle::config_check::check(limited, options);
    assert(!busy.has_errors());
    assert(mentions(busy, Severity::warning, "a single step of the stress battle"));
}

} // namespace

int main() {
    test_unlimited_counterattack_is_an_error();
    test_a_limit_anywhere_in_the_loop_ends_it();
    test_triggers_that_cannot_repeat_are_not_loops();
    test_buff_reactions_are_found_through_add_buff();
    test_stress_battle_measures_the_cascade();
    std::cout << "all config check tests passed\n";
    return 0;
}
