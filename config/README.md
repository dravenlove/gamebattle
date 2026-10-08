# 战斗配置表

**中文** | [English](README.en.md)

策划维护以下六张 UTF-8 CSV：

- `buffs.csv`：Buff 身份、生命周期和叠层策略。
- `buff_modifiers.csv`：Buff 对任意战斗属性的通用修改器。
- `buff_reactions.csv`：Buff 在战斗事件发生时引用的效果序列。
- `effects.csv`：伤害、治疗、添加 Buff、移除 Buff等效果定义。
- `skills.csv`：主动技能及其效果序列。
- `passives.csv`：被动触发条件及其效果序列。

Excel 可以直接编辑；保存时请选择 `CSV UTF-8`。`notes` 仅供策划阅读，不写入运行时配置包。

引用方向固定为：

```text
skills/passives -------------> effects -> buffs
buffs -> buff_reactions -----> effects -> buffs
buffs -> buff_modifiers
```

- `effect_ids` 使用 `|` 分隔，并保持执行顺序，例如 `9001|9002`。
- 所有概率和百分比使用万分比：`10000` 表示 100%，`3500` 表示 35%。
- `lifetime` 支持 `finite`、`permanent`；永久 Buff 的 `duration` 必须为 `0`。
- `decrement_on` 使用被动相同的触发点；永久 Buff 中该列仍需填写，但运行时会忽略。
- `stack_policy` 支持 `stack`、`refresh`；`refresh` 的 `max_stacks` 必须为 `1`。
- `refresh_policy` 支持 `reset`、`extend`、`keep`。
- modifier 的 `attribute` 支持 `attack`、`defense`、`speed`、`crit_rate_bp`、`crit_damage_bp`、`hit_rate_bp`、`dodge_rate_bp`、`damage_bonus_bp`、`damage_reduction_bp`。
- modifier 的 `operation` 支持 `add` 和 `scale_bp`；modifier 会按 Buff 当前层数应用。
- `buff_modifiers.csv` 和 `buff_reactions.csv` 使用 `(buff_id, sequence)` 作为组合唯一键，并按 `sequence` 执行。
- reaction 的 `source` 支持 `owner`、`applier`。目标选择始终以 Buff 持有者为上下文；`source` 只决定效果属性和事件来源。
- reaction 的 `stack_scaling` 支持 `once`、`per_stack`；后者按当前 Buff 层数放大伤害、治疗或直接伤害，添加/移除 Buff 不会重复执行。
- reaction 引用 `add_buff` 效果时不能形成有向环，否则共享配置定义会产生所有权环，编译器和加载器都会拒绝。
- `direct_damage` 是不经过命中、暴击、防御、增伤和减伤公式的直接伤害；持续伤害可以通过 reaction 引用它。
- `add_buff` 效果必须填写 `buff_id`。
- `remove_buff` 效果必须填写 `remove_buff_id`。
- `enemy_activate`、`ally_activate` 是被动专用的响应触发点（规则见根目录 README 的「连锁与响应」），不能用于 `buff_reactions.csv` 的 `trigger` 和 `buffs.csv` 的 `decrement_on`。
- `negate` 效果无效响应所回应的连锁环节，只能被 `trigger` 为 `enemy_activate` 或 `ally_activate` 的被动引用；技能和 Buff reaction 都不能引用它。`target` 列对它没有作用，可以填 `trigger_unit`。
- 非对应效果的 `buff_id`/`remove_buff_id` 必须为 `0` 或留空。
- 空白数值使用编译器默认值，但列本身不能删除或改名。
- ID 在各自表内必须唯一；生成时会检查所有跨表引用。

## 死循环检查

编译器每次运行（包括 `--check-only`）都会检查有没有被动和 Buff reaction 会无休止地互相触发，发现这样的循环就拒绝生成配置包。

伤害会给攻击者触发 `on_hit`，给目标触发 `on_damaged`，击杀时触发 `unit_death`。所以一个 `on_damaged` 时造成伤害的被动，会在它的目标身上再次触发 `on_damaged`；如果那个单位也有这个被动，就会再反击回来，如此往复。如果没有 `max_triggers_per_round`，就没有东西能停下它，直到引擎在触发深度上限处截断，或者在 `max_events` 处结束战斗（结束原因 `event_limit`）。要是伤害是群体的，每一环都会再触发好几环，一次行动就可能变成几十万个事件。

