#include "battle_runtime.hpp"

#include <algorithm>
#include <limits>

namespace gamebattle::runtime {
namespace {

auto find_buff_instance(RuntimeUnit& unit, std::uint64_t instance_id) {
    return std::find_if(
        unit.buffs.begin(), unit.buffs.end(),
        [instance_id](const ActiveBuff& active) {
            return active.instance_id == instance_id;
        });
}

std::int64_t saturating_multiply(std::int64_t left, std::int64_t right) {
    if (left == 0 || right == 0) {
        return 0;
    }
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    if (left > 0) {
        if (right > 0 && left > maximum / right) return maximum;
        if (right < 0 && right < minimum / left) return minimum;
    } else {
        if (right > 0 && left < minimum / right) return minimum;
        if (right < 0 && left < maximum / right) return maximum;
    }
    return left * right;
}

} // namespace

EffectSystem::EffectSystem(BattleState& state) : state_(state) {
    for (const auto& unit : state_.units) {
        for (const auto trigger : {Trigger::enemy_activate, Trigger::ally_activate}) {
            if (!unit.passives_by_trigger.at(static_cast<std::size_t>(trigger)).empty()) {
                responses_possible_ = true;
            }
        }
    }
}

void EffectSystem::execute_action(std::size_t actor_index) {
    const Skill* selected = nullptr;
    for (const auto& skill : state_.units[actor_index].config.skills) {
        if (state_.random.roll(skill.chance_bp)) {
            selected = &skill;
            break;
        }
    }

    Skill basic;
    if (selected == nullptr) {
        basic.id = 0;
        basic.name = "basic_attack";
        basic.effects.push_back(Effect{});
        selected = &basic;
    }

    state_.emit(state_.phase, "skill", state_.units[actor_index].side,
                state_.units[actor_index].config.id, 0, selected->id, 0);
    trigger_owner(actor_index, Trigger::on_attack, actor_index, selected->id);
    // Only active skills open a chain; the basic attack resolves immediately.
    if (selected == &basic || !responses_possible_) {
        execute_effects(actor_index, actor_index, selected->effects, selected->id,
                        0, std::nullopt);
        return;
    }
    run_chain(actor_index, *selected);
}

// Builds a chain on top of an activated skill, then resolves it last-in,
// first-out. Triggers caused while a link resolves (on_hit, on_damaged, ...)
// still resolve immediately inside that link; only response passives join
// the chain.
void EffectSystem::run_chain(std::size_t actor_index, const Skill& skill) {
    chain_.clear();
    chain_.push_back(ChainLink{.source_index = actor_index,
                               .source_id = skill.id,
                               .effects = &skill.effects,
                               .answered_unit = std::nullopt});
    while (!state_.event_limit && add_response()) {
    }

    for (std::size_t link = chain_.size(); link-- > 0;) {
        if (state_.event_limit) {
            break;
        }
        const ChainLink current = chain_[link];
        if (current.negated) {
            continue;
        }
        const auto& source = state_.units[current.source_index];
        if (chain_.size() > 1 && !source.alive()) {
            // A response killed this link's owner before it could resolve.
            state_.emit(state_.phase, "fizzle", source.side, source.config.id, 0,
                        current.source_id, static_cast<std::int64_t>(link + 1));
            continue;
        }
        resolving_link_ = link;
        execute_effects(current.source_index, current.source_index,
                        *current.effects, current.source_id, 0,
                        current.answered_unit);
    }
    resolving_link_.reset();
    chain_.clear();
}

// Offers the top link to responders: first the side that did not add it, then
// that side's allies. Each unit adds at most one link per chain, which also
// bounds the chain length. Returns false once nobody responds.
bool EffectSystem::add_response() {
    const auto answered = chain_.back().source_index;
    const auto answered_side = state_.units[answered].side;
    for (const auto side : {other(answered_side), answered_side}) {
        const auto trigger = side == answered_side ? Trigger::ally_activate
                                                   : Trigger::enemy_activate;
        for (const auto unit_index : state_.response_order(side)) {
            const bool already_linked = std::any_of(
                chain_.begin(), chain_.end(), [&](const ChainLink& link) {
                    return link.source_index == unit_index;
                });
            if (already_linked) {
                continue;
            }
            auto& unit = state_.units[unit_index];
            for (const auto passive_index :
                 unit.passives_by_trigger.at(static_cast<std::size_t>(trigger))) {
                const auto& passive = unit.config.passives[passive_index];
                if (!state_.random.roll(passive.chance_bp)) {
                    continue;
                }
                auto& count = unit.passive_triggers[passive.id];
                if (passive.max_triggers_per_round > 0 &&
                    count >= passive.max_triggers_per_round) {
                    continue;
                }
                ++count;
                chain_.push_back(ChainLink{.source_index = unit_index,
                                           .source_id = passive.id,
                                           .effects = &passive.effects,
                                           .answered_unit = answered});
                state_.emit(state_.phase, "chain", unit.side, unit.config.id,
                            state_.units[answered].config.id, passive.id,
                            static_cast<std::int64_t>(chain_.size()));
                return true;
            }
        }
    }
    return false;
}

void EffectSystem::negate_answered_link(std::size_t source_index) {
    if (!resolving_link_.has_value() || *resolving_link_ == 0) {
        return;
    }
    auto& answered = chain_[*resolving_link_ - 1];
    if (answered.negated) {
        return;
    }
    answered.negated = true;
    const auto& source = state_.units[source_index];
    state_.emit(state_.phase, "negate", source.side, source.config.id,
                state_.units[answered.source_index].config.id, answered.source_id,
                static_cast<std::int64_t>(*resolving_link_));
}

void EffectSystem::execute_effects(
    std::size_t source_index,
    std::size_t selection_owner_index,
    const std::vector<Effect>& effects,
    std::uint32_t source_id,
    std::size_t depth,
    std::optional<std::size_t> trigger_unit,
    std::int32_t magnitude_stacks) {
    if (depth > kMaxTriggerDepth || state_.event_limit) {
        return;
    }
    for (const auto& effect : effects) {
        if (effect.kind == EffectKind::negate) {
            negate_answered_link(source_index);
            continue;
        }
        Effect scaled_effect;
        const Effect* executable = &effect;
        if (magnitude_stacks > 1 &&
            (effect.kind == EffectKind::damage ||
             effect.kind == EffectKind::heal ||
             effect.kind == EffectKind::direct_damage)) {
            scaled_effect = effect;
            scaled_effect.flat =
                saturating_multiply(effect.flat, magnitude_stacks);
            const auto scaled_attack = saturating_multiply(
                effect.attack_bp, magnitude_stacks);
            scaled_effect.attack_bp = static_cast<BasisPoints>(
                std::clamp<std::int64_t>(
                    scaled_attack,
                    std::numeric_limits<BasisPoints>::min(),
                    std::numeric_limits<BasisPoints>::max()));
            executable = &scaled_effect;
        }
        const auto targets = TargetSelector::select(
            state_, selection_owner_index, executable->target,
            executable->target_count, trigger_unit);
        for (const auto target_index : targets) {
            if (state_.event_limit) {
                return;
            }
            switch (executable->kind) {
            case EffectKind::damage:
            case EffectKind::direct_damage:
                apply_damage(source_index, target_index, *executable, source_id,
                             depth + 1);
                break;
            case EffectKind::heal:
                apply_heal(source_index, target_index, *executable, source_id);
                break;
            case EffectKind::add_buff:
                apply_buff(source_index, target_index, executable->buff,
                           source_id);
                break;
            case EffectKind::remove_buff:
                remove_buff(source_index, target_index,
                            executable->remove_buff_id, source_id);
                break;
            case EffectKind::negate:
                break;
            }
        }
    }
}

void EffectSystem::apply_damage(
    std::size_t actor_index,
    std::size_t target_index,
    const Effect& effect,
    std::uint32_t source_id,
    std::size_t depth) {
    auto& actor = state_.units[actor_index];
    auto& target = state_.units[target_index];
    const bool direct = effect.kind == EffectKind::direct_damage;
    if ((!direct && !actor.alive()) || !target.alive()) {
        return;
    }

    const auto actor_stats = state_.effective_stats(actor_index);
    auto damage = saturating_add(
        scale(actor_stats.attack, effect.attack_bp), effect.flat);
    bool critical = false;

    if (!direct) {
        const auto target_stats = state_.effective_stats(target_index);
        const auto hit_chance = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(actor_stats.hit_rate_bp) -
                target_stats.dodge_rate_bp,
            0, kBasisPoints);
        if (!state_.random.roll(static_cast<BasisPoints>(hit_chance))) {
            state_.emit(state_.phase, "miss", actor.side, actor.config.id,
                        target.config.id, source_id, 0, target.hp, target.hp);
            return;
        }

        damage = std::max<std::int64_t>(
            1, saturating_add(damage, -target_stats.defense));
        damage = std::max<std::int64_t>(
            1, scale(damage, kBasisPoints + actor_stats.damage_bonus_bp));
        damage = std::max<std::int64_t>(
            1, scale(damage,
                     std::max<std::int64_t>(
                         0, kBasisPoints - target_stats.damage_reduction_bp)));
        critical = state_.random.roll(actor_stats.crit_rate_bp);
        if (critical) {
            damage = std::max<std::int64_t>(
                1, scale(damage,
                         std::max<BasisPoints>(kBasisPoints,
                                               actor_stats.crit_damage_bp)));
        }
    } else {
        // This is the general form of the old periodic-damage path. It uses
        // source scaling but bypasses hit, defense, bonuses/reductions and crit.
        damage = std::max<std::int64_t>(1, damage);
    }

