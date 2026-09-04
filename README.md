# gamebattle

这是一个可由 Erlang 调用的 C++20 回合制战斗框架。当前版本同时提供：

- `open_port`：默认推荐的生产入口。C++ 崩溃只会带走 Port 进程，Erlang 监督树可以重启它。
- NIF：接近 BIF 的本地调用方式，使用 dirty CPU scheduler 运行完整战斗。
- 纯 C++ 核心：两种适配层共用一份状态机、随机数和协议解析，不会产生两套战斗结果。

普通 Erlang 应用无法添加真正的 VM BIF；那需要修改并重新编译 OTP。这里实现的 `gamebattle_nif:simulate/1` 是通常所说的“BIF 式调用”。战斗逻辑有崩溃或死循环时 NIF 会影响整个 BEAM，因此线上主路径建议使用 Port，NIF 只在充分压测和模糊测试后启用。

## 已实现的战斗流程

```text
解析并校验双方布阵
  -> 生成战斗对象（英雄 / 美人 / 宠物 / 神兵）
  -> 双方可行动对象基础速度之和 + 队形先手值，确定先手方
  -> battle_start 被动
  -> round_start Buff 挂点与被动
  -> 先手方全部对象依速度、位置行动
       -> before_action Buff / 被动
       -> 按技能优先级逐个判定触发，未触发则普攻
       -> 选目标、命中、伤害、暴击
       -> on_attack / on_hit / on_damaged / unit_death 被动
       -> 被动可以造成伤害、治疗、添加或移除 Buff
       -> after_action 被动与 Buff 挂点
  -> 后手方全部对象行动
  -> round_end Buff 挂点、过期与被动
  -> 判断胜负，否则下一回合
```

同一个 `seed` 和同一份输入会生成完全一致的结果与事件日志，可用于战报回放、线上问题复现和反作弊校验。

## 目录

- `include/gamebattle/engine.hpp`：稳定的 C++ 战斗领域模型。
- `include/gamebattle/config_store.hpp`：只读、可并发共享的配置内存仓库。
- `tools/config_compiler.cpp`：C++20 CSV 校验与 `.gbcfg` 二进制配置编译器。
- `config/example`：技能、效果、Buff、被动的策划表示例。
- `docs/buff-v2-design.md`：通用 Modifier、Reaction、生命周期和叠层策略设计。
- `src/engine.cpp`：只负责编排先后手、回合与行动顺序。
- `src/battle_state.cpp`：本场可变状态、初始条件、属性缓存和结果快照。
- `src/target_selector.cpp`：独立的目标选择策略。
- `src/effect_system.cpp`：技能、伤害、被动与 Buff 效果系统。
- `src/battle_runtime.hpp`：上述运行时组件之间的内部接口。
- `src/term.cpp`：无第三方依赖的 Erlang External Term Format 子集编解码。
- `src/port_main.cpp`：`{packet, 4}` Port 可执行程序。
- `src/nif.cpp`：dirty CPU NIF 适配器。
- `erlang/src/gamebattle_port.erl`：受监督、串行化请求的 Port worker。
- `erlang/src/gamebattle_nif.erl`：NIF 模块。
- `erlang/src/gamebattle.erl`：统一 API 与完整示例输入。

## Windows 构建

已安装 Visual Studio 2022、CMake 和 Erlang/OTP 时，在 PowerShell 中运行：

```powershell
.\scripts\build.ps1 -Configuration Release
```

脚本只编译战斗核心、Port、NIF 和核心测试，并把 Port/NIF 安装到 `erlang/priv`。配置工具是独立的可选目标，不会被普通战斗构建拉起。如果暂时不构建 NIF：

```powershell
.\scripts\build.ps1 -Configuration Release -WithoutNif
```

需要配置编译工具时单独构建，它使用独立的 `build-config-msvc` 目录：

```powershell
.\scripts\build-config-compiler.ps1 -Configuration Release
```

工具会安装到 `erlang/bin/gamebattle_config_compiler.exe`。对应的 CMake 开关是 `GAMEBATTLE_BUILD_CONFIG_COMPILER`，默认值为 `OFF`。

若 `rebar3` 不在 PATH，脚本仍会完成 C++ 构建；之后将 `rebar3` 加入 PATH，再运行：

```powershell
cd erlang
rebar3 compile
```

