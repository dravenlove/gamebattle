#pragma once

#include "gamebattle/engine.hpp"

#include <cstdint>
#include <string>

namespace gamebattle::report {

// How much of a battle the BattleReport carries (ReportDetail in
// proto/battle_client.proto). Every level has the result and the units' final
// states.
enum class Detail : std::uint8_t {
    summary,  // nothing more
    actions,  // plus one aggregated BattleAction per step
    events    // plus every BattleEvent
};

// Encodes result as a protobuf BattleReport (proto/battle_client.proto), ready
// to send to a client. The bytes are canonical: fields in number order and
// zero values left out, as every protobuf library writes them.
//
// With Detail::actions the events are added up step by step:
//   - an ACTION_START begins a step for that unit, which ends with its
//     ACTION_END (or with the last event);
//   - events outside actions form trigger steps, one per run of events with
//     the same round and phase;
//   - DAMAGE, DIRECT_DAMAGE, MISS, HEAL and DEATH go to the target's
//     UnitChange (sums saturate at the int64 limits, hp is the last
//     hp_after); SKILL inside an action sets the step's skill_id;
//     ACTION_START and ACTION_END are only counted;
//   - every other event goes to an EffectCount keyed by type, unit and
//     source_id, where the unit is the target for BUFF_ADD, BUFF_REMOVE and
//     BUFF_EXPIRE and the actor otherwise.
// UnitChanges and EffectCounts keep the order in which they first appeared.
// gamebattle_client:report/2 in Erlang implements the same rules and must
// produce the same bytes.
std::string encode(const BattleResult& result, Detail detail);

} // namespace gamebattle::report