    damage = std::min(damage, target.hp);
    const auto before = target.hp;
    target.hp -= damage;
    state_.emit(state_.phase, direct ? "direct_damage" : "damage", actor.side,
                actor.config.id, target.config.id, source_id, damage, before,
                target.hp, critical);

    if (!direct) {
        trigger_owner(actor_index, Trigger::on_hit, target_index, source_id,
                      depth);
    }
    trigger_owner(target_index, Trigger::on_damaged, actor_index, source_id,
                  depth);
    if (!target.alive()) {
        state_.emit(state_.phase, "death", target.side, actor.config.id,
                    target.config.id, source_id, 0, 0, 0);
        trigger_all(Trigger::unit_death, target_index, source_id, depth);
    }
}

void EffectSystem::apply_heal(
    std::size_t actor_index,
    std::size_t target_index,
    const Effect& effect,
    std::uint32_t source_id) {
    auto& actor = state_.units[actor_index];
    auto& target = state_.units[target_index];
    if (!actor.alive() || !target.alive()) {
        return;
    }
    const auto amount = std::max<std::int64_t>(
        0, saturating_add(
               scale(state_.effective_stats(actor_index).attack,
                     effect.attack_bp),
               effect.flat));
    const auto before = target.hp;
    target.hp = std::min(target.config.final_stats.hp,
                         saturating_add(target.hp, amount));
    state_.emit(state_.phase, "heal", actor.side, actor.config.id,
                target.config.id, source_id, target.hp - before, before,
                target.hp);
}

