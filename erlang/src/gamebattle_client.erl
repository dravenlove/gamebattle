-module(gamebattle_client).

%% Game-client protocol adapter. The schema is proto/battle_client.proto;
%% battle_client_pb is generated from it by rebar3_gpb_plugin.
%%
%% Engine results stay Erlang maps inside the server. This module turns them
%% into the protobuf messages clients receive, and decodes and validates what
%% clients send. It never forwards engine error text to clients.

-export([
    encode_battle_report/2,
    encode_gauntlet_report/2,
    encode_error/3,
    error_code/1,
    decode_client_message/1,
    battle_report/1,
    gauntlet_report/1
]).

-export_type([error_code/0, client_request/0]).

%% The engine's bounds for one formation (validate_request in battle_state.cpp).
-define(MAX_LINEUP, 256).
-define(MAX_POSITION, 1000).

-type error_code() :: bad_message | invalid_request | retry_later | internal.
-type lineup_slot() :: #{unit_id := pos_integer(), position := 0..?MAX_POSITION}.
-type client_request() ::
    {start_battle, #{stage_id := non_neg_integer(), lineup := [lineup_slot(), ...]}}.

%%% Server -> client ---------------------------------------------------------

%% RequestId is the ClientMessage.request_id being answered, or 0 for a push.
-spec encode_battle_report(non_neg_integer(), map()) -> binary().
encode_battle_report(RequestId, Result) ->
    encode_server(RequestId, {battle_report, battle_report(Result)}).

-spec encode_gauntlet_report(non_neg_integer(), map()) -> binary().
encode_gauntlet_report(RequestId, Gauntlet) ->
    encode_server(RequestId, {gauntlet_report, gauntlet_report(Gauntlet)}).

%% Message is shown to players as is: pass your own text, not engine errors.
-spec encode_error(non_neg_integer(), error_code(), unicode:chardata()) -> binary().
encode_error(RequestId, Code, Message) ->
    Error = #{code => error_code_enum(Code),
              message => unicode:characters_to_binary(Message)},
    encode_server(RequestId, {error, Error}).

%% A suggested client-facing code for an {error, Map} returned by gamebattle.
-spec error_code(map()) -> error_code().
error_code(#{type := invalid_request}) -> invalid_request;
error_code(#{type := timeout}) -> retry_later;
error_code(#{type := port_exit}) -> retry_later;
error_code(_) -> internal.

%% Engine result map (gamebattle:simulate/1,2) -> BattleReport message map.
-spec battle_report(map()) -> battle_client_pb:'BattleReport'().
battle_report(#{battle_id := BattleId, seed := Seed, winner := Winner,
                reason := Reason, rounds := Rounds,
                attacker_initiative := AttackerInitiative,
                defender_initiative := DefenderInitiative,
                units := Units, events := Events} = Result) ->
    #{battle_id => BattleId,
      seed => Seed,
      source_battle_id => maps:get(source_battle_id, Result, 0),
      winner => winner(Winner),
      reason => end_reason(Reason),
      rounds => Rounds,
      attacker_initiative => AttackerInitiative,
      defender_initiative => DefenderInitiative,
      units => [unit_state(Unit) || Unit <- Units],
      events => [event(Event) || Event <- Events]}.

%% Gauntlet summary (gamebattle:run_gauntlet/3,4) -> GauntletReport message map.
%% The carryover stays on the server.
-spec gauntlet_report(map()) -> battle_client_pb:'GauntletReport'().
gauntlet_report(#{status := Status, winner := Winner,
                  completed_waves := Completed, fought_waves := Fought,
                  total_waves := Total, stopped_at_wave := StoppedAt,
                  wave_results := Waves}) ->
    #{status => gauntlet_status(Status),
      winner => winner(Winner),
      completed_waves => Completed,
      fought_waves => Fought,
      total_waves => Total,
      stopped_at_wave => StoppedAt,
      waves => [battle_report(Wave) || Wave <- Waves]}.

%%% Client -> server ---------------------------------------------------------

%% Bound the frame size at the socket ({packet_size, N} for gen_tcp) before
%% calling this: protobuf itself has no size limit. The request id is returned
%% with errors too (0 if the frame could not be decoded) so the caller can
%% answer with encode_error/3.
-spec decode_client_message(binary()) ->
    {ok, non_neg_integer(), client_request()} | {error, non_neg_integer(), bad_message}.
decode_client_message(Bytes) when is_binary(Bytes) ->
    try battle_client_pb:decode_msg(Bytes, 'ClientMessage') of
        #{request_id := RequestId, body := {start_battle, Request}} ->
            case start_battle(Request) of
                {ok, Decoded} -> {ok, RequestId, Decoded};
                error -> {error, RequestId, bad_message}
            end;
        #{request_id := RequestId} ->
            {error, RequestId, bad_message}
    catch
        error:_ -> {error, 0, bad_message}
    end.

%%% Internal -----------------------------------------------------------------

-spec encode_server(non_neg_integer(), term()) -> binary().
encode_server(RequestId, Body) ->
    battle_client_pb:encode_msg(#{request_id => RequestId, body => Body},
                                'ServerMessage').

