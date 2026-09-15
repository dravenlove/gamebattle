# 战斗配置表

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
- `stack_key` 支持 `by_buff`、`by_buff_and_source`；后者让不同施加者拥有互相独立的 Buff 实例和层数。
- `stack_policy` 支持 `stack`、`refresh`；`refresh` 的 `max_stacks` 必须为 `1`。
- `refresh_policy` 支持 `reset`、`extend`、`keep`。
- modifier 的 `attribute` 支持 `attack`、`defense`、`speed`、`crit_rate_bp`、`crit_damage_bp`、`hit_rate_bp`、`dodge_rate_bp`、`damage_bonus_bp`、`damage_reduction_bp`。
- modifier 的 `operation` 支持 `add` 和 `scale_bp`；modifier 会按 Buff 当前层数应用。
- `buff_modifiers.csv` 和 `buff_reactions.csv` 使用 `(buff_id, sequence)` 作为组合唯一键，并按 `sequence` 执行。
- reaction 的 `source` 支持 `owner`、`applier`。目标选择始终以 Buff 持有者为上下文；`source` 只决定效果属性和事件来源。
- reaction 的 `stack_scaling` 支持 `once`、`per_stack`；后者按当前 Buff 层数放大伤害、治疗或直接伤害，添加/移除 Buff 不会重复执行。
- reaction 和 passive 的 `priority` 必须在 `-1000000` 到 `1000000` 之间，数值越大越先触发。同优先级先执行 passive，再执行 Buff reaction，之后使用定义 ID、Buff 实例 ID 和配置 `sequence` 保证确定性顺序。
- reaction 引用 `add_buff` 效果时不能形成有向环，否则共享配置定义会产生所有权环，编译器和加载器都会拒绝。
- `direct_damage` 是不经过命中、暴击、防御、增伤和减伤公式的直接伤害；持续伤害可以通过 reaction 引用它。
- `add_buff` 效果必须填写 `buff_id`。
- `remove_buff` 效果必须填写 `remove_buff_id`。
- 非对应效果的 `buff_id`/`remove_buff_id` 必须为 `0` 或留空。
- 空白数值使用编译器默认值，但列本身不能删除或改名。
- ID 在各自表内必须唯一；生成时会检查所有跨表引用。

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

`.gbcfg` 是确定性的 little-endian 二进制包，包含魔数 `GBCF`、主/次版本、负载长度和 CRC32。当前格式版本是 `3.0`，与旧 `2.0` 不兼容。
负载先写入 Buff、modifier、reaction、effect、skill、passive 六个记录数，再依次写入六类记录；modifier 和 reaction 记录都携带所属 `buff_id` 与 `sequence`。3.0 的 Buff 记录增加 `stack_key`，reaction 和 passive 记录增加 `priority`。
同一份 CSV 会生成完全一致的文件，便于发布系统比较哈希和安全回滚。
配置编译器自身使用 C++20，与战斗引擎由同一个 CMake 工程维护，但通过独立目标和构建目录编译，不需要 Python 或 Excel 运行库。
它默认关闭；普通战斗核心构建不会编译配置工具。