void EffectSystem::apply_buff(
    std::size_t actor_index,
    std::size_t target_index,
    std::shared_ptr<const BuffSpec> definition,
    std::uint32_t source_id) {
    if (definition == nullptr || definition->id == 0 ||
        definition->stacking.max_stacks <= 0 ||
        (!definition->lifetime.permanent &&
         definition->lifetime.duration <= 0)) {
        return;
    }

    auto& target = state_.units[target_index];
    auto iterator = std::find_if(
        target.buffs.begin(), target.buffs.end(),
        [&](const ActiveBuff& active) {
            return active.definition != nullptr &&
                   active.definition->id == definition->id;
        });
    if (iterator == target.buffs.end()) {
        if (state_.next_buff_instance_id ==
            std::numeric_limits<std::uint64_t>::max()) {
            state_.event_limit = true;
            return;
        }
        ActiveBuff active;
        active.definition = std::move(definition);
        active.instance_id = state_.next_buff_instance_id++;
        active.remaining = active.definition->lifetime.permanent
                               ? 0
                               : active.definition->lifetime.duration;
        active.stacks = 1;
        active.source = state_.units[actor_index].config.id;
        active.reaction_triggers.assign(active.definition->reactions.size(), 0);
        target.buffs.push_back(std::move(active));
        iterator = std::prev(target.buffs.end());
    } else {
        const auto& spec = *iterator->definition;
        if (spec.stacking.mode == StackPolicy::stack) {
            iterator->stacks =
                std::min(spec.stacking.max_stacks, iterator->stacks + 1);
        }
        if (!spec.lifetime.permanent) {
            switch (spec.stacking.refresh) {
            case RefreshPolicy::reset:
                iterator->remaining = spec.lifetime.duration;
                break;
            case RefreshPolicy::extend: {
                const auto extended = saturating_add(
                    iterator->remaining, spec.lifetime.duration);
                iterator->remaining = static_cast<std::int32_t>(
                    std::min<std::int64_t>(
                        extended, std::numeric_limits<std::int32_t>::max()));
                break;
            }
            case RefreshPolicy::keep:
                break;
            }
        }
        iterator->source = state_.units[actor_index].config.id;
    }

    state_.mark_stats_dirty(target_index);
    state_.emit(state_.phase, "buff_add", state_.units[actor_index].side,
                state_.units[actor_index].config.id, target.config.id,
                source_id == 0 ? iterator->definition->id : source_id,
                iterator->stacks);
}

