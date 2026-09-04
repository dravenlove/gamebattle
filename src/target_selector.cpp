#include "battle_runtime.hpp"

#include <algorithm>

namespace gamebattle::runtime {

std::vector<std::size_t> TargetSelector::select(
    BattleState& state,
    std::size_t owner_index,
    TargetRule rule,
    std::int32_t requested_count,
    std::optional<std::size_t> trigger_unit) {
    if (rule == TargetRule::self) {
        return state.units[owner_index].alive()
                   ? std::vector<std::size_t>{owner_index}
                   : std::vector<std::size_t>{};
    }
    if (rule == TargetRule::trigger_unit) {
        if (trigger_unit.has_value() && *trigger_unit < state.units.size() &&
            state.units[*trigger_unit].alive() &&
            state.units[*trigger_unit].config.targetable) {
            return {*trigger_unit};
        }
        return {};
    }

    const Side target_side =
        rule == TargetRule::ally_lowest_hp || rule == TargetRule::all_allies
            ? state.units[owner_index].side
            : other(state.units[owner_index].side);
    std::vector<std::size_t> candidates;
    for (std::size_t index = 0; index < state.units.size(); ++index) {
        if (state.units[index].side == target_side && state.units[index].alive() &&
            state.units[index].config.targetable) {
            candidates.push_back(index);
        }
    }

    if (rule == TargetRule::enemy_lowest_hp ||
        rule == TargetRule::ally_lowest_hp) {
        std::sort(candidates.begin(), candidates.end(),
                  [&](std::size_t left, std::size_t right) {
                      const auto& lhs = state.units[left];
                      const auto& rhs = state.units[right];
                      const auto ratio = [](std::int64_t hp, std::int64_t maximum) {
                          return (hp / maximum) * kBasisPoints +
                                 ((hp % maximum) * kBasisPoints) / maximum;
                      };
                      const auto lhs_ratio =
                          ratio(lhs.hp, lhs.config.final_stats.hp);
                      const auto rhs_ratio =
                          ratio(rhs.hp, rhs.config.final_stats.hp);
                      if (lhs_ratio != rhs_ratio) {
                          return lhs_ratio < rhs_ratio;
                      }
                      if (lhs.config.position != rhs.config.position) {
                          return lhs.config.position < rhs.config.position;
                      }
                      return lhs.config.id < rhs.config.id;
                  });
    } else {
        std::sort(candidates.begin(), candidates.end(),
                  [&](std::size_t left, std::size_t right) {
                      if (state.units[left].config.position !=
                          state.units[right].config.position) {
                          return state.units[left].config.position <
                                 state.units[right].config.position;
                      }
                      return state.units[left].config.id <
                             state.units[right].config.id;
                  });
    }

    if (rule != TargetRule::all_enemies && rule != TargetRule::all_allies) {
        const auto count = static_cast<std::size_t>(std::max(0, requested_count));
        if (candidates.size() > count) {
            candidates.resize(count);
        }
    }
    return candidates;
}

} // namespace gamebattle::runtime
