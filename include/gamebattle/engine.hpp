#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gamebattle {

using UnitId = std::uint64_t;
using BasisPoints = std::int32_t;

enum class Side : std::uint8_t { attacker = 0, defender = 1 };
enum class UnitKind : std::uint8_t { hero, beauty, pet, artifact };
enum class TargetRule : std::uint8_t {
    self,
    trigger_unit,
    enemy_front,
    enemy_lowest_hp,
    ally_lowest_hp,
    all_enemies,
    all_allies
};
enum class Trigger : std::uint8_t {
    battle_start,
    round_start,
    before_action,
    on_attack,
    on_hit,
    on_damaged,
    unit_death,
    after_action,
    round_end
};
enum class EffectKind : std::uint8_t {
    damage = 0,
    heal = 1,
    add_buff = 2,
    remove_buff = 3,
    direct_damage = 4
};
enum class Attribute : std::uint8_t {
    attack = 0,
    defense = 1,
    speed = 2,
    crit_rate_bp = 3,
    crit_damage_bp = 4,
    hit_rate_bp = 5,
    dodge_rate_bp = 6,
    damage_bonus_bp = 7,
    damage_reduction_bp = 8
};
enum class ModifierOperation : std::uint8_t { add = 0, scale_bp = 1 };
enum class StackPolicy : std::uint8_t { stack = 0, refresh = 1 };
enum class RefreshPolicy : std::uint8_t { reset = 0, extend = 1, keep = 2 };
enum class EffectSource : std::uint8_t { owner = 0, applier = 1 };
enum class StackScaling : std::uint8_t { once = 0, per_stack = 1 };

struct Stats {
    std::int64_t hp{1};
    std::int64_t attack{0};
    std::int64_t defense{0};
    std::int64_t speed{0};
    BasisPoints crit_rate_bp{0};
    BasisPoints crit_damage_bp{15000};
    BasisPoints hit_rate_bp{10000};
    BasisPoints dodge_rate_bp{0};
    BasisPoints damage_bonus_bp{0};
    BasisPoints damage_reduction_bp{0};
};

struct AttributeModifier {
    Attribute attribute{Attribute::attack};
    ModifierOperation operation{ModifierOperation::add};
    std::int64_t value{0};
};

struct LifetimePolicy {
    bool permanent{false};
    std::int32_t duration{1};
    Trigger decrement_on{Trigger::round_end};
};

struct StackingPolicy {
    std::int32_t max_stacks{1};
    StackPolicy mode{StackPolicy::stack};
    RefreshPolicy refresh{RefreshPolicy::reset};
};

struct BuffSpec;

struct Effect {
    EffectKind kind{EffectKind::damage};
    TargetRule target{TargetRule::enemy_front};
    std::int32_t target_count{1};
    BasisPoints attack_bp{10000};
    std::int64_t flat{0};
    std::shared_ptr<const BuffSpec> buff;
    std::uint32_t remove_buff_id{0};
};

struct BuffReaction {
    Trigger trigger{Trigger::round_end};
    EffectSource source{EffectSource::owner};
    StackScaling stack_scaling{StackScaling::once};
    BasisPoints chance_bp{10000};
    std::int32_t max_triggers_per_round{0};
    std::vector<Effect> effects;
};

struct BuffSpec {
    std::uint32_t id{0};
    std::string name;
    LifetimePolicy lifetime;
    StackingPolicy stacking;
    std::vector<AttributeModifier> modifiers;
    std::vector<BuffReaction> reactions;
};

struct Skill {
    std::uint32_t id{0};
    std::string name;
    BasisPoints chance_bp{10000};
    std::int32_t priority{0};
    std::vector<Effect> effects;
};

struct Passive {
    std::uint32_t id{0};
    std::string name;
    Trigger trigger{Trigger::on_damaged};
    BasisPoints chance_bp{10000};
    std::int32_t max_triggers_per_round{0};
    std::vector<Effect> effects;
};

struct UnitConfig {
    UnitId id{0};
    UnitKind kind{UnitKind::hero};
    std::int32_t position{0};
    std::int32_t level{1};
    bool can_act{true};
    bool targetable{true};
    std::unordered_map<std::string, std::int32_t> growth_levels;
    Stats final_stats;
    std::vector<Skill> skills;
    std::vector<Passive> passives;
};

struct Formation {
    std::string name;
    std::int64_t initiative_bonus{0};
    std::vector<UnitConfig> units;
};

// Formation data describes a unit's full-health battle configuration. Initial
// conditions are sparse runtime overrides for a particular battle instance.
// Units omitted here start at final_stats.hp.
struct UnitInitialState {
    UnitId unit_id{0};
    std::int64_t current_hp{0};
};

struct BattleInitialConditions {
    // Optional audit link to the battle that produced these inherited values.
    std::uint64_t source_battle_id{0};
    // Normally initiative determines the first side. Scripted encounters may
    // explicitly override it without changing either formation.
    std::optional<Side> forced_first_side;
    std::vector<UnitInitialState> unit_states;
};

struct BattleRequest {
    std::uint64_t battle_id{0};
    std::uint64_t seed{1};
    std::int32_t max_rounds{50};
    std::int32_t max_events{10000};
    Formation attacker;
    Formation defender;
    BattleInitialConditions initial_conditions;
};

struct Event {
    std::uint32_t seq{0};
    std::int32_t round{0};
    std::string phase;
    std::string type;
    Side side{Side::attacker};
    UnitId actor{0};
    UnitId target{0};
    std::uint32_t source_id{0};
    std::int64_t value{0};
    std::int64_t hp_before{0};
    std::int64_t hp_after{0};
    bool critical{false};
};

struct UnitResult {
    UnitId id{0};
    Side side{Side::attacker};
    std::int64_t initial_hp{0};
    std::int64_t hp{0};
    std::int64_t max_hp{0};
    bool alive{false};
};

enum class Winner : std::uint8_t { attacker, defender, draw };

struct BattleResult {
    std::uint64_t battle_id{0};
    std::uint64_t seed{0};
    std::uint64_t source_battle_id{0};
    Winner winner{Winner::draw};
    std::string reason;
    std::int32_t rounds{0};
    std::uint64_t attacker_initiative{0};
    std::uint64_t defender_initiative{0};
    std::vector<Event> events;
    std::vector<UnitResult> units;
};

class Engine {
public:
    BattleResult simulate(const BattleRequest& request) const;
};

} // namespace gamebattle
