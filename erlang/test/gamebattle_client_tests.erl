-module(gamebattle_client_tests).

-include_lib("eunit/include/eunit.hrl").

%% Every event type, phase and end reason the C++ engine emits. When the engine
%% gains one, add it here, to proto/battle_client.proto and to
%% gamebattle_client; the tests below then check the mapping.
-define(ENGINE_EVENT_TYPES,
        [initiative, action_start, action_end, skill, passive, damage,
         direct_damage, miss, heal, death, buff_add, buff_remove, buff_reaction,
         buff_expire, chain, negate, fizzle]).
-define(ENGINE_PHASES, [battle, round_start, first_side, second_side, round_end]).
-define(ENGINE_END_REASONS,
        [initial_state, battle_start, round_start, all_units_defeated, round_end,
         max_rounds, event_limit]).

%% Shaped exactly like a map returned by gamebattle:simulate/1,2.
event(Seq, Type, Phase) ->
    #{seq => Seq, round => 1, phase => Phase, type => Type, side => defender,
      actor => 2001, target => 1001, source_id => 501, value => 120,
      hp_before => 900, hp_after => 780, critical => Seq rem 2 =:= 0}.

result(Events) ->
    #{battle_id => 3001, seed => 42, source_battle_id => 0, winner => attacker,
      reason => all_units_defeated, rounds => 7,
      attacker_initiative => 260, defender_initiative => 240,
      units => [#{id => 1001, side => attacker, initial_hp => 1800, hp => 780,
                  max_hp => 1800, alive => true},
                #{id => 2001, side => defender, initial_hp => 1500, hp => 0,
                  max_hp => 1500, alive => false}],
      events => Events}.

decode_server(Bytes) ->
    battle_client_pb:decode_msg(Bytes, 'ServerMessage').

