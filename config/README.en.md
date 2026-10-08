# Battle config tables

[中文](README.md) | **English**

Designers maintain the following six UTF-8 CSV files:

- `buffs.csv`: buff identity, lifetime and stacking policy.
- `buff_modifiers.csv`: generic modifiers a buff applies to any battle attribute.
- `buff_reactions.csv`: the effect sequences a buff runs when battle events happen.
- `effects.csv`: effect definitions such as damage, healing, adding a buff, removing a buff.
- `skills.csv`: active skills and their effect sequences.
- `passives.csv`: passive trigger conditions and their effect sequences.

They can be edited directly in Excel; choose `CSV UTF-8` when saving. `notes` is for designers only and isn't written into the runtime config package.

References always point in these directions:

```text
skills/passives -------------> effects -> buffs
buffs -> buff_reactions -----> effects -> buffs
buffs -> buff_modifiers
```

- `effect_ids` are separated by `|` and keep their execution order, e.g. `9001|9002`.
- All probabilities and percentages use basis points: `10000` means 100%, `3500` means 35%.
- `lifetime` supports `finite` and `permanent`; a permanent buff's `duration` must be `0`.
- `decrement_on` uses the same trigger points as passives; permanent buffs must still fill in this column, but the runtime ignores it.
- `stack_policy` supports `stack` and `refresh`; with `refresh`, `max_stacks` must be `1`.
- `refresh_policy` supports `reset`, `extend` and `keep`.
- A modifier's `attribute` supports `attack`, `defense`, `speed`, `crit_rate_bp`, `crit_damage_bp`, `hit_rate_bp`, `dodge_rate_bp`, `damage_bonus_bp` and `damage_reduction_bp`.
- A modifier's `operation` supports `add` and `scale_bp`; modifiers are applied according to the buff's current stack count.
- `buff_modifiers.csv` and `buff_reactions.csv` use `(buff_id, sequence)` as a composite unique key and execute in `sequence` order.
- A reaction's `source` supports `owner` and `applier`. Target selection always uses the buff holder as its context; `source` only decides the effect's stats and the event's source.
- A reaction's `stack_scaling` supports `once` and `per_stack`; the latter scales damage, healing or direct damage by the buff's current stack count, while adding/removing buffs is never repeated.
- Reactions that reference `add_buff` effects must not form a directed cycle, or the shared config definitions would form an ownership cycle; both the compiler and the loader reject it.
- `direct_damage` is direct damage that bypasses the hit, crit, defense, damage bonus and damage reduction formulas; damage over time can be built by having a reaction reference it.
- `add_buff` effects must fill in `buff_id`.
- `remove_buff` effects must fill in `remove_buff_id`.
- `enemy_activate` and `ally_activate` are response triggers for passives only (see "Chains and responses" in the root README); they can't be used in `trigger` in `buff_reactions.csv` or in `decrement_on` in `buffs.csv`.
- A `negate` effect cancels the chain link its response answered, and can only be referenced by passives whose `trigger` is `enemy_activate` or `ally_activate`; neither skills nor buff reactions may reference it. The `target` column has no effect on it; `trigger_unit` is fine.
- For effects of other types, `buff_id`/`remove_buff_id` must be `0` or empty.
- Blank numeric values use the compiler's defaults, but the columns themselves can't be deleted or renamed.
- IDs must be unique within their own table; every cross-table reference is checked during generation.

## Runaway loop check

Every run of the compiler, `--check-only` included, also looks for passives and buff reactions that set each other off without end, and refuses to write a pack that has one.

Damage sets off `on_hit` for the attacker, `on_damaged` for the target and `unit_death` on a kill. A passive on `on_damaged` that deals damage therefore sets off `on_damaged` on its own target; if that unit has the same passive, it answers, and so on. With no `max_triggers_per_round`, nothing stops this until the engine cuts the cascade at its trigger depth limit or ends the battle at `max_events` (end reason `event_limit`). With area damage every link sets off several more, and a single action can turn into hundreds of thousands of events.

The check has two parts:

1. **Loops**: passives and buff reactions are linked when the damage of one sets off the trigger of the other. The check assumes any unit may carry any passive, so it also catches loops between different heroes.
   - `error`: a loop in which nothing has a `max_triggers_per_round`. No pack is written. The message shows the loop and asks for a limit on at least one passive or reaction in it.
   - `note`: a loop that limits end. It lists the passives involved and how often each unit can fire them a round.
   - These never loop: `unit_death` (a unit dies once), the response triggers, triggers only the battle itself sets off (`battle_start`, `round_start`, `before_action`, `on_attack`, `after_action`, `round_end`), healing and adding or removing buffs (they set off nothing), and anything with a `chance_bp` of 0.
2. **Stress battles**: 3 battles of 7v7 in which every unit carries every passive (in groups of 128 if there are more, the most one unit may have) and the first 128 skills, nobody dies, 5 rounds, at most 200,000 events. It reports the largest step, one unit's action or one run of triggers, and the passives that fired most in it.
   - `error`: a battle ran out of events, and the loop check found a loop.
   - `warning`: a battle ran out of events though every loop is limited, or a single step had more than 5,000 events. The limits are probably too high.
   - `note`: the size of the battles and their largest step.

`--stress-rounds N` and `--stress-units N` change the stress battles; `--no-stress` skips them. The loop check always runs.

For example, a counterattack with no limit and a thorns buff that reflects damage:

```text
error: runaway loop: nothing in it has a max_triggers_per_round, so the triggers keep setting each other off until the engine cuts the cascade at its trigger depth limit, or ends the battle at max_events:
    passive 801 "反击" (on_damaged, no limit) deals damage, which sets off on_damaged
    -> passive 801 "反击" (on_damaged, no limit) again
  All of these can set one another off: passive 801 "反击", buff 901 "荆棘" reaction 1.
  Give at least one of them a max_triggers_per_round.
error: stress battle (7v7, every unit with all 3 passives and 1 skill, nobody dies, 5 rounds) ran out of events: it reached max_events (200,000) in round 1. Largest step: 199,971 events in round 1, during unit 2007's action; most set off: passive 801 x49,996, buff 901 reactions x49,986, passive 803 x2.
config error: the cascade check found runaway loops; no pack was written
```

Giving passive 801 and the thorns reaction a `max_triggers_per_round` of 1 fixes it. The tables in [`tests/fixtures/config_runaway_loop`](../tests/fixtures/config_runaway_loop) reproduce this output.

## Building the pack

Validate without generating:

```powershell
.\scripts\build-config-compiler.ps1 -Configuration Release

.\erlang\bin\gamebattle_config_compiler.exe `
  --input-dir config/example `
  --check-only
```

Generate the binary config package:

```powershell
.\erlang\bin\gamebattle_config_compiler.exe `
  --input-dir config/example `
  --output build/config/battle.gbcfg
```

A `.gbcfg` is a deterministic little-endian binary package containing the magic number `GBCF`, major/minor version, payload length and CRC32. The current format version is `2.0`.
The payload first writes six record counts, for buffs, modifiers, reactions, effects, skills and passives, then the six kinds of records in turn; modifier and reaction records both carry their owning `buff_id` and `sequence`.
The same CSV files always produce exactly the same file, making it easy for a release system to compare hashes and roll back safely.
The config compiler itself uses C++20 and is maintained in the same CMake project as the battle engine, but is compiled through a separate target and build directory, and needs neither Python nor any Excel runtime. It links the battle core to run the stress battles.
It's off by default; a normal battle core build doesn't compile the config tool.
