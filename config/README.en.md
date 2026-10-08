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
- For effects of other types, `buff_id`/`remove_buff_id` must be `0` or empty.
- Blank numeric values use the compiler's defaults, but the columns themselves can't be deleted or renamed.
- IDs must be unique within their own table; every cross-table reference is checked during generation.

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
The config compiler itself uses C++20 and is maintained in the same CMake project as the battle engine, but is compiled through a separate target and build directory, and needs neither Python nor any Excel runtime.
It's off by default; a normal battle core build doesn't compile the config tool.
