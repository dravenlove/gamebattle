# gamebattle

[中文](README.md) | **English**

A C++20 turn-based battle framework callable from Erlang. The current version provides all of:

- `open_port`: the recommended production entry point. A C++ crash only takes down the Port process, which the Erlang supervision tree can restart.
- NIF: a near-BIF native call path that runs the whole battle on a dirty CPU scheduler.
- A pure C++ core: both adapters share one state machine, one RNG and one protocol parser, so they can never produce two different battle results.

An ordinary Erlang application can't add a true VM BIF; that requires modifying and recompiling OTP. The `gamebattle_nif:simulate/1` implemented here is what's usually called a "BIF-style call". If the battle logic crashes or loops forever, a NIF affects the whole BEAM, so the main production path should use the Port, and the NIF should only be enabled after thorough load testing and fuzzing.

## The battle flow implemented

```text
Parse and validate both formations
  -> create battle objects (heroes / beauties / pets / artifacts)
  -> sum of base speed of each side's units that can act + formation initiative bonus decides who moves first
  -> battle_start passives
  -> round_start buff hooks and passives
  -> every unit on the first side acts, ordered by speed and position
       -> before_action buffs / passives
       -> skills are rolled one by one in priority order; basic attack if none triggers
       -> an activated skill first opens a chain: both sides' response passives join in turn, and the last to join resolves first
       -> target selection, hit, damage, crit
       -> on_attack / on_hit / on_damaged / unit_death passives
       -> passives can deal damage, heal, add or remove buffs
       -> after_action passives and buff hooks
  -> every unit on the second side acts
  -> round_end buff hooks, expiry and passives
  -> check for a winner, otherwise next round
```

The same `seed` and the same input produce exactly the same result and event log, usable for battle replays, reproducing production problems and anti-cheat verification.

## Layout

- `include/gamebattle/engine.hpp`: the stable C++ battle domain model.
- `include/gamebattle/config_store.hpp`: a read-only in-memory config store that can be shared concurrently.
- `tools/config_compiler.cpp`: a C++20 CSV validator and `.gbcfg` binary config compiler.
- `config/example`: sample designer tables for skills, effects, buffs and passives.
- `docs/buff-v2-design.md`: the design of generic modifiers, reactions, lifetimes and stacking policies.
- `proto/battle_client.proto`: the game-client protocol (protobuf); see `docs/client-protocol.en.md`.
- `src/engine.cpp`: only orchestrates first/second side, rounds and acting order.
- `src/battle_state.cpp`: per-battle mutable state, initial conditions, attribute cache and result snapshot.
- `src/target_selector.cpp`: a standalone target selection strategy.
- `src/effect_system.cpp`: the skill, damage, passive and buff effect system.
- `src/battle_runtime.hpp`: the internal interfaces between the runtime components above.
- `src/term.cpp`: encoding/decoding of a subset of the Erlang External Term Format, with no third-party dependencies.
- `src/port_main.cpp`: the `{packet, 4}` Port executable.
- `src/nif.cpp`: the dirty CPU NIF adapter.
- `erlang/src/gamebattle_port.erl`: a supervised Port worker that serializes requests.
- `erlang/src/gamebattle_nif.erl`: the NIF module.
- `erlang/src/gamebattle.erl`: the unified API and a complete sample input.
- `erlang/src/gamebattle_erl*.erl`: the battle engine in plain Erlang, a line-by-line port of the C++ engine with identical results; see `docs/engine-benchmark.en.md` for how it compares with the NIF and the Port.
- `erlang/src/gamebattle_client.erl`: converts engine results to client protobuf messages and validates client requests.
- `Dockerfile`, `compose.yaml`, `docker/`, `erlang/config/vm.args.src`: the Docker build and deploy images; see "Docker build and deployment".

## Building on Windows

With Visual Studio 2022, CMake and Erlang/OTP installed, run in PowerShell:

```powershell
.\scripts\build.ps1 -Configuration Release
```

The script compiles only the battle core, Port, NIF and core tests, and installs the Port/NIF into `erlang/priv`. The config tool is a separate optional target that the normal battle build doesn't pull in. To skip the NIF for now:

```powershell
.\scripts\build.ps1 -Configuration Release -WithoutNif
```