-spec start_battle(battle_client_pb:'StartBattleReq'()) -> {ok, client_request()} | error.
start_battle(#{stage_id := StageId, lineup := Slots}) ->
    Lineup = [#{unit_id => UnitId, position => Position}
              || #{unit_id := UnitId, position := Position} <- Slots],
    UnitIds = [UnitId || #{unit_id := UnitId} <- Lineup],
    Valid = Lineup =/= []
        andalso length(Lineup) =< ?MAX_LINEUP
        andalso lists:all(fun valid_slot/1, Lineup)
        andalso length(lists:usort(UnitIds)) =:= length(UnitIds),
    case Valid of
        true -> {ok, {start_battle, #{stage_id => StageId, lineup => Lineup}}};
        false -> error
    end.

-spec valid_slot(map()) -> boolean().
valid_slot(#{unit_id := UnitId, position := Position}) ->
    UnitId > 0 andalso Position >= 0 andalso Position =< ?MAX_POSITION.

-spec unit_state(map()) -> battle_client_pb:'UnitState'().
unit_state(#{id := Id, side := Side, initial_hp := InitialHp, hp := Hp,
             max_hp := MaxHp, alive := Alive}) ->
    #{id => Id, side => side(Side), initial_hp => InitialHp, hp => Hp,
      max_hp => MaxHp, alive => Alive}.

-spec event(map()) -> battle_client_pb:'BattleEvent'().
event(#{seq := Seq, round := Round, phase := Phase, type := Type, side := Side,
        actor := Actor, target := Target, source_id := SourceId, value := Value,
        hp_before := HpBefore, hp_after := HpAfter, critical := Critical}) ->
    #{seq => Seq, round => Round, phase => phase(Phase),
      type => event_type(Type), side => side(Side), actor => Actor,
      target => Target, source_id => SourceId, value => Value,
      hp_before => HpBefore, hp_after => HpAfter, critical => Critical}.

%% The engine's names (atoms decoded from its ETF) -> protobuf enum symbols.
%% A name added to the engine but not here becomes *_UNSPECIFIED, which
%% clients skip; add it to the .proto and to these functions together.

-spec event_type(atom()) -> atom().
event_type(initiative) -> 'EVENT_TYPE_INITIATIVE';
event_type(action_start) -> 'EVENT_TYPE_ACTION_START';
event_type(action_end) -> 'EVENT_TYPE_ACTION_END';
event_type(skill) -> 'EVENT_TYPE_SKILL';
event_type(passive) -> 'EVENT_TYPE_PASSIVE';
event_type(damage) -> 'EVENT_TYPE_DAMAGE';
event_type(direct_damage) -> 'EVENT_TYPE_DIRECT_DAMAGE';
event_type(miss) -> 'EVENT_TYPE_MISS';
event_type(heal) -> 'EVENT_TYPE_HEAL';
event_type(death) -> 'EVENT_TYPE_DEATH';
event_type(buff_add) -> 'EVENT_TYPE_BUFF_ADD';
event_type(buff_remove) -> 'EVENT_TYPE_BUFF_REMOVE';
event_type(buff_reaction) -> 'EVENT_TYPE_BUFF_REACTION';
event_type(buff_expire) -> 'EVENT_TYPE_BUFF_EXPIRE';
event_type(chain) -> 'EVENT_TYPE_CHAIN';
event_type(negate) -> 'EVENT_TYPE_NEGATE';
event_type(fizzle) -> 'EVENT_TYPE_FIZZLE';
event_type(_) -> 'EVENT_TYPE_UNSPECIFIED'.

-spec phase(atom()) -> atom().
phase(battle) -> 'PHASE_BATTLE';
phase(round_start) -> 'PHASE_ROUND_START';
phase(first_side) -> 'PHASE_FIRST_SIDE';
phase(second_side) -> 'PHASE_SECOND_SIDE';
phase(round_end) -> 'PHASE_ROUND_END';
phase(_) -> 'PHASE_UNSPECIFIED'.

-spec end_reason(atom()) -> atom().
end_reason(initial_state) -> 'END_REASON_INITIAL_STATE';
end_reason(battle_start) -> 'END_REASON_BATTLE_START';
end_reason(round_start) -> 'END_REASON_ROUND_START';
end_reason(all_units_defeated) -> 'END_REASON_ALL_UNITS_DEFEATED';
end_reason(round_end) -> 'END_REASON_ROUND_END';
end_reason(max_rounds) -> 'END_REASON_MAX_ROUNDS';
end_reason(event_limit) -> 'END_REASON_EVENT_LIMIT';
end_reason(_) -> 'END_REASON_UNSPECIFIED'.

-spec side(attacker | defender) -> atom().
side(attacker) -> 'SIDE_ATTACKER';
side(defender) -> 'SIDE_DEFENDER'.

-spec winner(attacker | defender | draw) -> atom().
winner(attacker) -> 'WINNER_ATTACKER';
winner(defender) -> 'WINNER_DEFENDER';
winner(draw) -> 'WINNER_DRAW'.

-spec gauntlet_status(completed | defeated | draw) -> atom().
gauntlet_status(completed) -> 'GAUNTLET_STATUS_COMPLETED';
gauntlet_status(defeated) -> 'GAUNTLET_STATUS_DEFEATED';
gauntlet_status(draw) -> 'GAUNTLET_STATUS_DRAW'.

-spec error_code_enum(error_code()) -> atom().
error_code_enum(bad_message) -> 'ERROR_CODE_BAD_MESSAGE';
error_code_enum(invalid_request) -> 'ERROR_CODE_INVALID_REQUEST';
error_code_enum(retry_later) -> 'ERROR_CODE_RETRY_LATER';
error_code_enum(internal) -> 'ERROR_CODE_INTERNAL'.
