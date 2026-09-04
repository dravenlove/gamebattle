#include "gamebattle/config_store.hpp"
#include "gamebattle/term.hpp"
#include "gamebattle/wire.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <utility>

namespace {

gamebattle::term::Value configured_request_term() {
    using gamebattle::term::Value;
    const auto stats = [](std::int64_t hp, std::int64_t attack,
                          std::int64_t defense, std::int64_t speed) {
        return Value::object({
            {"hp", Value(hp)}, {"attack", Value(attack)},
            {"defense", Value(defense)}, {"speed", Value(speed)}
        });
    };
    const auto unit = [&](std::int64_t id, std::int64_t position,
                          Value final_stats, bool configured) {
        Value::Object fields{
            {"id", Value(id)},
            {"kind", Value::atom("hero")},
            {"position", Value(position)},
            {"final_stats", std::move(final_stats)}
        };
        if (configured) {
            fields.emplace_back("skill_ids", Value::list({Value(std::int64_t{501})}));
            fields.emplace_back("passive_ids", Value::list({Value(std::int64_t{701})}));
        }
        return Value::object(std::move(fields));
    };
    return Value::object({
        {"battle_id", Value(std::int64_t{91001})},
        {"seed", Value(std::int64_t{12345})},
        {"attacker", Value::object({
            {"formation", Value::atom("test")},
            {"units", Value::list({unit(3001, 1, stats(1000, 200, 20, 120), true)})}
        })},
        {"defender", Value::object({
            {"formation", Value::atom("test")},
            {"units", Value::list({unit(4001, 1, stats(1000, 150, 30, 100), false)})}
        })}
    });
}

void test_config_store_and_wire_ids() {
    const auto store = gamebattle::ConfigStore::load_file(GAMEBATTLE_TEST_CONFIG_PATH);
    assert(store.buff_count() == 2);
    assert(store.effect_count() == 5);
    assert(store.skill_count() == 1);
    assert(store.passive_count() == 3);
    assert(store.require_buff(801).lifetime.decrement_on ==
           gamebattle::Trigger::round_end);
    assert(store.require_buff(801).reactions.size() == 1);
    assert(store.require_buff(801).reactions.front().stack_scaling ==
           gamebattle::StackScaling::per_stack);
    assert(store.require_buff(801).reactions.front().effects.front().kind ==
           gamebattle::EffectKind::direct_damage);
    assert(store.require_buff(802).modifiers.size() == 1);
    assert(store.require_buff(802).modifiers.front().attribute ==
           gamebattle::Attribute::attack);
    assert(store.require_skill(501).effects.size() == 1);
    assert(store.require_passive(701).effects.front().buff != nullptr);
    assert(store.require_passive(701).effects.front().buff->id == 801);

    gamebattle::UnitConfig configured;
    const std::uint32_t skill_ids[] = {501};
    const std::uint32_t passive_ids[] = {701};
    store.assign_loadout(configured, skill_ids, passive_ids);
    assert(configured.skills.front().id == 501);
    assert(configured.passives.front().id == 701);

    const auto parsed = gamebattle::wire::parse_request(configured_request_term(), &store);
    assert(parsed.attacker.units.front().skills.front().id == 501);
    assert(parsed.attacker.units.front().passives.front().id == 701);

    gamebattle::wire::Handler handler;
    const auto no_config = handler.handle_etf(
        gamebattle::term::encode(configured_request_term()));
    const auto no_config_term = gamebattle::term::decode(no_config);
    const auto* no_config_tuple =
        std::get_if<gamebattle::term::Value::TupleValue>(&no_config_term.data);
    assert(no_config_tuple != nullptr);
    assert(gamebattle::term::as_string(no_config_tuple->value[0], "status") == "error");

    const auto load = gamebattle::term::Value::tuple({
        gamebattle::term::Value::atom("load_config"),
        gamebattle::term::Value::binary(GAMEBATTLE_TEST_CONFIG_PATH)
    });
    const auto loaded = gamebattle::term::decode(
        handler.handle_etf(gamebattle::term::encode(load)));
    const auto* loaded_tuple =
        std::get_if<gamebattle::term::Value::TupleValue>(&loaded.data);
    assert(loaded_tuple != nullptr);
    assert(gamebattle::term::as_string(loaded_tuple->value[0], "status") == "ok");

    const auto failed_reload = gamebattle::term::Value::tuple({
        gamebattle::term::Value::atom("load_config"),
        gamebattle::term::Value::binary("missing-config.gbcfg")
    });
    const auto failed = gamebattle::term::decode(
        handler.handle_etf(gamebattle::term::encode(failed_reload)));
    const auto* failed_tuple =
        std::get_if<gamebattle::term::Value::TupleValue>(&failed.data);
    assert(failed_tuple != nullptr);
    assert(gamebattle::term::as_string(failed_tuple->value[0], "status") == "error");

    // Failed reload must not replace the last known-good configuration.
    const auto response = gamebattle::term::decode(handler.handle_etf(
        gamebattle::term::encode(configured_request_term())));
    const auto* response_tuple =
        std::get_if<gamebattle::term::Value::TupleValue>(&response.data);
    assert(response_tuple != nullptr);
    assert(gamebattle::term::as_string(response_tuple->value[0], "status") == "ok");
}

} // namespace

int main() {
    test_config_store_and_wire_ids();
    std::cout << "all gamebattle config tests passed\n";
    return 0;
}