When you need the config compiler, build it separately; it uses its own `build-config-msvc` directory:

```powershell
.\scripts\build-config-compiler.ps1 -Configuration Release
```

The tool is installed to `erlang/bin/gamebattle_config_compiler.exe`. The corresponding CMake switch is `GAMEBATTLE_BUILD_CONFIG_COMPILER`, which defaults to `OFF`.

If `rebar3` isn't on PATH, the script still completes the C++ build; afterwards add `rebar3` to PATH and run:

```powershell
cd erlang
rebar3 compile
```

Linux production builds are recompiled with the shared presets. The `.exe` / `.dll` files produced on Windows can't be deployed to Linux; produce Release artifacts in a Linux container, CI runner or server matching the production system.

```bash
# Recommended default: build only the standalone-process Port
cmake --preset linux-runtime-release
cmake --build --preset build-linux-runtime-release --parallel
cmake --install out/build/linux-runtime-release \
  --prefix "$PWD/package" --component BattleRuntime

# If production really needs the NIF, the variable must be the exact directory containing erl_nif.h
export ERLANG_ERTS_INCLUDE_DIR=/usr/lib/erlang/erts-<otp-version>/include
cmake --preset linux-runtime-release-nif
cmake --build --preset build-linux-runtime-release-nif --parallel
cmake --install out/build/linux-runtime-release-nif \
  --prefix "$PWD/package" --component BattleRuntime
```

The production machine or build image needs a C++20 compiler, CMake 3.21+ and Make; when building the NIF, the build machine's Erlang/OTP major version should match the production runtime. If the target system's glibc version differs, compile on an older distribution or one identical to production.

## Docker build and deployment

To avoid installing the toolchain locally, or to produce files on a Linux that matches production, use the `Dockerfile` at the repository root. It has three targets:

| Target | Contents |
|---|---|
| `build` | The build environment: compiles the Port, the NIF, the config compiler and the Erlang code, runs the C++ tests and `rebar3 eunit` (including an end-to-end test through the real Port), then assembles the release. Any failing step fails the build. |
| `artifacts` | Just the outputs, exported to the host with `--output`. |
| `runtime` (default) | The deploy image: a standalone battle node. It is an Erlang release with its own ERTS, and game nodes call it over distributed Erlang. |

### The build version

```bash
# Build, run every test and export the Linux outputs to dist/
docker build --target artifacts --output type=local,dest=dist .
```

`dist/` contains:

- `priv/gamebattle_port`, `priv/gamebattle_nif.so`: put these in your own Erlang application's `priv/` directory;
- `bin/gamebattle_config_compiler`: the config compiler;
- `gamebattle-0.1.0.tar.gz`: the complete battle-node release; unpack it and run `bin/gamebattle foreground` (environment variables below).

The outputs are compiled on Debian bookworm, the base of the `erlang:26` image: the target's glibc must not be older than that, and the NIF works only with OTP 26. To change the OTP version, `RUNTIME_IMAGE` must use the same Debian release as the new image (check with `docker run --rm erlang:27 cat /etc/os-release`):

```bash
docker build --build-arg ERLANG_IMAGE=erlang:27 --build-arg RUNTIME_IMAGE=debian:bookworm-slim \
  --target artifacts --output type=local,dest=dist .
```

The `build` target also works as a tool image for compiling designer config:

```bash
docker build --target build -t gamebattle-build .
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD/config:/config" gamebattle-build \
  gamebattle_config_compiler --input-dir /config/example --output /config/generated/battle.gbcfg
```

### The deploy version

Generate `config/generated/battle.gbcfg` as above, then start the battle node:

```bash
export ERLANG_COOKIE="$(openssl rand -hex 32)"   # the same cookie as the game nodes
docker compose up -d --build
```

`compose.yaml` starts a node named `gamebattle@battle`, mounts `config/generated` read-only into the container, and has the Port load `battle.gbcfg` every time it starts. Game nodes on the same Docker network, started with a short name (`-sname`) and the same cookie, call it like this:

```erlang
{ok, Result} = erpc:call('gamebattle@battle', gamebattle, simulate, [port, Request], 35000).
```

When the game nodes run on other machines with long names (`-name`), give the container the host's network and use the host's IP in the node name:

```bash
docker build -t gamebattle .
docker run -d --name battle --init --restart unless-stopped --network host \
  -e NODE_NAME=gamebattle@10.0.0.5 -e ERLANG_COOKIE="$ERLANG_COOKIE" \
  -e GAMEBATTLE_CONFIG=/etc/gamebattle/battle.gbcfg \
  -v "$PWD/config/generated:/etc/gamebattle:ro" gamebattle
```

| Environment variable | Default | Meaning |
|---|---|---|
| `ERLANG_COOKIE` | none, required | The cluster cookie. Without it the container exits at once. |
| `NODE_NAME` | `gamebattle@127.0.0.1` | The node name. The part after `@` must be the host name or IP the game nodes reach this container at. |
| `NODE_NAME_TYPE` | `name` | `name` (long names: the part after `@` must contain a dot, such as an IP) or `sname` (short names). It must match the game nodes; nodes of the two kinds cannot connect. |
| `GAMEBATTLE_CONFIG` | nothing loaded | The config package path. The Port loads it every time it starts, including after a crash. |
| `DIST_PORT` | `9100` | The distributed-Erlang port, to be opened to game nodes together with epmd's `4369`. |

Operations:

```bash
docker exec battle bin/gamebattle eval 'gamebattle_port:ping().'   # {ok, pong}
docker exec -it battle bin/gamebattle remote_console
docker logs -f battle
```

- **Security**: anyone who has the cookie and can reach `4369` and `9100` can run any code on the node. Open these ports only to game nodes on the internal network, never to the internet, and use a long random cookie. The container runs as a non-root user and the release directory is read-only.
- **Recovery**: a crashed Port is restarted by the supervisor and reloads the config. More than 5 crashes in 10 seconds stop the `gamebattle` application, the node exits with it, and Docker's restart policy starts a new container. A missing or invalid config package makes the node fail at start-up, with `config_load_failed` in the log.
- **Build network**: the build needs Docker Hub, the Debian package mirrors and hex.pm. If Docker Hub is hard to reach, point at a mirror with `--build-arg ERLANG_IMAGE=<mirror>/library/erlang:26 --build-arg RUNTIME_IMAGE=<mirror>/library/debian:bookworm-slim`.

## Debugging in CLion

The project root provides two layers of presets:

- `CMakePresets.json`: committed and shared by the team; only the Windows or Linux entries are shown, depending on the host system.
- `CMakeUserPresets.json`: specific to the current Windows machine, with the OTP 29 ERTS header path filled in automatically, and excluded by `.gitignore`.

On Windows it uses Visual Studio 2022 x64 by default, with the runtime, NIF and config tool in separate build directories.

1. Open the project root in CLion.
2. Under `Settings | Build, Execution, Deployment | Toolchains`, make sure the Visual Studio toolchain is used.
3. Run `Load CMake Presets` and enable `CLion | Battle Runtime | Debug`.
4. Create or select a `CMake Application` run configuration with the target set to `gamebattle_tests`.
5. Set breakpoints in places such as `BattleRunner::run` and `EffectSystem::apply_damage`, then click Debug.

The same presets can be checked from the command line:

```powershell
cmake --preset clion-runtime-debug
cmake --build --preset build-runtime-debug
ctest --test-dir out/build/clion-runtime-debug -C Debug --output-on-failure
```

To debug the NIF, switch to the local preset `CLion | Battle Runtime + NIF | Debug (Local OTP 29)` and choose `gamebattle_nif` as the build target; or build and test in one go:

```powershell
cmake --preset clion-runtime-debug-nif-local
cmake --build --preset build-runtime-debug-nif-local
ctest --test-dir out/build/clion-runtime-debug-nif-local -C Debug --output-on-failure
```

To debug the config tool, switch to `CLion | Config Compiler | Debug`, choose `gamebattle_config_compiler` as the run target, and set the program arguments to:

```text
--input-dir config/example --output config/generated/debug.gbcfg
```

Once started, `gamebattle_port` waits for Erlang ETF on standard input, so prefer `gamebattle_tests` for debugging pure battle logic. To debug the Port protocol, have Erlang start the Port and attach CLion to that process.

## Calling from Erlang

