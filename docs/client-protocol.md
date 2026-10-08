# 客户端协议（protobuf）

**中文** | [English](client-protocol.en.md)

游戏客户端与战斗服务之间使用 protobuf。协议定义在 [`proto/battle_client.proto`](../proto/battle_client.proto)，Erlang 侧由 [`erlang/src/gamebattle_client.erl`](../erlang/src/gamebattle_client.erl) 负责转换。Erlang 与 C++ 之间仍然使用原来的 ETF，这一层没有任何变化。

```text
客户端（Unity/C#、Cocos/TS……）
   │  protobuf：ClientMessage / ServerMessage
   ▼
Erlang 网关或玩法进程 ── gamebattle_client:decode_client_message/1
   │  用服务器上的玩家数据组装战斗请求（客户端只提供阵容）
   ▼
gamebattle:simulate/2 ── ETF {packet,4} ──> C++ gamebattle_port
   │  结果 map
   ▼
gamebattle_client:encode_battle_report/2 ──> 客户端
```

## 帧格式

- **TCP**：每帧为 4 字节大端长度加一条消息，与 Erlang `gen_tcp` 的 `{packet, 4}` 完全一致。
- **WebSocket**：一个二进制帧放一条消息，不再加长度前缀。
- 客户端发往服务器的帧是 `ClientMessage`，服务器发往客户端的帧是 `ServerMessage`。

## 消息

| 方向 | 消息 | 用途 |
|---|---|---|
| 客户端 → 服务器 | `ClientMessage.start_battle` | 选择关卡并提交阵容：`stage_id` 与 `lineup`（`unit_id` + `position`）。`summary_only` 为真时回复不带事件日志，适合扫荡、自动战斗这类没人观看的战斗。 |
| 服务器 → 客户端 | `ServerMessage.battle_report` | 一场战斗的完整战报：胜负、结束原因、各单位最终状态、按顺序排列的事件。 |
| 服务器 → 客户端 | `ServerMessage.gauntlet_report` | 车轮战汇总，`waves` 中每一波都是一份 `BattleReport`。 |
| 服务器 → 客户端 | `ServerMessage.error` | 错误码与给玩家看的文字。 |

`request_id` 由客户端选择，服务器在回复中原样带回；服务器主动推送的消息填 0。