Linux 生产环境使用共享预设重新编译。Windows 生成的 `.exe` / `.dll` 不能部署到 Linux；建议在与生产系统一致的 Linux 容器、CI Runner 或服务器上产出 Release 文件。

```bash
# 默认推荐：只构建独立进程 Port
cmake --preset linux-runtime-release
cmake --build --preset build-linux-runtime-release --parallel
cmake --install out/build/linux-runtime-release \
  --prefix "$PWD/package" --component BattleRuntime

# 如果生产确实需要 NIF，变量必须是包含 erl_nif.h 的确切目录
export ERLANG_ERTS_INCLUDE_DIR=/usr/lib/erlang/erts-<otp-version>/include
cmake --preset linux-runtime-release-nif
cmake --build --preset build-linux-runtime-release-nif --parallel
cmake --install out/build/linux-runtime-release-nif \
  --prefix "$PWD/package" --component BattleRuntime
```

生产机或构建镜像需要 C++20 编译器、CMake 3.21+ 和 Make；构建 NIF 时，构建机的 Erlang/OTP 主版本应与生产运行时保持一致。若目标系统的 glibc 版本不同，尽量在较旧或与生产完全一致的发行版上编译。

## CLion Debug

项目根目录提供两层预设：

- `CMakePresets.json`：可提交、供团队共享，根据宿主系统只显示 Windows 或 Linux 对应项。
- `CMakeUserPresets.json`：当前 Windows 机器专用，已自动写入 OTP 29 的 ERTS 头文件路径，并由 `.gitignore` 排除。

Windows 上默认使用 Visual Studio 2022 x64，并把运行时、NIF 和配置工具放在不同构建目录。

1. 用CLion打开项目根目录。
2. 在 `Settings | Build, Execution, Deployment | Toolchains` 确认使用 Visual Studio 工具链。
3. 执行 `Load CMake Presets`，启用 `CLion | Battle Runtime | Debug`。
4. 新建或选择 `CMake Application` 运行配置，目标设为 `gamebattle_tests`。
5. 在 `BattleRunner::run`、`EffectSystem::apply_damage` 等位置设置断点后点击 Debug。

命令行可用同一套预设验证：

```powershell
cmake --preset clion-runtime-debug
cmake --build --preset build-runtime-debug
ctest --test-dir out/build/clion-runtime-debug -C Debug --output-on-failure
```

调试 NIF 时改用本机预设 `CLion | Battle Runtime + NIF | Debug (Local OTP 29)`，构建目标选择 `gamebattle_nif`；也可以一次构建和测试：

```powershell
cmake --preset clion-runtime-debug-nif-local
cmake --build --preset build-runtime-debug-nif-local
ctest --test-dir out/build/clion-runtime-debug-nif-local -C Debug --output-on-failure
```

调试配置工具时切换到 `CLion | Config Compiler | Debug`，运行目标选择 `gamebattle_config_compiler`，程序参数填写：

```text
--input-dir config/example --output config/generated/debug.gbcfg
```

`gamebattle_port` 启动后会等待Erlang ETF标准输入，因此调试纯战斗逻辑优先使用 `gamebattle_tests`。需要调试Port协议时，再由Erlang启动Port并让CLion附加到该进程。

## Erlang 调用

```erlang
application:ensure_all_started(gamebattle),
Request = gamebattle:example_request(),

%% 推荐：C++ 独立进程
{ok, Result1} = gamebattle:simulate(port, Request),

%% 可选：dirty NIF
{ok, Result2} = gamebattle:simulate(nif, Request),

true = (Result1 =:= Result2).
```

也可以通过应用配置指定 Port 路径与超时：

```erlang
application:set_env(gamebattle, port_executable, "D:/server/priv/gamebattle_port.exe"),
application:set_env(gamebattle, port_timeout, 30000).
```

Port worker 当前一次处理一场战斗，这是刻意的背压边界。需要并发时，应由监督树启动多个带名字或无注册名的 worker，再按 `battle_id` 做一致性分片；不要让多个 Erlang 进程直接争用同一个 Port 的响应。

## 策划配置编译与加载

战斗核心不直接读取 Excel。策划可以用 Excel 编辑六张 UTF-8 CSV，再由构建工具生成一个 C++ 可直接加载的配置包：

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
            C++ ConfigStore（只读内存）