```erlang
application:ensure_all_started(gamebattle),
Request = gamebattle:example_request(),

%% Recommended: a separate C++ process
{ok, Result1} = gamebattle:simulate(port, Request),

%% Optional: dirty NIF
{ok, Result2} = gamebattle:simulate(nif, Request),

true = (Result1 =:= Result2).

%% The engine in plain Erlang: runs in the calling process, same results as C++
{ok, Result3} = gamebattle:simulate(erlang, Request),
true = (Result1 =:= Result3).
```

See [docs/engine-benchmark.en.md](docs/engine-benchmark.en.md) for how fast the three are, where the differences come from, and which to choose.

The Port path and timeout can also be set through application config:

```erlang
application:set_env(gamebattle, port_executable, "D:/server/priv/gamebattle_port.exe"),
application:set_env(gamebattle, port_timeout, 30000).
```

A freshly started C++ process has no config at all. With `config_path` (or the `GAMEBATTLE_CONFIG` environment variable) set, the Port loads that package before serving any request every time it starts, including after a crash; if loading fails, the worker fails to start. A successful `load_config(port, Path)` records the new path, so later restarts load the most recently loaded package:

```erlang
application:set_env(gamebattle, config_path, "D:/server/config/battle.gbcfg").
```

A Port worker currently handles one battle at a time; this is a deliberate backpressure boundary. When you need concurrency, have the supervision tree start several named or unregistered workers and shard consistently by `battle_id`; don't let several Erlang processes compete directly for the same Port's responses.

## Compiling and loading designer config

The battle core doesn't read Excel directly. Designers can edit six UTF-8 CSV files in Excel, and a build tool then generates a config package C++ can load directly:

```text
buff_modifiers.csv ───────────────→ buffs.csv
buff_reactions.csv → effects.csv ─→ buffs.csv
skills.csv ────────→ effects.csv
passives.csv ──────→ effects.csv
                         ↓
             gamebattle_config_compiler
                         ↓
                  battle.gbcfg
                         ↓
            C++ ConfigStore (read-only memory)
```

Generic buffs no longer add fixed fields per attribute or periodic effect. A buff definition only combines a lifetime policy, a stacking policy, generic attribute modifiers and event reactions; see `docs/buff-v2-design.md` for the detailed semantics.

Table fields, enums and filling rules are in `config/README.md`. Validate the config first:

```powershell
.\erlang\bin\gamebattle_config_compiler.exe `
  --input-dir config/example `
  --check-only
```

Validate and generate the config package:

```powershell
.\erlang\bin\gamebattle_config_compiler.exe `
  --input-dir config/example `
  --output config/generated/battle.gbcfg
```

The config tool and the battle engine both use C++20 and are maintained in the same CMake project but built separately; neither servers nor designers' build machines need Python. On Linux, build the tool with its own preset:

```bash
cmake --preset linux-config-compiler-release
cmake --build --preset build-linux-config-compiler-release --parallel
```

After starting the application, load the config into the Port or NIF process:

```erlang
application:ensure_all_started(gamebattle),
{ok, #{buffs := 2, effects := 5, skills := 1, passives := 3}} =
    gamebattle:load_config(port, "D:/server/config/battle.gbcfg").

%% The NIF and the plain-Erlang engine each keep their own copy; load the ones you use:
{ok, _} = gamebattle:load_config(nif, "D:/server/config/battle.gbcfg"),
{ok, _} = gamebattle:load_config(erlang, "D:/server/config/battle.gbcfg").
```

Once loaded, units can send just config IDs instead of repeatedly sending full skill structures to C++:

```erlang
#{id => 1001,
  kind => hero,
  position => 1,
  level => 80,
  final_stats => #{hp => 1800, attack => 260, defense => 80, speed => 120},
  skill_ids => [501],
  passive_ids => [701]}.
```

`skills` and `skill_ids` can't appear together, nor can `passives` and `passive_ids`. Using IDs without a loaded config package, or referencing an ID that doesn't exist, returns `invalid_request`. Loading a new package follows "read and validate it completely first, switch only on success"; if loading fails, the old config keeps serving, and battles that have already started parsing keep using the old config snapshot they obtained.

