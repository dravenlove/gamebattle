#include "battle_runtime.hpp"

#include <algorithm>
#include <array>

namespace gamebattle::runtime {

BattleRunner::BattleRunner(const BattleRequest& request)
    : state_(request), effects_(state_) {}

BattleResult BattleRunner::run() {
    state_.result.attacker_initiative =
        state_.initiative(Side::attacker,
                          state_.request.attacker.initiative_bonus);
    state_.result.defender_initiative =
        state_.initiative(Side::defender,
                          state_.request.defender.initiative_bonus);

    if (state_.request.initial_conditions.forced_first_side.has_value()) {
        state_.first_side =
            *state_.request.initial_conditions.forced_first_side;
    } else if (state_.result.attacker_initiative ==
               state_.result.defender_initiative) {
        state_.first_side = (state_.random.next() & 1U) == 0
                                ? Side::attacker
                                : Side::defender;
    } else {
        state_.first_side =
            state_.result.attacker_initiative >
                    state_.result.defender_initiative
                ? Side::attacker
                : Side::defender;
    }
    const auto initiative_context = state_.make_event_context(
        Trigger::battle_start, 0, 0, 0, 0);
    state_.emit(
        initiative_context, "battle", "initiative", state_.first_side,
        0, 0, 0,
        static_cast<std::int64_t>(
            state_.first_side == Side::attacker
                ? state_.result.attacker_initiative
                : state_.result.defender_initiative));

    // Inherited dead units are part of the formation snapshot, but do not fire
    // battle-start passives. A fully dead side therefore ends immediately.
    if (state_.finish_if_decided("initial_state")) {
        return state_.finish();
    }

    effects_.trigger_all(Trigger::battle_start, std::nullopt);
    if (state_.execution_limit) {
        state_.result.winner = Winner::draw;
        state_.result.reason = "execution_limit";
        state_.result.rounds = 0;
        return state_.finish();
    }
    if (state_.finish_if_decided("battle_start")) {
        return state_.finish();
    }

    for (state_.round = 1;
         state_.round <= state_.request.max_rounds && !state_.execution_limit;
         ++state_.round) {
        state_.reset_round_trigger_counts();
        state_.phase = "round_start";
        effects_.trigger_all(Trigger::round_start, std::nullopt);
        if (state_.execution_limit) {
            break;
        }
        if (state_.finish_if_decided("round_start")) {
            break;
        }

        const std::array<Side, 2> order{
            state_.first_side, other(state_.first_side)};
        for (const Side side : order) {
            if (state_.side_defeated(side) || state_.execution_limit) {
                continue;
            }
            state_.phase =
                side == state_.first_side ? "first_side" : "second_side";
            take_side_turn(side);
            if (state_.finish_if_decided("all_units_defeated")) {
                break;
            }
        }
        if (state_.decided || state_.execution_limit) {
            break;
        }

        state_.phase = "round_end";
        effects_.trigger_all(Trigger::round_end, std::nullopt);
        if (state_.execution_limit) {
            break;
        }
        if (state_.finish_if_decided("round_end")) {
            break;
        }
    }

    if (!state_.decided) {
        state_.result.winner = Winner::draw;
        state_.result.reason =
            state_.execution_limit ? "execution_limit" : "max_rounds";
        state_.result.rounds =
            std::min(state_.round, state_.request.max_rounds);
    }
    return state_.finish();
}

void BattleRunner::take_side_turn(Side side) {
    // Snapshot the order for one side turn. Speed changes affect later side
    // turns without mutating the action list currently being traversed.
    const auto order = state_.acting_order(side);
    for (const auto actor_index : order) {
        if (state_.execution_limit || state_.side_defeated(other(side))) {
            return;
        }
        auto& actor = state_.units[actor_index];
        if (!actor.alive()) {
            continue;
        }

        effects_.trigger_owner(actor_index, Trigger::before_action,
                               actor_index, 0);
        if (state_.execution_limit) {
            return;
        }
        if (!actor.alive()) {
            continue;
        }

        const auto action_start_context = state_.make_event_context(
            Trigger::on_attack, actor.config.id, 0, actor.config.id, 0);
        state_.emit(action_start_context, state_.phase, "action_start", side,
                    actor.config.id, 0, 0, 0);
        effects_.execute_action(actor_index);
        if (state_.execution_limit) {
            return;
        }
        if (actor.alive()) {
            effects_.trigger_owner(actor_index, Trigger::after_action,
                                   actor_index, 0);
        }
        if (state_.execution_limit) {
            return;
        }
        const auto action_end_context = state_.make_event_context(
            Trigger::after_action, actor.config.id, 0, actor.config.id, 0);
        state_.emit(action_end_context, state_.phase, "action_end", side,
                    actor.config.id, 0, 0, 0);
    }
}

} // namespace gamebattle::runtime

namespace gamebattle {

BattleResult Engine::simulate(const BattleRequest& request) const {
    return runtime::BattleRunner(request).run();
}

} // namespace gamebattle