battle_report_round_trip_test() ->
    Events = [event(Seq, Type, battle)
              || {Seq, Type} <- lists:zip(lists:seq(1, length(?ENGINE_EVENT_TYPES)),
                                          ?ENGINE_EVENT_TYPES)],
    Bytes = gamebattle_client:encode_battle_report(17, result(Events)),
    #{request_id := 17, body := {battle_report, Report}} = decode_server(Bytes),
    ?assertMatch(#{battle_id := 3001, seed := 42, winner := 'WINNER_ATTACKER',
                   reason := 'END_REASON_ALL_UNITS_DEFEATED', rounds := 7,
                   attacker_initiative := 260, defender_initiative := 240},
                 Report),
    [Attacker, Defender] = maps:get(units, Report),
    ?assertMatch(#{id := 1001, side := 'SIDE_ATTACKER', hp := 780, alive := true}, Attacker),
    ?assertMatch(#{id := 2001, side := 'SIDE_DEFENDER', hp := 0, alive := false}, Defender),
    Decoded = maps:get(events, Report),
    ?assertEqual(length(Events), length(Decoded)),
    [First | _] = Decoded,
    ?assertMatch(#{seq := 1, round := 1, phase := 'PHASE_BATTLE',
                   type := 'EVENT_TYPE_INITIATIVE', side := 'SIDE_DEFENDER',
                   actor := 2001, target := 1001, source_id := 501, value := 120,
                   hp_before := 900, hp_after := 780, critical := false},
                 First).

every_engine_name_is_mapped_test() ->
    Report = gamebattle_client:battle_report(
               result([event(1, Type, Phase)
                       || Type <- ?ENGINE_EVENT_TYPES, Phase <- ?ENGINE_PHASES])),
    Types = lists:usort([Type || #{type := Type} <- maps:get(events, Report)]),
    Phases = lists:usort([Phase || #{phase := Phase} <- maps:get(events, Report)]),
    ?assertEqual(length(?ENGINE_EVENT_TYPES), length(Types)),
    ?assertNot(lists:member('EVENT_TYPE_UNSPECIFIED', Types)),
    ?assertEqual(length(?ENGINE_PHASES), length(Phases)),
    ?assertNot(lists:member('PHASE_UNSPECIFIED', Phases)),
    Reasons = [maps:get(reason, gamebattle_client:battle_report(
                                  (result([]))#{reason => Reason}))
               || Reason <- ?ENGINE_END_REASONS],
    ?assertEqual(length(?ENGINE_END_REASONS), length(lists:usort(Reasons))),
    ?assertNot(lists:member('END_REASON_UNSPECIFIED', Reasons)).

unknown_engine_names_become_unspecified_test() ->
    Report = gamebattle_client:battle_report(
               (result([event(1, some_future_event, some_future_phase)]))#{
                 reason => some_future_reason}),
    ?assertMatch(#{reason := 'END_REASON_UNSPECIFIED',
                   events := [#{type := 'EVENT_TYPE_UNSPECIFIED',
                                phase := 'PHASE_UNSPECIFIED'}]},
                 Report),
    %% ...and still encode: clients skip what they don't know.
    ?assert(is_binary(gamebattle_client:encode_battle_report(0, result([])))).

invalid_values_are_rejected_when_encoding_test() ->
    %% {verify, always}: a bad value raises instead of producing corrupt bytes.
    ?assertError(_, gamebattle_client:encode_battle_report(
                      1, (result([]))#{battle_id => -1})),
    ?assertError(_, gamebattle_client:encode_battle_report(-1, result([]))).

gauntlet_report_test() ->
    Gauntlet = #{status => defeated, winner => defender, completed_waves => 1,
                 fought_waves => 2, total_waves => 3, stopped_at_wave => 2,
                 wave_results => [result([]), (result([]))#{winner => defender}],
                 carryover => #{source_battle_id => 3001}},
    Bytes = gamebattle_client:encode_gauntlet_report(0, Gauntlet),
    #{request_id := 0, body := {gauntlet_report, Report}} = decode_server(Bytes),
    ?assertMatch(#{status := 'GAUNTLET_STATUS_DEFEATED', winner := 'WINNER_DEFENDER',
                   completed_waves := 1, fought_waves := 2, total_waves := 3,
                   stopped_at_wave := 2,
                   waves := [#{winner := 'WINNER_ATTACKER'},
                             #{winner := 'WINNER_DEFENDER'}]},
                 Report).

error_test() ->
    Bytes = gamebattle_client:encode_error(9, retry_later, <<"战斗服务繁忙，请稍后再试"/utf8>>),
    ?assertMatch(#{request_id := 9,
                   body := {error, #{code := 'ERROR_CODE_RETRY_LATER',
                                     message := <<"战斗服务繁忙，请稍后再试"/utf8>>}}},
                 decode_server(Bytes)),
    ?assertEqual(invalid_request, gamebattle_client:error_code(
                                    #{type => invalid_request, message => <<"x">>})),
    ?assertEqual(retry_later, gamebattle_client:error_code(#{type => timeout})),
    ?assertEqual(retry_later, gamebattle_client:error_code(#{type => port_exit})),
    ?assertEqual(internal, gamebattle_client:error_code(#{type => internal_error})).

client_bytes(RequestId, Lineup) ->
    battle_client_pb:encode_msg(
      #{request_id => RequestId,
        body => {start_battle,
                 #{stage_id => 12,
                   lineup => [#{unit_id => U, position => P} || {U, P} <- Lineup]}}},
      'ClientMessage').

decode_start_battle_test() ->
    ?assertEqual({ok, 5, {start_battle,
                          #{stage_id => 12,
                            lineup => [#{unit_id => 1001, position => 1},
                                       #{unit_id => 1002, position => 2}],
                            summary_only => false}}},
                 gamebattle_client:decode_client_message(
                   client_bytes(5, [{1001, 1}, {1002, 2}]))),
    Summary = battle_client_pb:encode_msg(
                #{request_id => 6,
                  body => {start_battle, #{stage_id => 1, summary_only => true,
                                           lineup => [#{unit_id => 1, position => 1}]}}},
                'ClientMessage'),
    ?assertMatch({ok, 6, {start_battle, #{summary_only := true}}},
                 gamebattle_client:decode_client_message(Summary)).

decode_rejects_bad_requests_test() ->
    Decode = fun gamebattle_client:decode_client_message/1,
    ?assertEqual({error, 6, bad_message}, Decode(client_bytes(6, []))),
    ?assertEqual({error, 7, bad_message}, Decode(client_bytes(7, [{1001, 1}, {1001, 2}]))),
    ?assertEqual({error, 8, bad_message}, Decode(client_bytes(8, [{0, 1}]))),
    ?assertEqual({error, 9, bad_message}, Decode(client_bytes(9, [{1001, 1001}]))),
    ?assertEqual({error, 10, bad_message}, Decode(client_bytes(10, [{1001, -1}]))),
    TooMany = [{Id, 1} || Id <- lists:seq(1, 257)],
    ?assertEqual({error, 11, bad_message}, Decode(client_bytes(11, TooMany))),
    %% No body at all, and bytes that are not a ClientMessage.
    ?assertEqual({error, 12, bad_message},
                 Decode(battle_client_pb:encode_msg(#{request_id => 12}, 'ClientMessage'))),
    ?assertEqual({error, 0, bad_message}, Decode(<<16#0a, 200>>)),
    ?assertEqual({error, 0, bad_message}, Decode(<<8, 255, 255, 255, 255, 255, 255, 255,
                                                    255, 255, 255, 255, 1>>)).

%% End to end through the real C++ Port. Runs only when GAMEBATTLE_PORT points
%% at a built gamebattle_port executable.
port_round_trip_test_() ->
    case os:getenv("GAMEBATTLE_PORT") of
        false -> [];
        _Path ->
            {setup,
             fun() -> {ok, Apps} = application:ensure_all_started(gamebattle), Apps end,
             fun(Apps) -> [application:stop(App) || App <- lists:reverse(Apps)] end,
             fun port_round_trip/0}
    end.

port_round_trip() ->
    {ok, Result} = gamebattle:simulate(port, gamebattle:example_request()),
    Bytes = gamebattle_client:encode_battle_report(1, Result),
    #{body := {battle_report, Report}} = decode_server(Bytes),
    Events = maps:get(events, Report),
    ?assertEqual(length(maps:get(events, Result)), length(Events)),
    ?assertNot(lists:member('EVENT_TYPE_UNSPECIFIED', [T || #{type := T} <- Events])),
    ?assertNot(lists:member('PHASE_UNSPECIFIED', [P || #{phase := P} <- Events])),
    ?assertNotEqual('END_REASON_UNSPECIFIED', maps:get(reason, Report)),
    ?assertEqual(maps:get(battle_id, Result), maps:get(battle_id, Report)).
