#include "battle_runtime.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

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

std::uint64_t current_buff_cutoff(const BattleState& state) {
    return state.next_buff_instance_id == 0
               ? 0
               : state.next_buff_instance_id - 1;
}

UnitId unit_id(const BattleState& state,
               std::optional<std::size_t> index) {
    if (!index.has_value() || *index >= state.units.size()) {
        return 0;
    }
    return state.units[*index].config.id;
}

CombatEventContext children_of(const CombatEventContext& emitted) {
    auto context = emitted;
    context.parent_event_id = emitted.event_id;
    if (context.depth != std::numeric_limits<std::uint32_t>::max()) {
        ++context.depth;
    }
    return context;
}

} // namespace

EffectSystem::EffectSystem(BattleState& state) : state_(state) {}

void EffectSystem::execute_action(std::size_t actor_index) {
    if (actor_index >= state_.units.size() || state_.execution_limit) {
        return;
    }
    const auto actor_id = state_.units[actor_index].config.id;
    push_root(ActionWork{actor_index}, Trigger::on_attack,
              actor_id, 0, actor_id, 0);
    drain_work_queue();
}

void EffectSystem::trigger_owner(
    std::size_t owner_index,
    Trigger trigger,
    std::optional<std::size_t> event_unit,
    std::uint32_t source_id,
    std::size_t depth) {
    if (owner_index >= state_.units.size() || state_.execution_limit) {
        return;
    }
    const auto owner_id = state_.units[owner_index].config.id;
    const auto event_id = unit_id(state_, event_unit);
    const bool allow_dead_owner =
        trigger == Trigger::on_damaged ||
        (trigger == Trigger::unit_death && event_unit == owner_index);
    work_queue_.push_back(WorkItem{
        .context = state_.make_event_context(
            trigger, owner_id, event_id, owner_id, source_id, 0,
            static_cast<std::uint32_t>(std::min<std::size_t>(
                depth, std::numeric_limits<std::uint32_t>::max()))),
        .payload = TriggerOwnerWork{
            .owner_index = owner_index,
            .trigger = trigger,
            .event_unit = event_unit,
            .source_id = source_id,
            .buff_instance_cutoff = current_buff_cutoff(state_),
            .allow_dead_owner = allow_dead_owner
        }
    });
    drain_work_queue();
}

void EffectSystem::trigger_all(
    Trigger trigger,
    std::optional<std::size_t> event_unit,
    std::uint32_t source_id,
    std::size_t depth) {
    if (state_.execution_limit) {
        return;
    }
    const auto subject_id = unit_id(state_, event_unit);
    work_queue_.push_back(WorkItem{
        .context = state_.make_event_context(
            trigger, 0, subject_id, subject_id, source_id, 0,
            static_cast<std::uint32_t>(std::min<std::size_t>(
                depth, std::numeric_limits<std::uint32_t>::max()))),
        .payload = TriggerAllWork{
            .trigger = trigger,
            .event_unit = event_unit,
            .source_id = source_id,
            .buff_instance_cutoff = current_buff_cutoff(state_)
        }
    });
    drain_work_queue();
}

void EffectSystem::drain_work_queue() {
    if (draining_ || state_.execution_limit) {
        return;
    }

    draining_ = true;
    try {
        while (!work_queue_.empty() && !state_.execution_limit) {
            WorkItem item = std::move(work_queue_.back());
            work_queue_.pop_back();
            if (!state_.consume_execution_step()) {
                work_queue_.clear();
                break;
            }
            std::visit(
                [&](auto& payload) { process(payload, item.context); },
                item.payload);
        }
    } catch (...) {
        work_queue_.clear();
        draining_ = false;
        throw;
    }
    draining_ = false;
}

void EffectSystem::push_root(
    WorkPayload payload,
    Trigger trigger,
    UnitId source,
    UnitId target,
    UnitId subject,
    std::uint32_t source_id) {
    if (state_.execution_limit) {
        return;
    }
    work_queue_.push_back(WorkItem{
        .context = state_.make_event_context(
            trigger, source, target, subject, source_id),
        .payload = std::move(payload)
    });
}