战报字段与引擎结果同名，含义见 README 的[结果与战报](../README.md#结果与战报)。引擎的名字按固定规则转成枚举：`damage` → `EVENT_TYPE_DAMAGE`，`first_side` → `PHASE_FIRST_SIDE`，`max_rounds` → `END_REASON_MAX_ROUNDS`。

| 错误码 | 含义 | 客户端处理 |
|---|---|---|
| `ERROR_CODE_BAD_MESSAGE` | 帧无法解码，或没有通过校验 | 视为客户端 bug，记录日志 |
| `ERROR_CODE_INVALID_REQUEST` | 格式正确，但请求不被允许 | 提示玩家 |
| `ERROR_CODE_RETRY_LATER` | 服务器暂时出错（Port 超时、重启） | 稍后用同样的请求重试 |
| `ERROR_CODE_INTERNAL` | 其他服务器错误 | 提示玩家，不要自动重试 |

## 服务器（Erlang）

```erlang
%% 每个连接一个进程。帧长度上限在 socket 上设置，protobuf 本身不限制大小：
%%   inet:setopts(Socket, [binary, {packet, 4}, {packet_size, 64 * 1024}])
handle_frame(Socket, Frame) ->
    Reply =
        case gamebattle_client:decode_client_message(Frame) of
            {ok, RequestId, {start_battle, #{stage_id := StageId, lineup := Lineup}}} ->
                %% 你的玩法代码：检查关卡是否解锁、单位是否属于该玩家，
                %% 再从服务器数据取出属性、技能，组装 gamebattle 请求。
                Request = build_request(StageId, Lineup),
                case gamebattle:simulate(port, Request) of
                    {ok, Result} ->
                        gamebattle_client:encode_battle_report(RequestId, Result);
                    {error, Reason} ->
                        logger:warning("battle failed: ~p", [Reason]),
                        gamebattle_client:encode_error(
                          RequestId, gamebattle_client:error_code(Reason),
                          <<"战斗失败，请稍后再试"/utf8>>)
                end;
            {error, RequestId, bad_message} ->
                gamebattle_client:encode_error(RequestId, bad_message, <<"bad request">>)
        end,
    gen_tcp:send(Socket, Reply).
```

`gamebattle_client` 提供：

| 函数 | 作用 |
|---|---|
| `decode_client_message/1` | 解码并校验 `ClientMessage`：阵容非空、不超过 256 个、`unit_id` 大于 0 且不重复、`position` 在 0..1000。任何错误都返回 `{error, RequestId, bad_message}`，不会抛异常。 |
| `encode_battle_report/2` | `gamebattle:simulate/1,2` 的结果 → `ServerMessage` 二进制。 |
| `encode_gauntlet_report/2` | `gamebattle:run_gauntlet/3,4` 的结果 → `ServerMessage` 二进制。`carryover` 只留在服务器。 |
| `encode_error/3` | 错误码 + 给玩家看的文字 → `ServerMessage` 二进制。 |
| `error_code/1` | 把 `gamebattle` 返回的 `{error, Map}` 映射为建议的错误码。 |
| `battle_report/1`、`gauntlet_report/1` | 只转换为消息 map，不编码；需要自己组装 `ServerMessage` 时使用。 |

编码前会校验每个字段（gpb 的 `{verify, always}`），值超出范围会直接抛错，而不是发出错误的字节。

## 客户端（Unity / C#）

1. 安装 `protoc`，引入 NuGet 包 `Google.Protobuf`，两者用同一个大版本。
2. 生成代码，放进 Unity 工程：

   ```bash
   protoc -I proto --csharp_out=Assets/Scripts/Proto proto/battle_client.proto
   ```

3. 收发消息（TCP，4 字节大端长度前缀）：

```csharp
using System.IO;
using Google.Protobuf;
using GameBattle.Client.V1;

static class BattleWire
{
    const int MaxFrame = 4 * 1024 * 1024;

    public static void Write(Stream s, IMessage msg)
    {
        byte[] body = msg.ToByteArray();
        int n = body.Length;
        s.Write(new[] { (byte)(n >> 24), (byte)(n >> 16), (byte)(n >> 8), (byte)n }, 0, 4);
        s.Write(body, 0, n);
    }

    public static ServerMessage Read(Stream s)
    {
        byte[] h = ReadExactly(s, 4);
        int n = (h[0] << 24) | (h[1] << 16) | (h[2] << 8) | h[3];
        if (n < 0 || n > MaxFrame) throw new InvalidDataException("frame too large");
        return ServerMessage.Parser.ParseFrom(ReadExactly(s, n));
    }

    static byte[] ReadExactly(Stream s, int n)
    {
        var buf = new byte[n];
        for (int off = 0; off < n;)
        {
            int got = s.Read(buf, off, n - off);
            if (got == 0) throw new EndOfStreamException();
            off += got;
        }
        return buf;
    }
}

// 发起战斗
var req = new ClientMessage { RequestId = 1, StartBattle = new StartBattleReq { StageId = 12 } };
req.StartBattle.Lineup.Add(new LineupSlot { UnitId = 1001, Position = 1 });
BattleWire.Write(stream, req);

// 播放战报
ServerMessage reply = BattleWire.Read(stream);
switch (reply.BodyCase)
{
    case ServerMessage.BodyOneofCase.BattleReport:
        foreach (BattleEvent e in reply.BattleReport.Events)
        {
            switch (e.Type)
            {
                case EventType.Damage: /* e.Actor 打 e.Target，e.Value 点伤害 */ break;
                case EventType.Chain:  /* 连锁第 e.Value 环 */ break;
                default: break;  // 不认识的类型直接跳过
            }
        }
        break;
    case ServerMessage.BodyOneofCase.Error:
        if (reply.Error.Code == ErrorCode.RetryLater) { /* 稍后重试 */ }
        break;
}
```

C# 生成代码会去掉枚举值的类型前缀：`EVENT_TYPE_DAMAGE` 在 C# 里是 `EventType.Damage`。走 WebSocket 时，直接用 `ServerMessage.Parser.ParseFrom(frameBytes)` 解析每个二进制帧，不需要长度前缀。

其他语言用各自的生成器即可：TypeScript 可用 `ts-proto` 或 `protobufjs`，Go 用 `protoc-gen-go`，C++ 用 `--cpp_out`。

完整的可运行示例：服务端是测试网关 [`erlang/src/gamebattle_gateway.erl`](../erlang/src/gamebattle_gateway.erl)，客户端是 [`client/battle_client.py`](../client/battle_client.py)，用法见 [client/README.md](../client/README.md)。

## 安全

- 客户端只提交玩家的选择（关卡、阵容）。HP、攻击、技能等数值一律来自服务器数据，不要相信客户端发来的数字。
- 在 socket 上限制帧长度（`{packet_size, N}`）；超过上限的连接会收到 `emsgsize` 错误，直接断开即可。
- 不要对客户端数据调用 `binary_to_term/1`：ETF 可以创建原子、耗尽原子表。protobuf 解码不会创建原子。
- `encode_error/3` 的文字会原样显示给玩家。引擎的错误详情只写服务器日志。

## 兼容规则

- 只新增字段和枚举值。不要修改已有字段的编号或类型，也不要复用编号；删除的字段用 `reserved` 占住编号。
- 旧客户端收到不认识的枚举值时，它会作为整数保留下来；客户端应跳过不认识的事件，而不是报错。
- 包名带版本（`gamebattle.client.v1`）。只有在必须做不兼容修改时才新建 `v2`。

### 引擎新增事件类型时

1. 照常修改 C++ 引擎与 ETF 协议。
2. 在 `proto/battle_client.proto` 的 `EventType` 末尾追加 `EVENT_TYPE_XXX = <下一个编号>;`。
3. 在 `gamebattle_client:event_type/1` 中追加一行映射。
4. 在 `erlang/test/gamebattle_client_tests.erl` 的 `ENGINE_EVENT_TYPES` 里加上新名字，运行 `rebar3 eunit`。
5. 重新生成客户端代码，再发布客户端。

漏掉第 3 步时，新事件会以 `EVENT_TYPE_UNSPECIFIED` 发给客户端，第 4 步的测试会发现它。阶段（`Phase`）和结束原因（`EndReason`）同理。

### 新增请求类型时

在 `ClientMessage.body` 的 `oneof` 里追加一个字段（使用新的编号，例如 11），再在 `decode_client_message/1` 里增加对应的分支和校验。

## 构建与测试

- `rebar3 compile` 会先由 `rebar3_gpb_plugin` 根据 `.proto` 生成 `erlang/src/battle_client_pb.erl`，再编译全部模块。生成的文件不提交（已在 `.gitignore` 中），`rebar3 clean` 会删除它。
- 第一次构建需要访问 hex.pm，下载 `rebar3_gpb_plugin` 和 `gpb`。生成的模块运行时不依赖 gpb，发布包里不需要它。
- 编译生成的模块时会出现 9 条 `missing specification` 警告（`decode_msg/2` 等）。这是预期的：gpb 不给这几个函数写 spec，而 `rebar.config` 的 `warn_missing_spec` 优先级高于模块内的 `-compile` 属性，无法只对这个文件关闭。
- `rebar3 eunit` 运行协议测试。设置 `GAMEBATTLE_PORT` 指向已构建的 `gamebattle_port`，还会跑一遍经过真实 C++ Port 的端到端测试：

  ```bash
  cd erlang
  GAMEBATTLE_PORT=../out/build/linux-runtime-debug/gamebattle_port rebar3 eunit
  ```

## 为什么选 protobuf

用 `gamebattle:example_request()` 跑出的一场战斗（19 回合、8 个单位、226 个事件）：

| 编码 | 原始大小 | gzip 后 |
|---|---:|---:|
| protobuf（本协议） | 4,428 B | 1,716 B |
| ETF（`term_to_binary`） | 40,913 B | 2,452 B |
| JSON（省略零值字段） | 31,738 B | 2,526 B |

此外，protobuf 有强类型 schema、各语言都有官方或成熟的生成器，并且按上面的兼容规则演进时，新旧客户端可以共存。
