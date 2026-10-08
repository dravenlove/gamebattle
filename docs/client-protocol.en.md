# Client protocol (protobuf)

[中文](client-protocol.md) | **English**

Game clients talk to the battle service in protobuf. The schema is [`proto/battle_client.proto`](../proto/battle_client.proto); on the Erlang side [`erlang/src/gamebattle_client.erl`](../erlang/src/gamebattle_client.erl) does the conversion. Erlang and C++ still talk ETF to each other; nothing changed there.

```text
client (Unity/C#, Cocos/TS, ...)
   │  protobuf: ClientMessage / ServerMessage
   ▼
Erlang gateway or game process ── gamebattle_client:decode_client_message/1
   │  builds the battle request from server-side player data (the client only sends a lineup)
   ▼
gamebattle:simulate/2 ── ETF {packet,4} ──> C++ gamebattle_port
   │  result map
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
| client → server | `ClientMessage.start_battle` | Choose a stage and submit a lineup: `stage_id` and `lineup` (`unit_id` + `position`). |
| server → client | `ServerMessage.battle_report` | A whole battle: winner, end reason, every unit's final state and the ordered events. |
| server → client | `ServerMessage.gauntlet_report` | A gauntlet summary; each entry of `waves` is a `BattleReport`. |
| server → client | `ServerMessage.error` | An error code and text for the player. |

The client picks `request_id` and the server echoes it in the reply; messages the server pushes unprompted carry 0.

Report fields have the same names as in the engine result; see [Results and battle reports](../README.en.md#results-and-battle-reports) in the README for their meaning. Engine names map to enum values by a fixed rule: `damage` → `EVENT_TYPE_DAMAGE`, `first_side` → `PHASE_FIRST_SIDE`, `max_rounds` → `END_REASON_MAX_ROUNDS`.

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
            {ok, RequestId, {start_battle, #{stage_id := StageId, lineup := Lineup}}} ->
                %% Your game code: check that the stage is unlocked and the units
                %% belong to this player, then build the gamebattle request from
                %% server-side stats and skills.
                Request = build_request(StageId, Lineup),
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
| `decode_client_message/1` | Decodes and validates a `ClientMessage`: a non-empty lineup of at most 256 slots, `unit_id`s above 0 and unique, `position` within 0..1000. Every failure returns `{error, RequestId, bad_message}`; it never raises. |
| `encode_battle_report/2` | A `gamebattle:simulate/1,2` result → `ServerMessage` bytes. |
| `encode_gauntlet_report/2` | A `gamebattle:run_gauntlet/3,4` result → `ServerMessage` bytes. The `carryover` stays on the server. |
| `encode_error/3` | An error code plus text for the player → `ServerMessage` bytes. |
| `error_code/1` | Maps an `{error, Map}` returned by `gamebattle` to a suggested error code. |
| `battle_report/1`, `gauntlet_report/1` | Convert to message maps without encoding, for when you build the `ServerMessage` yourself. |

Every field is checked before encoding (gpb's `{verify, always}`): an out-of-range value raises instead of producing corrupt bytes.

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

// Play the report
ServerMessage reply = BattleWire.Read(stream);
switch (reply.BodyCase)
{
    case ServerMessage.BodyOneofCase.BattleReport:
        foreach (BattleEvent e in reply.BattleReport.Events)
        {
            switch (e.Type)
            {
                case EventType.Damage: /* e.Actor hits e.Target for e.Value */ break;
                case EventType.Chain:  /* chain link number e.Value */ break;
                default: break;  // skip types you don't know
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
3. Add a mapping clause to `gamebattle_client:event_type/1`.
4. Add the new name to `ENGINE_EVENT_TYPES` in `erlang/test/gamebattle_client_tests.erl` and run `rebar3 eunit`.
5. Regenerate the client code and ship the client.

Skip step 3 and the new event reaches clients as `EVENT_TYPE_UNSPECIFIED`; the test from step 4 catches that. Phases (`Phase`) and end reasons (`EndReason`) work the same way.

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
| protobuf (this protocol) | 4,428 B | 1,716 B |
| ETF (`term_to_binary`) | 40,913 B | 2,452 B |
| JSON (zero-valued fields omitted) | 31,738 B | 2,526 B |

protobuf also has a typed schema and official or mature generators for every client language, and, when it evolves by the compatibility rules above, old and new clients keep working side by side.