void EffectSystem::remove_buff(
    std::size_t actor_index,
    std::size_t target_index,
    std::uint32_t buff_id,
    std::uint32_t source_id) {
    auto& target = state_.units[target_index];
    const auto old_size = target.buffs.size();
    target.buffs.erase(
        std::remove_if(target.buffs.begin(), target.buffs.end(),
                       [&](const ActiveBuff& active) {
                           return active.definition != nullptr &&
                                  active.definition->id == buff_id;
                       }),
        target.buffs.end());
    if (target.buffs.size() != old_size) {
        state_.mark_stats_dirty(target_index);
        state_.emit(
            state_.phase, "buff_remove", state_.units[actor_index].side,
            state_.units[actor_index].config.id, target.config.id, source_id,
            static_cast<std::int64_t>(old_size - target.buffs.size()));
    }
}

void EffectSystem::trigger_owner(
    std::size_t owner_index,
    Trigger trigger,
    std::optional<std::size_t> event_unit,
    std::uint32_t source_id,
    std::size_t depth) {
    const auto cutoff = state_.next_buff_instance_id - 1;
    trigger_owner_snapshot(owner_index, trigger, event_unit, source_id, depth,
                           cutoff);
}

void EffectSystem::trigger_all(
    Trigger trigger,
    std::optional<std::size_t> event_unit,
    std::uint32_t source_id,
    std::size_t depth) {
    const auto cutoff = state_.next_buff_instance_id - 1;
    const auto count = state_.units.size();
    for (std::size_t index = 0; index < count; ++index) {
        trigger_owner_snapshot(index, trigger, event_unit, source_id, depth,
                               cutoff);
    }
}

