#pragma once

#include "gamebattle/config_store.hpp"
#include "gamebattle/engine.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gamebattle::config_check {

// Finds skill and passive designs that set each other off without end: a
// passive whose damage triggers itself again (a counterattack answering a
// counterattack), or several passives and buff reactions that trigger one
// another in a circle. The engine stops such a cascade only at the trigger
// depth limit or max_events, and the battle then ends as event_limit.
//
// Two checks run:
//   - Static: a graph of which passive or buff reaction can set off which.
//     Damage sets off on_hit for the attacker, on_damaged for the target and
//     unit_death on a kill; no other effect sets off a trigger, and every
//     other trigger comes from the battle loop, so only these three can
//     close a loop. A loop in which no passive or reaction has a
//     max_triggers_per_round is an error; a loop limited that way is a note.
//     unit_death counts as limited, since a unit dies once.
//   - Stress: battles in which every unit carries every passive and skill
//     and nobody dies, to measure the largest cascade a single step sets off.

// What a config pack holds, as plain values, so that definitions that never
// went through the compiler can be checked too.
struct Definitions {
    std::vector<Skill> skills;
    std::vector<Passive> passives;
    // Buffs whose reactions can fire. Buffs added by effects of the skills,
    // passives and reactions above are found on their own.
    std::vector<std::shared_ptr<const BuffSpec>> buffs;
};

// The store's definitions, in id order. The buffs point into the store, so
// keep it alive while the result is in use.
Definitions definitions(const ConfigStore& store);

struct Options {
    bool stress{true};
    std::int32_t units_per_side{7};
    std::int32_t rounds{5};
    std::int32_t battles{3};
    std::int32_t max_events{200000};
    // A single step (one action, or a run of triggers outside actions) with
    // more events than this is reported as a warning.
    std::uint32_t step_warning{5000};
};

enum class Severity : std::uint8_t { error, warning, note };

struct Finding {
    Severity severity{Severity::note};
    std::string message;
};

struct Report {
    std::vector<Finding> findings;
    bool has_errors() const;
};

Report check(const Definitions& definitions, const Options& options = {});

// "error: ...", "warning: ..." or "note: ...", one finding after another.
std::string format(const Report& report);

} // namespace gamebattle::config_check