检查分两部分：

1. **循环检查**：如果一个被动或 reaction 的伤害能触发另一个的触发点，就在它们之间连一条线。检查假定任何单位都可能带任何被动，所以不同英雄之间的循环也能查出来。
   - `error`：循环里没有任何一个设置了 `max_triggers_per_round`。不会生成配置包。信息会列出这个循环，并要求至少给其中一个被动或 reaction 加上限。
   - `note`：有上限、会自然停止的循环。列出相关被动，以及每个单位每回合最多触发多少次。
   - 这些不会形成循环：`unit_death`（每个单位只会死一次）、响应触发点、只由战斗流程触发的触发点（`battle_start`、`round_start`、`before_action`、`on_attack`、`after_action`、`round_end`）、治疗和添加/移除 Buff（它们不触发任何东西），以及 `chance_bp` 为 0 的被动。
2. **压力战斗**：打 3 场 7v7，每个单位都带上全部被动（超过 128 个时按 128 个一组分给各单位，这是单个单位的上限）和前 128 个技能，没有人会死，打 5 回合，最多 200,000 个事件。报告最大的一步（一个单位的一次行动，或者行动之外的一串触发）以及这一步里触发最多的被动。
   - `error`：战斗耗尽了事件上限，并且循环检查发现了循环。
   - `warning`：所有循环都有上限，战斗却仍然耗尽了事件上限；或者单独一步超过 5,000 个事件。说明上限可能设得太高。
   - `note`：战斗规模和最大的一步。

`--stress-rounds N`、`--stress-units N` 可以调整压力战斗，`--no-stress` 跳过压力战斗；循环检查总会运行。

例如，一个没有上限的反击被动，加上一个反弹伤害的荆棘 Buff：

```text
error: runaway loop: nothing in it has a max_triggers_per_round, so the triggers keep setting each other off until the engine cuts the cascade at its trigger depth limit, or ends the battle at max_events:
    passive 801 "反击" (on_damaged, no limit) deals damage, which sets off on_damaged
    -> passive 801 "反击" (on_damaged, no limit) again
  All of these can set one another off: passive 801 "反击", buff 901 "荆棘" reaction 1.
  Give at least one of them a max_triggers_per_round.
error: stress battle (7v7, every unit with all 3 passives and 1 skill, nobody dies, 5 rounds) ran out of events: it reached max_events (200,000) in round 1. Largest step: 199,971 events in round 1, during unit 2007's action; most set off: passive 801 x49,996, buff 901 reactions x49,986, passive 803 x2.
config error: the cascade check found runaway loops; no pack was written
```

意思是：被动 801“反击”在受击时造成伤害，又触发受击，会一直反击下去；它和荆棘 Buff 的反弹可以互相触发。压力战斗里一次行动就产生了 199,971 个事件。给被动 801 和荆棘的 reaction 都设上 `max_triggers_per_round`（例如 1）就解决了。[`tests/fixtures/config_runaway_loop`](../tests/fixtures/config_runaway_loop) 里的表可以复现上面的输出。

## 生成配置包

校验但不生成：

```powershell
.\scripts\build-config-compiler.ps1 -Configuration Release

.\erlang\bin\gamebattle_config_compiler.exe `
  --input-dir config/example `
  --check-only
```

生成二进制配置包：

```powershell
.\erlang\bin\gamebattle_config_compiler.exe `
  --input-dir config/example `
  --output build/config/battle.gbcfg
```

`.gbcfg` 是确定性的 little-endian 二进制包，包含魔数 `GBCF`、主/次版本、负载长度和 CRC32。当前格式版本是 `2.0`。
负载先写入 Buff、modifier、reaction、effect、skill、passive 六个记录数，再依次写入六类记录；modifier 和 reaction 记录都携带所属 `buff_id` 与 `sequence`。
同一份 CSV 会生成完全一致的文件，便于发布系统比较哈希和安全回滚。
配置编译器自身使用 C++20，与战斗引擎由同一个 CMake 工程维护，但通过独立目标和构建目录编译，不需要 Python 或 Excel 运行库。它链接了战斗核心，用来跑压力战斗。
它默认关闭；普通战斗核心构建不会编译配置工具。