void EffectSystem::trigger_owner_snapshot(
    std::size_t owner_index,
    Trigger trigger,
    std::optional<std::size_t> event_unit,
    std::uint32_t source_id,
    std::size_t depth,
    std::uint64_t buff_instance_cutoff) {
    if (depth > kMaxTriggerDepth || state_.event_limit) {
        return;
    }
    auto& owner = state_.units[owner_index];
    if (!owner.alive()) {
        return;
    }

    const auto trigger_index = static_cast<std::size_t>(trigger);
    for (const auto passive_index : owner.passives_by_trigger.at(trigger_index)) {
        const auto& passive = owner.config.passives[passive_index];
        if (!state_.random.roll(passive.chance_bp)) {
            continue;
        }
        auto& count = owner.passive_triggers[passive.id];
        if (passive.max_triggers_per_round > 0 &&
            count >= passive.max_triggers_per_round) {
            continue;
        }
        ++count;
        state_.emit(
            state_.phase, "passive", owner.side, owner.config.id,
            event_unit.has_value() && *event_unit < state_.units.size()
                ? state_.units[*event_unit].config.id
                : 0,
            passive.id == 0 ? source_id : passive.id, 0);
        execute_effects(owner_index, owner_index, passive.effects, passive.id,
                        depth + 1, event_unit);
    }

    struct PendingReaction {
        std::uint64_t instance_id;
        std::size_t reaction_index;
    };
    std::vector<PendingReaction> pending;
    for (const auto& active : owner.buffs) {
        if (active.instance_id > buff_instance_cutoff ||
            active.definition == nullptr) {
            continue;
        }
        for (std::size_t index = 0;
             index < active.definition->reactions.size(); ++index) {
            if (active.definition->reactions[index].trigger == trigger) {
                pending.push_back({active.instance_id, index});
            }
        }
    }

    for (const auto& item : pending) {
        if (state_.event_limit || !owner.alive()) {
            break;
        }
        auto active = find_buff_instance(owner, item.instance_id);
        if (active == owner.buffs.end() || active->definition == nullptr ||
            item.reaction_index >= active->definition->reactions.size()) {
            continue;
        }
        auto definition = active->definition;
        const auto& reaction = definition->reactions[item.reaction_index];
        if (!state_.random.roll(reaction.chance_bp)) {
            continue;
        }
        if (active->reaction_triggers.size() < definition->reactions.size()) {
            active->reaction_triggers.resize(definition->reactions.size(), 0);
        }
        auto& count = active->reaction_triggers[item.reaction_index];
        if (reaction.max_triggers_per_round > 0 &&
            count >= reaction.max_triggers_per_round) {
            continue;
        }
        ++count;

        std::size_t effect_source_index = owner_index;
        if (reaction.source == EffectSource::applier) {
            effect_source_index =
                state_.find_unit(active->source).value_or(owner_index);
        }
        const auto& effect_source = state_.units[effect_source_index];
        state_.emit(
            state_.phase, "buff_reaction", effect_source.side,
            effect_source.config.id,
            event_unit.has_value() && *event_unit < state_.units.size()
                ? state_.units[*event_unit].config.id
                : owner.config.id,
            definition->id, 0);
        execute_effects(effect_source_index, owner_index, reaction.effects,
                        definition->id, depth + 1, event_unit,
                        reaction.stack_scaling == StackScaling::per_stack
                            ? active->stacks
                            : 1);
    }

    bool expired = false;
    auto active = owner.buffs.begin();
    while (active != owner.buffs.end()) {
        const bool should_decrement =
            active->instance_id <= buff_instance_cutoff &&
            active->definition != nullptr &&
            !active->definition->lifetime.permanent &&
            active->definition->lifetime.decrement_on == trigger;
        if (should_decrement) {
            --active->remaining;
        }
        if (should_decrement && active->remaining <= 0) {
            state_.emit(state_.phase, "buff_expire", owner.side,
                        owner.config.id, owner.config.id,
                        active->definition->id, 0);
            active = owner.buffs.erase(active);
            expired = true;
        } else {
            ++active;
        }
    }
    if (expired) {
        state_.mark_stats_dirty(owner_index);
    }
}

} // namespace gamebattle::runtime
