# Client protocol (protobuf)

[中文](client-protocol.md) | **English**

Game clients talk to the battle service in protobuf. The schema is [`proto/battle_client.proto`](../proto/battle_client.proto); on the Erlang side [`erlang/src/gamebattle_client.erl`](../erlang/src/gamebattle_client.erl) does the conversion. Erlang and C++ still talk ETF to each other. The battle report itself is written by the engine: a request with the `report` option gets a compact result whose `report` field already holds the encoded `BattleReport` (see [Compact results](../README.en.md#compact-results)).

```text
client (Unity/C#, Cocos/TS, ...)
   │  protobuf: ClientMessage / ServerMessage
   ▼
Erlang gateway or game process ── gamebattle_client:decode_client_message/1
   │  builds the battle request from server-side player data (the client only sends a lineup and a detail level)
   ▼
gamebattle:simulate/2 ── ETF ──> C++ engine (NIF or Port), which also writes the BattleReport bytes
   │  small result map: winner, units, report bytes
   ▼
gamebattle_client:encode_battle_report/2 ──> client
```

## Framing

- **TCP**: every frame is a 4-byte big-endian length followed by one message, exactly what Erlang `gen_tcp`'s `{packet, 4}` does.
- **WebSocket**: one message per binary frame, with no length prefix.
- Client → server frames carry a `ClientMessage`; server → client frames carry a `ServerMessage`.

## Messages

| Direction | Message | Purpose |
|---|---|---|
| client → server | `ClientMessage.start_battle` | Choose a stage and submit a lineup: `stage_id` and `lineup` (`unit_id` + `position`), plus `detail`, how much of the battle the reply should describe (see below). |
| server → client | `ServerMessage.battle_report` | A whole battle: winner, end reason, every unit's final state, and the battle step by step (`actions`) or event by event (`events`). |
| server → client | `ServerMessage.gauntlet_report` | A gauntlet summary; each entry of `waves` is a `BattleReport`. |
| server → client | `ServerMessage.error` | An error code and text for the player. |

The client picks `request_id` and the server echoes it in the reply; messages the server pushes unprompted carry 0.

Report fields have the same names as in the engine result; see [Results and battle reports](../README.en.md#results-and-battle-reports) in the README for their meaning. Engine names map to enum values by a fixed rule: `damage` → `EVENT_TYPE_DAMAGE`, `first_side` → `PHASE_FIRST_SIDE`, `max_rounds` → `END_REASON_MAX_ROUNDS`.

## How much of the battle a report carries

`StartBattleReq.detail` picks one of three levels; every level has the winner, the end reason and each unit's final state:

| `detail` | The report also carries | Example battle | 7v7, 40 passives each, 48,000 events | For |
|---|---|---:|---:|---|
| `REPORT_DETAIL_SUMMARY` | nothing more | 139 B | 407 B | Sweeps, auto-battles: battles nobody watches |
| `REPORT_DETAIL_ACTIONS` (also the default) | `actions`: one `BattleAction` per step | 1.8 KB | 240 KB | Playing the battle back |
| `REPORT_DETAIL_EVENTS` | `events`: every `BattleEvent` | 4.4 KB | 1.5 MB | Debugging and replay tools |

A `BattleAction` is one step of the battle with its events added up:

- A step is either one unit's action, from `ACTION_START` to `ACTION_END` (`actor`, `side` and `skill_id` are set; `skill_id` 0 is a basic attack), or a run of triggers outside any action, such as battle start, round start, round end or before-action passives (`actor` 0).
- `units` has a `UnitChange` for each unit the step affected, in the order they were first affected: `damage` and `heal` received, `hits`, `crits`, `misses`, `hp` after its last hit or heal, and `died`.
- `effects` counts everything else, one `EffectCount` per event type, unit and `source_id`: passives (`EVENT_TYPE_PASSIVE`, the unit that triggered), buffs added, removed or expired (the unit carrying the buff), buff reactions, chains, negates and fizzles. `value` is the value of the last such event, for example a buff's stack count.
- `event_count` says how many `BattleEvent`s the step stands for.

To play a step: show the actor's skill, then each `UnitChange` (a damage number, a heal number, the HP bar moving to `hp`, a death), then the effects (passive icons with a count). A chain of a thousand passive hits on one unit becomes one `UnitChange` with `hits` = 1000, which is what makes long passive chains cheap to send.

A server that doesn't know the detail level asked for (a newer client) treats it as `REPORT_DETAIL_ACTIONS`.

## Errors

| Error code | Meaning | What the client does |
|---|---|---|
| `ERROR_CODE_BAD_MESSAGE` | The frame could not be decoded or failed validation | Treat it as a client bug and log it |
| `ERROR_CODE_INVALID_REQUEST` | Well formed, but not allowed | Tell the player |
| `ERROR_CODE_RETRY_LATER` | A temporary server problem (Port timeout or restart) | Retry the same request later |
| `ERROR_CODE_INTERNAL` | Any other server error | Tell the player; don't retry automatically |

## Server (Erlang)

```erlang
%% One process per connection. Bound the frame size on the socket, since
%% protobuf itself has no size limit:
%%   inet:setopts(Socket, [binary, {packet, 4}, {packet_size, 64 * 1024}])
handle_frame(Socket, Frame) ->
    Reply =
        case gamebattle_client:decode_client_message(Frame) of
            {ok, RequestId, {start_battle, #{stage_id := StageId, lineup := Lineup,
                                             detail := Detail}}} ->
                %% Your game code: check that the stage is unlocked and the units
                %% belong to this player, then build the gamebattle request from
                %% server-side stats and skills. `report` makes the engine write
                %% the BattleReport (summary | actions | events) itself.
                Request = (build_request(StageId, Lineup))#{report => Detail},
                case gamebattle:simulate(port, Request) of
                    {ok, Result} ->
                        gamebattle_client:encode_battle_report(RequestId, Result);
                    {error, Reason} ->
                        logger:warning("battle failed: ~p", [Reason]),
                        gamebattle_client:encode_error(
                          RequestId, gamebattle_client:error_code(Reason),
                          <<"Battle failed, please try again later">>)
                end;
            {error, RequestId, bad_message} ->
                gamebattle_client:encode_error(RequestId, bad_message, <<"bad request">>)
        end,
    gen_tcp:send(Socket, Reply).
```

`gamebattle_client` provides:

| Function | What it does |
|---|---|
| `decode_client_message/1` | Decodes and validates a `ClientMessage`: a non-empty lineup of at most 256 slots, `unit_id`s above 0 and unique, `position` within 0..1000. `detail` comes back as `summary`, `actions` or `events`, the values of the `report` request option. Every failure returns `{error, RequestId, bad_message}`; it never raises. |
| `encode_battle_report/2` | A `gamebattle:simulate/1,2` result → `ServerMessage` bytes. A compact result's `report` bytes are sent as they are; a full result (no `report` option) is encoded with all its events. |
| `encode_gauntlet_report/2` | A `gamebattle:run_gauntlet/3,4` result → `ServerMessage` bytes. The `carryover` stays on the server. |
| `encode_error/3` | An error code plus text for the player → `ServerMessage` bytes. |
| `error_code/1` | Maps an `{error, Map}` returned by `gamebattle` to a suggested error code. |
| `battle_report/1`, `gauntlet_report/1` | Convert to message maps without encoding, for when you build the `ServerMessage` yourself. |

Every field gpb encodes is checked first (gpb's `{verify, always}`): an out-of-range value raises instead of producing corrupt bytes. The engine's report bytes don't go through gpb; the tests check that they decode and re-encode to the same bytes.

## Client (Unity / C#)

1. Install `protoc` and add the NuGet package `Google.Protobuf`, both of the same major version.
2. Generate the code into the Unity project:

   ```bash
   protoc -I proto --csharp_out=Assets/Scripts/Proto proto/battle_client.proto
   ```

3. Send and receive (TCP, 4-byte big-endian length prefix):

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

// Start a battle
var req = new ClientMessage { RequestId = 1, StartBattle = new StartBattleReq { StageId = 12 } };
req.StartBattle.Lineup.Add(new LineupSlot { UnitId = 1001, Position = 1 });
BattleWire.Write(stream, req);

// Play the report (detail ACTIONS, the default)
ServerMessage reply = BattleWire.Read(stream);
switch (reply.BodyCase)
{
    case ServerMessage.BodyOneofCase.BattleReport:
        foreach (BattleAction step in reply.BattleReport.Actions)
        {
            if (step.Actor != 0) { /* step.Actor uses skill step.SkillId (0: basic attack) */ }
            foreach (UnitChange u in step.Units)
            {
                /* u.Unit takes u.Damage in u.Hits hits (u.Crits crits), heals u.Heal,
                   HP bar to u.Hp; u.Died */
            }
            foreach (EffectCount fx in step.Effects)
            {
                switch (fx.Type)
                {
                    case EventType.Passive: /* passive fx.SourceId of fx.Unit, fx.Count times */ break;
                    case EventType.BuffAdd: /* buff fx.SourceId on fx.Unit, stacks fx.Value */ break;
                    default: break;  // skip types you don't know
                }
            }
        }
        break;
    case ServerMessage.BodyOneofCase.Error:
        if (reply.Error.Code == ErrorCode.RetryLater) { /* retry later */ }
        break;
}
```

The C# generator strips the type prefix from enum values: `EVENT_TYPE_DAMAGE` is `EventType.Damage` in C#. Over WebSocket, parse each binary frame with `ServerMessage.Parser.ParseFrom(frameBytes)`; there is no length prefix.

Other languages use their own generators: `ts-proto` or `protobufjs` for TypeScript, `protoc-gen-go` for Go, `--cpp_out` for C++.

A complete working example: the test gateway [`erlang/src/gamebattle_gateway.erl`](../erlang/src/gamebattle_gateway.erl) on the server side and [`client/battle_client.py`](../client/battle_client.py) as the client; see [client/README.en.md](../client/README.en.md).

## Security

- The client sends only the player's choices (stage, lineup). HP, attack, skills and every other number come from server data: never trust numbers from a client.
- Bound the frame size on the socket (`{packet_size, N}`); a connection that exceeds it gets an `emsgsize` error, so just close it.
- Never call `binary_to_term/1` on client data: ETF can create atoms and exhaust the atom table. Protobuf decoding creates no atoms.
- The text given to `encode_error/3` is shown to players as is. Engine error details belong in server logs only.

## Compatibility rules

- Only add fields and enum values. Never change an existing field's number or type, and never reuse a number; mark removed fields `reserved`.
- An old client keeps an enum value it doesn't know as a plain integer; clients should skip events they don't know rather than fail.
- The package name carries a version (`gamebattle.client.v1`). Create `v2` only when an incompatible change is unavoidable.

### When the engine gains an event type

1. Change the C++ engine and the ETF protocol as usual.
2. Append `EVENT_TYPE_XXX = <next number>;` to `EventType` in `proto/battle_client.proto`.
3. Map the engine name to it in three places: `gamebattle_client:event_type/1`, `gamebattle_report:event_type/1` and `kEventTypes` in `src/report.cpp` (in enum order). If the new event changes a unit's HP, also decide how `BattleAction` adds it up, in `src/report.cpp` and `gamebattle_report.erl` alike.
4. Add the new name to `ENGINE_EVENT_TYPES` in `erlang/test/gamebattle_client_tests.erl` and run `rebar3 eunit`.
5. Regenerate the client code and ship the client.

Skip step 3 and the new event reaches clients as `EVENT_TYPE_UNSPECIFIED`; the test from step 4 catches the `gamebattle_client` table, and the differential tests in `gamebattle_erl_tests` catch the other two disagreeing. Phases (`Phase`) and end reasons (`EndReason`) work the same way.

### When adding a request type

Add a field to the `oneof body` of `ClientMessage` (with a new number, such as 11), then add a matching clause and validation to `decode_client_message/1`.

## Building and testing

- `rebar3 compile` first has `rebar3_gpb_plugin` generate `erlang/src/battle_client_pb.erl` from the `.proto`, then compiles every module. The generated file is not committed (it is in `.gitignore`), and `rebar3 clean` deletes it.
- The first build needs hex.pm to download `rebar3_gpb_plugin` and `gpb`. The generated module does not depend on gpb at run time, so releases don't need it.
- Compiling the generated module prints 9 `missing specification` warnings (`decode_msg/2` and others). They are expected: gpb writes no specs for those functions, and `warn_missing_spec` from `rebar.config` overrides a module's own `-compile` attribute, so it cannot be turned off for just that file.
- `rebar3 eunit` runs the protocol tests. Point `GAMEBATTLE_PORT` at a built `gamebattle_port` and it also runs an end-to-end test through the real C++ Port:

  ```bash
  cd erlang
  GAMEBATTLE_PORT=../out/build/linux-runtime-debug/gamebattle_port rebar3 eunit
  ```

## Why protobuf

One battle from `gamebattle:example_request()` (19 rounds, 8 units, 226 events):

| Encoding | Raw size | After gzip |
|---|---:|---:|
| protobuf, `REPORT_DETAIL_ACTIONS` | 1,768 B | 737 B |
| protobuf, `REPORT_DETAIL_EVENTS` | 4,428 B | 1,716 B |
| ETF (`term_to_binary`) | 40,913 B | 2,452 B |
| JSON (zero-valued fields omitted) | 31,738 B | 2,526 B |

protobuf also has a typed schema and official or mature generators for every client language, and, when it evolves by the compatibility rules above, old and new clients keep working side by side.