```

通用 Buff 不再按属性或周期效果扩展固定字段。Buff 定义只组合生命周期策略、叠层策略、通用属性修改器和事件 Reaction；详细语义见 `docs/buff-v2-design.md`。

表字段、枚举和填写规则见 `config/README.md`。先校验配置：

```powershell
.\erlang\bin\gamebattle_config_compiler.exe `
  --input-dir config/example `
  --check-only
```

校验并生成配置包：

```powershell
.\erlang\bin\gamebattle_config_compiler.exe `
  --input-dir config/example `
  --output config/generated/battle.gbcfg
```

配置工具和战斗引擎都使用 C++20，由同一个 CMake 工程维护但分别构建；服务器和策划构建机不需要安装 Python。Linux 使用独立预设构建工具：

```bash
cmake --preset linux-config-compiler-release
cmake --build --preset build-linux-config-compiler-release --parallel
```

启动应用后，把配置加载到 Port 或 NIF 进程内：

```erlang
application:ensure_all_started(gamebattle),
{ok, #{buffs := 2, effects := 5, skills := 1, passives := 3}} =
    gamebattle:load_config(port, "D:/server/config/battle.gbcfg").

%% NIF 使用独立的进程内配置仓库，需要单独加载：
{ok, _} = gamebattle:load_config(nif, "D:/server/config/battle.gbcfg").
```

加载成功后，单位可以只传配置 ID，不再把完整技能结构重复发送给 C++：

```erlang
#{id => 1001,
  kind => hero,
  position => 1,
  level => 80,
  final_stats => #{hp => 1800, attack => 260, defense => 80, speed => 120},
  skill_ids => [501],
  passive_ids => [701]}.
```

`skills` 与 `skill_ids` 不能同时出现，`passives` 与 `passive_ids` 也不能同时出现。未加载配置包就使用 ID，或引用不存在的 ID，会返回 `invalid_request`。加载新包采用“先完整读取校验、成功后再切换”的方式；加载失败时旧配置继续服务，已经开始解析的战斗也继续使用它取得的旧配置快照。

`.gbcfg` 包含固定魔数、格式主/次版本、负载长度和 CRC32；输出按 ID 排序且不写时间戳，因此相同表格会生成完全相同的文件。发布系统可以先编译、测试和比较哈希，再调用 `load_config/2` 完成不重启进程的配置切换。

## 请求模型

请求是普通 Erlang map，通过 `term_to_binary/1` 发送。完整可运行样例在 `gamebattle:example_request/0`。

顶层字段：

| 字段 | 含义 |
|---|---|
| `battle_id` | 非负整数，透传到结果 |
| `seed` | 非负整数，决定所有概率和速度相同时的先手 |
| `max_rounds` | 最大回合数，默认 50 |
| `max_events` | 最大事件数，默认 10000，防止被动循环无限放大 |
| `attacker`, `defender` | 双方布阵 map |
| `initial_conditions` | 可选的本场运行时初值；未指定的对象默认满血 |

布阵结构：

```erlang
#{formation => crane_wing,
  initiative_bonus => 15,
  units => [Unit, ...]}.
```

战斗对象结构：

```erlang
#{id => 1001,
  kind => hero,                 %% hero | beauty | pet | artifact
  position => 1,
  level => 80,
  can_act => true,              %% 非英雄默认 false
  targetable => true,           %% 非英雄默认 false
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

所有概率和比例使用基点：`10000 = 100%`、`1500 = 15%`，避免不同语言的浮点差异。Erlang 传入的是已汇总的 `final_stats`；等级和养成等级会被保存为战斗快照元数据，但 v1 不在 C++ 内再次套成长公式，避免 Erlang 配置表和 C++ 公式形成双重真相。

### 内联 Buff 协议

不使用配置 ID 时，`add_buff` 的 `buff` 必须完整声明四个正交组件：