void EffectSystem::push_child(
    WorkPayload payload,
    const CombatEventContext& parent,
    Trigger trigger,
    UnitId source,
    UnitId target,
    UnitId subject,
    std::uint32_t source_id) {
    if (state_.execution_limit) {
        return;
    }
    work_queue_.push_back(WorkItem{
        .context = state_.make_event_context(
            trigger, source, target, subject, source_id,
            parent.parent_event_id, parent.depth),
        .payload = std::move(payload)
    });
}

void EffectSystem::schedule_effects(
    std::size_t source_index,
    std::size_t selection_owner_index,
    const std::vector<Effect>& effects,
    std::uint32_t source_id,
    std::optional<std::size_t> trigger_unit,
    std::int32_t magnitude_stacks,
    const CombatEventContext& parent) {
    if (source_index >= state_.units.size() ||
        selection_owner_index >= state_.units.size()) {
        return;
    }
    const auto source_unit_id = state_.units[source_index].config.id;
    const auto owner_unit_id = state_.units[selection_owner_index].config.id;
    const auto target_unit_id = unit_id(state_, trigger_unit);
    for (auto iterator = effects.rbegin(); iterator != effects.rend(); ++iterator) {
        push_child(
            EffectWork{
                .source_index = source_index,
                .selection_owner_index = selection_owner_index,
                .effect = *iterator,
                .source_id = source_id,
                .trigger_unit = trigger_unit,
                .magnitude_stacks = std::max<std::int32_t>(1, magnitude_stacks)
            },
            parent, parent.trigger, source_unit_id, target_unit_id,
            owner_unit_id, source_id);
    }
}

std::vector<EffectSystem::ReactionCandidate> EffectSystem::collect_reactions(
    std::size_t owner_index,
    Trigger trigger,
    std::optional<std::size_t> event_unit,
    std::uint32_t source_id,
    std::uint64_t buff_instance_cutoff,
    bool allow_dead_owner) const {
    std::vector<ReactionCandidate> candidates;
    if (owner_index >= state_.units.size()) {
        return candidates;
    }
    const auto& owner = state_.units[owner_index];
    if (!owner.alive() && !allow_dead_owner) {
        return candidates;
    }

    const auto trigger_index = static_cast<std::size_t>(trigger);
    for (const auto passive_index :
         owner.passives_by_trigger.at(trigger_index)) {
        const auto& passive = owner.config.passives.at(passive_index);
        candidates.push_back(ReactionCandidate{
            .work = ReactionWork{
                .kind = ReactionKind::passive,
                .owner_index = owner_index,
                .config_index = passive_index,
                .buff_instance_id = 0,
                .event_unit = event_unit,
                .source_id = source_id,
                .allow_dead_owner = allow_dead_owner
            },
            .priority = passive.priority,
            .definition_id = passive.id
        });
    }

    for (const auto& active : owner.buffs) {
        if (active.instance_id > buff_instance_cutoff ||
            active.definition == nullptr) {
            continue;
        }
        for (std::size_t reaction_index = 0;
             reaction_index < active.definition->reactions.size();
             ++reaction_index) {
            const auto& reaction =
                active.definition->reactions[reaction_index];
            if (reaction.trigger != trigger) {
                continue;
            }
            candidates.push_back(ReactionCandidate{
                .work = ReactionWork{
                    .kind = ReactionKind::buff,
                    .owner_index = owner_index,
                    .config_index = reaction_index,
                    .buff_instance_id = active.instance_id,
                    .event_unit = event_unit,
                    .source_id = source_id,
                    .allow_dead_owner = allow_dead_owner
                },
                .priority = reaction.priority,
                .definition_id = active.definition->id
            });
        }
    }
    return candidates;
}