A `.gbcfg` contains a fixed magic number, format major/minor version, payload length and CRC32; output is sorted by ID with no timestamp, so identical tables always produce identical files. A release system can compile, test and compare hashes first, then call `load_config/2` to switch config without restarting the process.

## Request model

A request is an ordinary Erlang map sent via `term_to_binary/1`. A complete runnable sample is in `gamebattle:example_request/0`.

Top-level fields:

| Field | Meaning |
|---|---|
| `battle_id` | Non-negative integer, passed through to the result |
| `seed` | Non-negative integer that determines every probability roll and who moves first on a speed tie |
| `max_rounds` | Maximum number of rounds, default 50 |
| `max_events` | Maximum number of events, default 10000, stopping passive loops from blowing up without bound |
| `attacker`, `defender` | The two formation maps |
| `initial_conditions` | Optional runtime initial values for this battle; units not mentioned start at full HP |

Formation structure:

```erlang
#{formation => crane_wing,
  initiative_bonus => 15,
  units => [Unit, ...]}.
```

Battle object structure:

```erlang
#{id => 1001,
  kind => hero,                 %% hero | beauty | pet | artifact
  position => 1,
  level => 80,
  can_act => true,              %% defaults to false for non-heroes
  targetable => true,           %% defaults to false for non-heroes
  growth_levels => #{star => 10, breakthrough => 6},
  final_stats => #{
      hp => 1800, attack => 260, defense => 80, speed => 120,
      crit_rate_bp => 1500, crit_damage_bp => 15000,
      hit_rate_bp => 10000, dodge_rate_bp => 300,
      damage_bonus_bp => 0, damage_reduction_bp => 0
  },
  skills => [Skill, ...],
  passives => [Passive, ...]}.
```

All probabilities and ratios use basis points: `10000 = 100%`, `1500 = 15%`, avoiding floating-point differences between languages. Erlang sends already aggregated `final_stats`; levels and growth levels are stored as battle snapshot metadata, but v1 doesn't apply growth formulas again inside C++, to avoid Erlang config tables and C++ formulas becoming two sources of truth.

### The inline buff protocol

When config IDs aren't used, the `buff` of an `add_buff` must fully declare its four orthogonal components:

```erlang
#{id => 801,
  name => <<"Poison">>,
  lifetime => #{
      type => finite,              %% finite | permanent
      duration => 2,               %% must be 0 for permanent
      decrement_on => round_end
  },
  stacking => #{
      max_stacks => 3,
      policy => stack,             %% stack | refresh
      refresh => reset             %% reset | extend | keep
  },
  modifiers => [
      #{attribute => defense, operation => add, value => -20}
  ],
  reactions => [#{
      trigger => round_end,
      source => applier,           %% owner | applier
      stack_scaling => per_stack,  %% once | per_stack
      chance_bp => 10000,
      max_triggers_per_round => 0,
      effects => [#{type => direct_damage, target => self, flat => 35}]
  }]}.
```

`lifetime`, `stacking`, `modifiers` and `reactions` must all appear explicitly in inline ETF, with empty lists for empty components. The protocol rejects unknown fields and old-style fixed fields instead of silently applying a default model. A permanent buff's `duration` must be `0`; `refresh` only refreshes the duration, so its `max_stacks` must be `1`.

Modifiers can currently target `attack`, `defense`, `speed`, `crit_rate_bp`, `crit_damage_bp`, `hit_rate_bp`, `dodge_rate_bp`, `damage_bonus_bp` and `damage_reduction_bp`. `add` accumulates flat values first, then `scale_bp` scales by an incremental basis-point amount; for example `1000` means +10% after the addition stage.

A reaction's target selection always uses the buff holder as its context; `source` only decides whether effect stats come from the holder or the applier. `per_stack` scales the values of damage, direct damage and healing by the current stack count; it never repeats adding or removing buffs. Production requests usually prefer `skill_ids`, `passive_ids` and a loaded config package; the inline structure is better suited to testing and debugging.

### Initial conditions and consecutive battles

`final_stats.hp` in a formation is always this battle's maximum HP; HP left over from the previous battle is passed in through a separate sparse override:

```erlang
#{
    attacker => AttackerFormation,
    defender => DefenderFormation,
    initial_conditions => #{
        source_battle_id => 10001,
        first_side => automatic, %% automatic | attacker | defender
        unit_states => [
            #{unit_id => 1001, current_hp => 760},
            #{unit_id => 1002, current_hp => 0}
        ]
    }
}.
```

- Units not listed in `unit_states` start at full HP.
- `current_hp = 0` means the unit is already dead and won't act or trigger battle-start passives in this battle.
- `current_hp` can't be below 0 or above this battle's `final_stats.hp`.
- Dead units don't count toward this battle's first-move speed; when all of one side's heroes start out dead, the battle ends immediately with `initial_state`.
- Buffs, energy and cooldowns aren't currently carried between battles; separate fields can be added to `UnitInitialState` later.

Erlang can generate one side's initial conditions for the next battle from the previous result:

```erlang
{ok, FirstResult} = gamebattle:simulate(FirstRequest),
Carryover = gamebattle:carryover(attacker, FirstResult),
SecondRequest = SecondBaseRequest#{initial_conditions => Carryover},
{ok, SecondResult} = gamebattle:simulate(SecondRequest).
```

### Gauntlet orchestration

A gauntlet keeps "one wave of enemies = one independent battle". The attacker carries remaining HP between waves, while each new defender enters at full HP with its own formation; it stops immediately when the attacker loses, draws, or a wave fails to execute.

```erlang
Base = gamebattle:example_request(),
Attacker = maps:get(attacker, Base),
Defender1 = maps:get(defender, Base),
Defender2 = AnotherDefenderFormation,

Waves = [
    #{battle_id => 3001, seed => 101, defender => Defender1},
    #{battle_id => 3002, seed => 102, defender => Defender2,
      max_rounds => 30, first_side => automatic}
],

{ok, GauntletResult} = gamebattle:run_gauntlet(
    port,
    Attacker,
    Waves,
    #{max_rounds => 20, max_events => 5000}
).
```

A sample aggregated result:

```erlang
#{
    status := completed | defeated | draw,
    winner := attacker | defender | draw,
    completed_waves := WonWaveCount,
    fought_waves := FoughtWaveCount,
    total_waves := TotalWaveCount,
    stopped_at_wave := 0 | WaveIndex,
    wave_results := [BattleResult, ...],
    carryover := NextInitialConditions
}.
```

Each wave must provide a unique `battle_id`, a deterministic `seed` and a `defender` formation, and may override `max_rounds`, `max_events` and `first_side` individually. If the service fails partway through a wave, the return value still keeps the completed `wave_results` and the last `carryover`; pass the remaining waves plus that carryover as `initial_conditions` to `run_gauntlet/4` to resume from that point.

Supported skill effects:

- `damage`: `attack_bp` ratio plus a `flat` value, then defense is subtracted and damage bonus, damage reduction and crits are applied.
- `direct_damage`: ratio plus flat value, skipping hit, defense, crit, damage bonus and damage reduction; it still triggers damaged and death events.
- `heal`: attack ratio plus a flat value.
- `add_buff`: adds, stacks or refreshes according to the buff's `StackingPolicy`.
- `remove_buff`: requires a non-zero `buff_id` and removes only that buff; there's currently no implicit "clear everything" wildcard.
- `negate`: cancels the chain link this response answered (the skill or the previous response). Only valid in `enemy_activate` and `ally_activate` passives; see "Chains and responses" below.

Supported target rules: `self`, `trigger_unit`, `enemy_front`, `enemy_lowest_hp`, `ally_lowest_hp`, `all_enemies`, `all_allies`. `trigger_unit` is for things like "the attacker poisons the unit it just hit" or "the unit hit counterattacks this attacker".

Passives and buff reactions share the trigger points `battle_start`, `round_start`, `before_action`, `on_attack`, `on_hit`, `on_damaged`, `unit_death`, `after_action` and `round_end`. Setting `max_triggers_per_round` on chained effects is strongly recommended; the framework also has two safeguards, a trigger depth of 32 and `max_events`. Passives can also use the two response triggers `enemy_activate` and `ally_activate`.

A buff's duration counter can be set to decrement after a chosen trigger; permanent buffs don't decrement. Damage over time, healing over time, counterattacks on being hit and so on are all expressed as reactions running ordinary effects, rather than handled by separate tick fields and code paths.