```erlang
#{id => 801,
  name => <<"中毒">>,
  lifetime => #{
      type => finite,              %% finite | permanent
      duration => 2,               %% permanent 必须为 0
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

`lifetime`、`stacking`、`modifiers`、`reactions` 在内联 ETF 中都必须显式出现，空组件使用空列表。协议会拒绝未知字段和旧版固定字段，不会静默套默认模型。永久 Buff 的 `duration` 必须为 `0`；`refresh` 只刷新持续时间，因此其 `max_stacks` 必须为 `1`。

Modifier 当前可选择 `attack`、`defense`、`speed`、`crit_rate_bp`、`crit_damage_bp`、`hit_rate_bp`、`dodge_rate_bp`、`damage_bonus_bp`、`damage_reduction_bp`。`add` 先累加固定值，`scale_bp` 再按增量万分比缩放，例如 `1000` 表示在加法阶段之后增加 10%。

Reaction 的目标选择始终以 Buff 持有者为上下文；`source` 只决定效果属性取自持有者还是施加者。`per_stack` 会按当前层数放大伤害、直接伤害和治疗的数值，不会重复执行添加或移除 Buff。线上请求通常优先使用 `skill_ids`、`passive_ids` 和已加载配置包，内联结构更适合测试与调试。

### 初始条件与连续战斗

布阵中的 `final_stats.hp` 始终是本场最大生命；上一场剩余生命通过独立的稀疏覆盖传入：

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

- 没有出现在 `unit_states` 中的对象按满血初始化。
- `current_hp = 0` 表示对象已经阵亡，本场不会行动或触发开场被动。
- `current_hp` 不能小于 0，也不能大于本场 `final_stats.hp`。
- 已阵亡对象不计入本场先手速度；一方所有英雄初始均阵亡时，战斗以 `initial_state` 立即结束。
- Buff、能量、冷却目前不会跨场继承，后续可继续在 `UnitInitialState` 中增加独立字段。

Erlang 可以从上一场结果生成下一场某一方的初始条件：

```erlang
{ok, FirstResult} = gamebattle:simulate(FirstRequest),
Carryover = gamebattle:carryover(attacker, FirstResult),
SecondRequest = SecondBaseRequest#{initial_conditions => Carryover},
{ok, SecondResult} = gamebattle:simulate(SecondRequest).
```

### 车轮战编排

车轮战保持“一波敌人对应一场独立战斗”。攻击方在波次之间继承剩余生命，新的防守方按自己的布阵满血进入；攻击方失败、打平或某波执行错误时立即停止。

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

汇总结果示例：

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

每个 Wave 必须提供唯一的 `battle_id`、确定性 `seed` 和 `defender` 布阵，可单独覆盖 `max_rounds`、`max_events`、`first_side`。若服务在中间波次发生执行错误，返回值中仍保留已经完成的 `wave_results` 和最后一份 `carryover`；把剩余 Waves 与该 carryover 作为 `initial_conditions` 传给 `run_gauntlet/4` 即可断点恢复。

技能效果支持：

- `damage`：`attack_bp` 倍率加 `flat` 固定值，再扣防御并计算增伤、减伤、暴击。
- `direct_damage`：倍率加固定值，跳过命中、防御、暴击、增伤与减伤，仍触发受击和死亡事件。
- `heal`：攻击倍率加固定值。
- `add_buff`：按照 Buff 的 `StackingPolicy` 添加、叠层或刷新。
- `remove_buff`：必须提供非零 `buff_id`，只移除指定 Buff；当前不提供隐式“清全部”通配语义。

目标规则支持 `self`、`trigger_unit`、`enemy_front`、`enemy_lowest_hp`、`ally_lowest_hp`、`all_enemies`、`all_allies`。`trigger_unit` 用于“命中者给本次受击者挂毒”或“受击者反击本次攻击者”。

被动和 Buff Reaction 共用 `battle_start`、`round_start`、`before_action`、`on_attack`、`on_hit`、`on_damaged`、`unit_death`、`after_action`、`round_end` 触发点。强烈建议连锁效果设置 `max_triggers_per_round`；框架另有 32 层触发深度和 `max_events` 两道保险。

Buff 的持续计数可以选择在哪一种 Trigger 后递减；永久 Buff 不递减。周期伤害、持续治疗、受击反击等都表示为 Reaction 执行普通 Effect，不再由单独的 Tick 字段和代码路径处理。

## 结果与战报

成功返回 `{ok, Result}`，其中：

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

事件包含严格递增的 `seq`，以及 `round`、`phase`、`type`、`actor`、`target`、`source_id`、`value`、受击前后 HP 和暴击标记。前端可以只依赖事件流播放战报，服务端则以 `units` 和 `winner` 做最终结算。

## v1 的明确边界

这一版提供的是可接入、可回放的骨架，不假定具体游戏数值。能量、技能冷却、控制抗性、护盾、复活、召唤物、阵型邻接、效果驱散标签和属性快照/动态取值规则尚未实现。新增这些能力时，优先扩展 `EffectKind` 和事件类型，保持 Erlang 请求 schema 向后兼容；不要在 Erlang 与 C++ 两边各写一份伤害公式。
