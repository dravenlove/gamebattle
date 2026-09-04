#pragma once

#include "gamebattle/engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gamebattle::runtime {

inline constexpr std::int64_t kBasisPoints = 10000;
inline constexpr std::size_t kMaxTriggerDepth = 32;
inline constexpr std::size_t kTriggerCount =
    static_cast<std::size_t>(Trigger::round_end) + 1;

Side other(Side side);
std::int64_t saturating_add(std::int64_t left, std::int64_t right);
std::int64_t scale(std::int64_t value, std::int64_t basis_points);

class Random {
public:
    explicit Random(std::uint64_t seed);
    std::uint64_t next();
    bool roll(BasisPoints chance_bp);

private:
    std::uint64_t state_;
};

struct ActiveBuff {
    std::shared_ptr<const BuffSpec> definition;
    std::uint64_t instance_id{0};
    std::int32_t remaining{0};
    std::int32_t stacks{1};
    UnitId source{0};
    std::vector<std::int32_t> reaction_triggers;
};

struct RuntimeUnit {
    UnitConfig config;
    Side side{Side::attacker};
    std::int64_t initial_hp{0};
    std::int64_t hp{0};
    std::vector<ActiveBuff> buffs;
    std::unordered_map<std::uint32_t, std::int32_t> passive_triggers;
    std::array<std::vector<std::size_t>, kTriggerCount> passives_by_trigger;
    Stats cached_stats;
    bool stats_dirty{true};

    bool alive() const { return hp > 0; }
};

// Owns all mutable data for exactly one battle. It has no rule ordering and no
// transport knowledge; BattleRunner and EffectSystem operate on this state.
class BattleState {
public:
    explicit BattleState(const BattleRequest& request);

    Stats effective_stats(std::size_t unit_index);
    void mark_stats_dirty(std::size_t unit_index);
    std::uint64_t initiative(Side side, std::int64_t bonus) const;
    std::vector<std::size_t> acting_order(Side side);
    std::optional<std::size_t> find_unit(UnitId id) const;
    bool side_defeated(Side side) const;
    bool finish_if_decided(std::string reason);
    void reset_round_trigger_counts();
    void emit(std::string phase, std::string type, Side side, UnitId actor, UnitId target,
              std::uint32_t source_id, std::int64_t value,
              std::int64_t hp_before = 0, std::int64_t hp_after = 0,
              bool critical = false);
    BattleResult finish();

    const BattleRequest& request;
    Random random;
    BattleResult result;
    std::vector<RuntimeUnit> units;
    std::unordered_map<UnitId, std::size_t> unit_index;
    Side first_side{Side::attacker};
    std::int32_t round{0};
    std::string phase{"battle"};
    bool decided{false};
    bool event_limit{false};
    std::uint64_t next_buff_instance_id{1};

private:
    void add_formation(const Formation& formation, Side side);
    void apply_initial_conditions();
};

// Target selection is deliberately read-mostly and independent from effect
// execution so formation/position algorithms can evolve without changing the
// damage or passive system.
class TargetSelector {
public:
    static std::vector<std::size_t> select(
        BattleState& state,
        std::size_t owner_index,
        TargetRule rule,
        std::int32_t requested_count,
        std::optional<std::size_t> trigger_unit);
};

// Executes declarative skills, passives and buffs. It owns rule recursion but
// not the round/side schedule.
class EffectSystem {
public:
    explicit EffectSystem(BattleState& state);

    void execute_action(std::size_t actor_index);
    void trigger_owner(std::size_t owner_index, Trigger trigger,
                       std::optional<std::size_t> event_unit,
                       std::uint32_t source_id, std::size_t depth = 0);
    void trigger_all(Trigger trigger, std::optional<std::size_t> event_unit,
                     std::uint32_t source_id = 0, std::size_t depth = 0);

private:
    void execute_effects(std::size_t source_index,
                         std::size_t selection_owner_index,
                         const std::vector<Effect>& effects,
                         std::uint32_t source_id, std::size_t depth,
                         std::optional<std::size_t> trigger_unit,
                         std::int32_t magnitude_stacks = 1);
    void apply_damage(std::size_t actor_index, std::size_t target_index,
                      const Effect& effect, std::uint32_t source_id,
                      std::size_t depth);
    void apply_heal(std::size_t actor_index, std::size_t target_index,
                    const Effect& effect, std::uint32_t source_id);
    void apply_buff(std::size_t actor_index, std::size_t target_index,
                    std::shared_ptr<const BuffSpec> definition,
                    std::uint32_t source_id);
    void remove_buff(std::size_t actor_index, std::size_t target_index,
                     std::uint32_t buff_id, std::uint32_t source_id);
    void trigger_owner_snapshot(std::size_t owner_index, Trigger trigger,
                                std::optional<std::size_t> event_unit,
                                std::uint32_t source_id, std::size_t depth,
                                std::uint64_t buff_instance_cutoff);

    BattleState& state_;
};

// Defines the high-level battle schedule. All ordering decisions live here,
// while EffectSystem handles mutations caused by one action or trigger.
class BattleRunner {
public:
    explicit BattleRunner(const BattleRequest& request);
    BattleResult run();

private:
    void take_side_turn(Side side);

    BattleState state_;
    EffectSystem effects_;
};

} // namespace gamebattle::runtime