### Chains and responses

When an active skill (not a basic attack) is activated, it doesn't resolve immediately. It opens a **chain** first, with rules similar to Yu-Gi-Oh:

1. The skill itself is link 1.
2. Ask who wants to respond to the top link: first the other side (`enemy_activate` passives), then the other units on the same side (`ally_activate` passives). Within a side, units are asked in speed, position and ID order; the first passive that passes its chance roll joins the chain as the new top link, and the asking starts over.
3. Each unit adds at most one link per chain; asking stops when nobody responds.
4. Links resolve in reverse, starting from the last one added. `negate` makes the link it answered be skipped; if a link's owner has died by the time it would resolve, that link fizzles.
5. Damage dealt while a link resolves still triggers ordinary passives such as `on_hit` and `on_damaged` immediately, exactly as before.

A response link's `trigger_unit` is the owner of the link it answered, so "counterattack the caster" is `target => trigger_unit`, and so is "buff the ally who cast".

```erlang
%% When the other side activates a skill, negate it with 50% chance, at most once per round
#{id => 711, name => <<"counter_spell">>, trigger => enemy_activate,
  chance_bp => 5000, max_triggers_per_round => 1,
  effects => [#{type => negate}]}.

%% When an ally activates a skill, buff the caster's attack first; the skill then resolves with the boosted attack (a combo)
#{id => 712, name => <<"support">>, trigger => ally_activate,
  effects => [#{type => add_buff, target => trigger_unit, buff => RallyBuff}]}.
```

- When no unit has a response passive, skills resolve exactly as before and consume no extra random numbers, so results for old requests are byte-identical.
- Response triggers can only be used by passives; `negate` can only appear in response passives; buff reactions and `decrement_on` can't use response triggers. Violations make the request return `invalid_request`, and the config compiler and loader reject them too.
- `chance_bp` and `max_triggers_per_round` apply as usual; a response that gets negated still counts as a trigger.

## Results and battle reports

Success returns `{ok, Result}`, where:

```erlang
#{battle_id := BattleId,
  seed := Seed,
  source_battle_id := PreviousBattleId,
  winner := attacker | defender | draw,
  reason := initial_state | all_units_defeated | round_end | max_rounds | event_limit | battle_start,
  rounds := RoundCount,
  attacker_initiative := Integer,
  defender_initiative := Integer,
  events := [Event, ...],
  units := [#{id := Id, side := Side, initial_hp := InitialHp,
              hp := Hp, max_hp := MaxHp, alive := Bool}, ...]}.
```

Events carry a strictly increasing `seq`, plus `round`, `phase`, `type`, `actor`, `target`, `source_id`, `value`, HP before and after the hit, and a crit flag. The client can play back a battle report from the event stream alone, while the server settles based on `units` and `winner`.

Chains produce the following events, only when someone responds:

| `type` | `actor` | `target` | `source_id` | `value` |
|---|---|---|---|---|
| `chain` | The responder | The unit it answered | The response passive's ID | Link number (from 2) |
| `negate` | The negating unit | Owner of the negated link | The negated skill or passive ID | Number of the negated link |
| `fizzle` | Owner of the fizzled link | 0 | The fizzled skill or passive ID | Link number |

## Client protocol

Game clients talk to the server in protobuf, defined in `proto/battle_client.proto`. `gamebattle_client:encode_battle_report/2` encodes the result above as the `ServerMessage` sent to clients, and `gamebattle_client:decode_client_message/1` decodes and validates the `ClientMessage` a client sends. The client submits only a stage and a lineup; every number comes from the server.

See [docs/client-protocol.en.md](docs/client-protocol.en.md) for framing, an Erlang server example, Unity/C# integration, security notes and compatibility rules.

## The explicit boundaries of v1

This version provides an integrable, replayable skeleton and assumes no particular game's numbers. Energy, skill cooldowns, control resistance, shields, revival, summons, formation adjacency, effect dispel tags, and rules for attribute snapshots / dynamic values are not implemented yet. When adding these capabilities, prefer extending `EffectKind` and the event types, keep the Erlang request schema backward compatible, and never write the damage formula twice, once in Erlang and once in C++.