void EffectSystem::schedule_reactions(
    std::vector<ReactionCandidate> candidates,
    const CombatEventContext& parent) {
    std::stable_sort(
        candidates.begin(), candidates.end(),
        [](const ReactionCandidate& left, const ReactionCandidate& right) {
            if (left.priority != right.priority) {
                return left.priority > right.priority;
            }
            if (left.work.owner_index != right.work.owner_index) {
                return left.work.owner_index < right.work.owner_index;
            }
            if (left.work.kind != right.work.kind) {
                return left.work.kind < right.work.kind;
            }
            if (left.definition_id != right.definition_id) {
                return left.definition_id < right.definition_id;
            }
            if (left.work.buff_instance_id != right.work.buff_instance_id) {
                return left.work.buff_instance_id < right.work.buff_instance_id;
            }
            return left.work.config_index < right.work.config_index;
        });

    for (auto iterator = candidates.rbegin();
         iterator != candidates.rend(); ++iterator) {
        const auto owner_id =
            state_.units[iterator->work.owner_index].config.id;
        const auto target_id = unit_id(state_, iterator->work.event_unit);
        push_child(iterator->work, parent, parent.trigger, owner_id,
                   target_id, owner_id, iterator->definition_id);
    }
}

void EffectSystem::process(
    ActionWork& work,
    const CombatEventContext& context) {
    if (work.actor_index >= state_.units.size()) {
        return;
    }
    auto& actor = state_.units[work.actor_index];
    if (!actor.alive()) {
        return;
    }

    const Skill* selected = nullptr;
    for (const auto& skill : actor.config.skills) {
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

    state_.emit(context, state_.phase, "skill", actor.side,
                actor.config.id, 0, selected->id, 0);
    const auto event_parent = children_of(context);

    // LIFO: put on_attack above the skill effects, preserving the old
    // depth-first semantics without using the C++ call stack.
    schedule_effects(work.actor_index, work.actor_index, selected->effects,
                     selected->id, std::nullopt, 1, event_parent);
    push_child(
        TriggerOwnerWork{
            .owner_index = work.actor_index,
            .trigger = Trigger::on_attack,
            .event_unit = work.actor_index,
            .source_id = selected->id,
            .buff_instance_cutoff = current_buff_cutoff(state_),
            .allow_dead_owner = false
        },
        event_parent, Trigger::on_attack, actor.config.id, actor.config.id,
        actor.config.id, selected->id);
}

void EffectSystem::process(
    EffectWork& work,
    const CombatEventContext& context) {
    if (work.source_index >= state_.units.size() ||
        work.selection_owner_index >= state_.units.size()) {
        return;
    }

    if (work.magnitude_stacks > 1 &&
        (work.effect.kind == EffectKind::damage ||
         work.effect.kind == EffectKind::heal ||
         work.effect.kind == EffectKind::direct_damage)) {
        work.effect.flat =
            saturating_multiply(work.effect.flat, work.magnitude_stacks);
        const auto scaled_attack = saturating_multiply(
            work.effect.attack_bp, work.magnitude_stacks);
        work.effect.attack_bp = static_cast<BasisPoints>(
            std::clamp<std::int64_t>(
                scaled_attack,
                std::numeric_limits<BasisPoints>::min(),
                std::numeric_limits<BasisPoints>::max()));
    }

    const auto targets = TargetSelector::select(
        state_, work.selection_owner_index, work.effect.target,
        work.effect.target_count, work.trigger_unit);
    for (auto iterator = targets.rbegin(); iterator != targets.rend(); ++iterator) {
        const auto source_unit_id =
            state_.units[work.source_index].config.id;
        const auto owner_unit_id =
            state_.units[work.selection_owner_index].config.id;
        const auto target_unit_id = state_.units[*iterator].config.id;
        push_child(
            ResolvedEffectWork{
                .source_index = work.source_index,
                .target_index = *iterator,
                .effect = work.effect,
                .source_id = work.source_id
            },
            context, context.trigger, source_unit_id, target_unit_id,
            owner_unit_id, work.source_id);
    }
}

void EffectSystem::process(
    ResolvedEffectWork& work,
    const CombatEventContext& context) {
    switch (work.effect.kind) {
    case EffectKind::damage:
    case EffectKind::direct_damage:
        apply_damage(work.source_index, work.target_index, work.effect,
                     work.source_id, context);
        break;
    case EffectKind::heal:
        apply_heal(work.source_index, work.target_index, work.effect,
                   work.source_id, context);
        break;
    case EffectKind::add_buff:
        apply_buff(work.source_index, work.target_index, work.effect.buff,
                   work.source_id, context);
        break;
    case EffectKind::remove_buff:
        remove_buff(work.source_index, work.target_index,
                    work.effect.remove_buff_id, work.source_id, context);
        break;
    }
}

void EffectSystem::process(
    TriggerOwnerWork& work,
    const CombatEventContext& context) {
    if (work.owner_index >= state_.units.size()) {
        return;
    }
    const auto& owner = state_.units[work.owner_index];
    if (!owner.alive() && !work.allow_dead_owner) {
        return;
    }

    const bool has_lifetime_work = std::any_of(
        owner.buffs.begin(), owner.buffs.end(),
        [&](const ActiveBuff& active) {
            return active.instance_id <= work.buff_instance_cutoff &&
                   active.definition != nullptr &&
                   !active.definition->lifetime.permanent &&
                   active.definition->lifetime.decrement_on == work.trigger;
        });
    if (has_lifetime_work) {
        push_child(
            LifetimeWork{
                .owner_index = work.owner_index,
                .trigger = work.trigger,
                .buff_instance_cutoff = work.buff_instance_cutoff,
                .allow_dead_owner = work.allow_dead_owner
            },
            context, work.trigger, owner.config.id,
            unit_id(state_, work.event_unit), owner.config.id,
            work.source_id);
    }

    schedule_reactions(
        collect_reactions(work.owner_index, work.trigger, work.event_unit,
                          work.source_id, work.buff_instance_cutoff,
                          work.allow_dead_owner),
        context);
}

void EffectSystem::process(
    TriggerAllWork& work,
    const CombatEventContext& context) {
    std::vector<ReactionCandidate> candidates;
    std::vector<std::pair<std::size_t, bool>> owners;
    owners.reserve(state_.units.size());

    for (std::size_t owner_index = 0;
         owner_index < state_.units.size(); ++owner_index) {
        const bool allow_dead_owner =
            work.trigger == Trigger::unit_death &&
            work.event_unit.has_value() &&
            *work.event_unit == owner_index;
        if (!state_.units[owner_index].alive() && !allow_dead_owner) {
            continue;
        }
        owners.emplace_back(owner_index, allow_dead_owner);
        auto owner_candidates = collect_reactions(
            owner_index, work.trigger, work.event_unit, work.source_id,
            work.buff_instance_cutoff, allow_dead_owner);
        candidates.insert(candidates.end(),
                          std::make_move_iterator(owner_candidates.begin()),
                          std::make_move_iterator(owner_candidates.end()));
    }

    // Lifetime runs after every reaction belonging to this trigger snapshot.
    for (auto iterator = owners.rbegin(); iterator != owners.rend(); ++iterator) {
        const auto owner_index = iterator->first;
        const auto& owner = state_.units[owner_index];
        const bool has_lifetime_work = std::any_of(
            owner.buffs.begin(), owner.buffs.end(),
            [&](const ActiveBuff& active) {
                return active.instance_id <= work.buff_instance_cutoff &&
                       active.definition != nullptr &&
                       !active.definition->lifetime.permanent &&
                       active.definition->lifetime.decrement_on == work.trigger;
            });
        if (!has_lifetime_work) {
            continue;
        }
        push_child(
            LifetimeWork{
                .owner_index = owner_index,
                .trigger = work.trigger,
                .buff_instance_cutoff = work.buff_instance_cutoff,
                .allow_dead_owner = iterator->second
            },
            context, work.trigger, owner.config.id,
            unit_id(state_, work.event_unit), owner.config.id,
            work.source_id);
    }

    schedule_reactions(std::move(candidates), context);
}

void EffectSystem::process(
    ReactionWork& work,
    const CombatEventContext& context) {
    if (work.owner_index >= state_.units.size()) {
        return;
    }
    auto& owner = state_.units[work.owner_index];
    if (!owner.alive() && !work.allow_dead_owner) {
        return;
    }
    const auto event_target_id = unit_id(state_, work.event_unit);

    if (work.kind == ReactionKind::passive) {
        if (work.config_index >= owner.config.passives.size()) {
            return;
        }
        const auto& passive = owner.config.passives[work.config_index];
        if (passive.trigger != context.trigger) {
            return;
        }
        auto& count = owner.passive_triggers[passive.id];
        if (passive.max_triggers_per_round > 0 &&
            count >= passive.max_triggers_per_round) {
            return;
        }
        // Exhausted reactions never consume RNG. This ordering is part of the
        // deterministic replay contract.
        if (!state_.random.roll(passive.chance_bp)) {
            return;
        }
        ++count;
        state_.emit(context, state_.phase, "passive", owner.side,
                    owner.config.id, event_target_id, passive.id, 0);
        const auto event_parent = children_of(context);
        schedule_effects(work.owner_index, work.owner_index, passive.effects,
                         passive.id, work.event_unit, 1, event_parent);
        return;
    }

    auto active = find_buff_instance(owner, work.buff_instance_id);
    if (active == owner.buffs.end() || active->definition == nullptr ||
        work.config_index >= active->definition->reactions.size()) {
        return;
    }
    auto definition = active->definition;
    const auto& reaction = definition->reactions[work.config_index];
    if (reaction.trigger != context.trigger) {
        return;
    }
    if (active->reaction_triggers.size() < definition->reactions.size()) {
        active->reaction_triggers.resize(definition->reactions.size(), 0);
    }
    auto& count = active->reaction_triggers[work.config_index];
    if (reaction.max_triggers_per_round > 0 &&
        count >= reaction.max_triggers_per_round) {
        return;
    }

    std::size_t effect_source_index = work.owner_index;
    if (reaction.source == EffectSource::applier) {
        const auto source = state_.find_unit(active->source);
        if (!source.has_value()) {
            return;
        }
        effect_source_index = *source;
    }
    if (!state_.random.roll(reaction.chance_bp)) {
        return;
    }
    ++count;

    const auto stacks =
        reaction.stack_scaling == StackScaling::per_stack
            ? active->stacks
            : 1;
    const auto& effect_source = state_.units[effect_source_index];
    state_.emit(
        context, state_.phase, "buff_reaction", effect_source.side,
        effect_source.config.id,
        event_target_id == 0 ? owner.config.id : event_target_id,
        definition->id, 0);
    const auto event_parent = children_of(context);
    schedule_effects(effect_source_index, work.owner_index,
                     reaction.effects, definition->id,
                     work.event_unit, stacks, event_parent);
}

void EffectSystem::process(
    LifetimeWork& work,
    const CombatEventContext& context) {
    if (work.owner_index >= state_.units.size()) {
        return;
    }
    const auto& owner = state_.units[work.owner_index];
    if (!owner.alive() && !work.allow_dead_owner) {
        return;
    }

    std::vector<std::uint64_t> instances;
    for (const auto& active : owner.buffs) {
        if (active.instance_id <= work.buff_instance_cutoff &&
            active.definition != nullptr &&
            !active.definition->lifetime.permanent &&
            active.definition->lifetime.decrement_on == work.trigger) {
            instances.push_back(active.instance_id);
        }
    }
    for (auto iterator = instances.rbegin(); iterator != instances.rend();
         ++iterator) {
        push_child(
            DecrementBuffWork{
                .owner_index = work.owner_index,
                .buff_instance_id = *iterator,
                .trigger = work.trigger
            },
            context, work.trigger, owner.config.id, owner.config.id,
            owner.config.id, 0);
    }
}

void EffectSystem::process(
    DecrementBuffWork& work,
    const CombatEventContext& context) {
    if (work.owner_index >= state_.units.size()) {
        return;
    }
    auto& owner = state_.units[work.owner_index];
    auto active = find_buff_instance(owner, work.buff_instance_id);
    if (active == owner.buffs.end() || active->definition == nullptr ||
        active->definition->lifetime.permanent ||
        active->definition->lifetime.decrement_on != work.trigger) {
        return;
    }

    --active->remaining;
    if (active->remaining > 0) {
        return;
    }
    const auto buff_id = active->definition->id;
    state_.emit(context, state_.phase, "buff_expire", owner.side,
                owner.config.id, owner.config.id, buff_id, 0);
    owner.buffs.erase(active);
    state_.mark_stats_dirty(work.owner_index);
}

void EffectSystem::process(
    DeathCheckWork& work,
    const CombatEventContext& context) {
    if (work.actor_index >= state_.units.size() ||
        work.target_index >= state_.units.size()) {
        return;
    }
    auto& target = state_.units[work.target_index];
    if (target.alive() || target.death_notified) {
        return;
    }
    target.death_notified = true;
    const auto& actor = state_.units[work.actor_index];
    state_.emit(context, state_.phase, "death", target.side,
                actor.config.id, target.config.id, work.source_id,
                0, 0, 0);
    const auto event_parent = children_of(context);
    push_child(
        TriggerAllWork{
            .trigger = Trigger::unit_death,
            .event_unit = work.target_index,
            .source_id = work.source_id,
            .buff_instance_cutoff = current_buff_cutoff(state_)
        },
        event_parent, Trigger::unit_death, actor.config.id, target.config.id,
        target.config.id, work.source_id);
}

void EffectSystem::apply_damage(
    std::size_t actor_index,
    std::size_t target_index,
    const Effect& effect,
    std::uint32_t source_id,
    const CombatEventContext& context) {
    if (actor_index >= state_.units.size() ||
        target_index >= state_.units.size()) {
        return;
    }
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
            state_.emit(context, state_.phase, "miss", actor.side,
                        actor.config.id, target.config.id, source_id,
                        0, target.hp, target.hp);
            return;
        }

        damage = std::max<std::int64_t>(
            1, saturating_add(damage, -target_stats.defense));
        damage = std::max<std::int64_t>(
            1, scale(damage, kBasisPoints + actor_stats.damage_bonus_bp));
        damage = std::max<std::int64_t>(
            1, scale(damage,
                     std::max<std::int64_t>(
                         0, kBasisPoints -
                                target_stats.damage_reduction_bp)));
        critical = state_.random.roll(actor_stats.crit_rate_bp);
        if (critical) {
            damage = std::max<std::int64_t>(
                1, scale(damage,
                         std::max<BasisPoints>(
                             kBasisPoints, actor_stats.crit_damage_bp)));
        }
    } else {
        damage = std::max<std::int64_t>(1, damage);
    }

    damage = std::min(damage, target.hp);
    const auto before = target.hp;
    target.hp -= damage;
    state_.emit(context, state_.phase,
                direct ? "direct_damage" : "damage", actor.side,
                actor.config.id, target.config.id, source_id, damage,
                before, target.hp, critical);
    const auto event_parent = children_of(context);

    // One snapshot for this damage event: a Buff installed by on_hit cannot
    // retroactively observe the same on_damaged event.
    const auto cutoff = current_buff_cutoff(state_);
    if (!target.alive()) {
        push_child(
            DeathCheckWork{
                .actor_index = actor_index,
                .target_index = target_index,
                .source_id = source_id
            },
            event_parent, Trigger::unit_death, actor.config.id,
            target.config.id, target.config.id, source_id);
    }
    push_child(
        TriggerOwnerWork{
            .owner_index = target_index,
            .trigger = Trigger::on_damaged,
            .event_unit = actor_index,
            .source_id = source_id,
            .buff_instance_cutoff = cutoff,
            .allow_dead_owner = true
        },
        event_parent, Trigger::on_damaged, target.config.id, actor.config.id,
        target.config.id, source_id);
    if (!direct) {
        push_child(
            TriggerOwnerWork{
                .owner_index = actor_index,
                .trigger = Trigger::on_hit,
                .event_unit = target_index,
                .source_id = source_id,
                .buff_instance_cutoff = cutoff,
                .allow_dead_owner = false
            },
            event_parent, Trigger::on_hit, actor.config.id, target.config.id,
            actor.config.id, source_id);
    }
}

void EffectSystem::apply_heal(
    std::size_t actor_index,
    std::size_t target_index,
    const Effect& effect,
    std::uint32_t source_id,
    const CombatEventContext& context) {
    if (actor_index >= state_.units.size() ||
        target_index >= state_.units.size()) {
        return;
    }
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
    state_.emit(context, state_.phase, "heal", actor.side,
                actor.config.id, target.config.id, source_id,
                target.hp - before, before, target.hp);
}

void EffectSystem::apply_buff(
    std::size_t actor_index,
    std::size_t target_index,
    std::shared_ptr<const BuffSpec> definition,
    std::uint32_t source_id,
    const CombatEventContext& context) {
    if (actor_index >= state_.units.size() ||
        target_index >= state_.units.size() || definition == nullptr ||
        definition->id == 0 || definition->stacking.max_stacks <= 0 ||
        (!definition->lifetime.permanent &&
         definition->lifetime.duration <= 0)) {
        return;
    }

    const auto applier_id = state_.units[actor_index].config.id;
    auto& target = state_.units[target_index];
    auto iterator = std::find_if(
        target.buffs.begin(), target.buffs.end(),
        [&](const ActiveBuff& active) {
            if (active.definition == nullptr ||
                active.definition->id != definition->id) {
                return false;
            }
            return definition->stacking.key == StackKeyPolicy::by_buff ||
                   active.source == applier_id;
        });
    if (iterator == target.buffs.end()) {
        if (state_.next_buff_instance_id ==
            std::numeric_limits<std::uint64_t>::max()) {
            state_.execution_limit = true;
            return;
        }
        ActiveBuff active;
        active.definition = std::move(definition);
        active.instance_id = state_.next_buff_instance_id++;
        active.remaining = active.definition->lifetime.permanent
                               ? 0
                               : active.definition->lifetime.duration;
        active.stacks = 1;
        active.source = applier_id;
        active.reaction_triggers.assign(
            active.definition->reactions.size(), 0);
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
                        extended,
                        std::numeric_limits<std::int32_t>::max()));
                break;
            }
            case RefreshPolicy::keep:
                break;
            }
        }
        // by_buff intentionally uses last-applier attribution. Select
        // by_buff_and_source when each applier must own an independent stack.
        iterator->source = applier_id;
    }

    state_.mark_stats_dirty(target_index);
    state_.emit(
        context, state_.phase, "buff_add",
        state_.units[actor_index].side, applier_id, target.config.id,
        source_id == 0 ? iterator->definition->id : source_id,
        iterator->stacks);
}

void EffectSystem::remove_buff(
    std::size_t actor_index,
    std::size_t target_index,
    std::uint32_t buff_id,
    std::uint32_t source_id,
    const CombatEventContext& context) {
    if (actor_index >= state_.units.size() ||
        target_index >= state_.units.size()) {
        return;
    }
    auto& target = state_.units[target_index];
    const auto old_size = target.buffs.size();
    target.buffs.erase(
        std::remove_if(
            target.buffs.begin(), target.buffs.end(),
            [&](const ActiveBuff& active) {
                return active.definition != nullptr &&
                       active.definition->id == buff_id;
            }),
        target.buffs.end());
    if (target.buffs.size() == old_size) {
        return;
    }
    state_.mark_stats_dirty(target_index);
    state_.emit(
        context, state_.phase, "buff_remove",
        state_.units[actor_index].side,
        state_.units[actor_index].config.id, target.config.id, source_id,
        static_cast<std::int64_t>(old_size - target.buffs.size()));
}

} // namespace gamebattle::runtime
