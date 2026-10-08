-module(gamebattle_gateway_tests).

-include_lib("eunit/include/eunit.hrl").

%% The test gateway end to end over TCP, with the plain-Erlang engine. One
%% battle runs at a time and none may wait, so a second request while the
%% first runs is rejected as busy.

gateway_test_() ->
    {setup, fun setup/0, fun cleanup/1,
     [fun battle/0, fun summary_only/0, fun unknown_unit/0, fun bad_frame/0, fun busy/0]}.

setup() ->
    Settings = [{gateway_port, 0}, {gateway_engine, erlang}, {battle_workers, 1},
                {battle_queue, 0}],
    [ok = application:set_env(gamebattle, K, V) || {K, V} <- Settings],
    {ok, Apps} = application:ensure_all_started(gamebattle),
    {Apps, [K || {K, _} <- Settings]}.

cleanup({Apps, Keys}) ->
    [application:stop(App) || App <- lists:reverse(Apps)],
    [application:unset_env(gamebattle, K) || K <- Keys].

connect() ->
    {ok, Socket} = gen_tcp:connect("localhost", gamebattle_gateway:port(),
                                   [binary, {packet, 4}, {active, false}]),
    Socket.

start_battle(Socket, RequestId, Stage, Lineup, SummaryOnly) ->
    Message = #{request_id => RequestId,
                body => {start_battle,
                         #{stage_id => Stage, summary_only => SummaryOnly,
                           lineup => [#{unit_id => U, position => P} || {U, P} <- Lineup]}}},
    ok = gen_tcp:send(Socket, battle_client_pb:encode_msg(Message, 'ClientMessage')).

reply(Socket) ->
    {ok, Frame} = gen_tcp:recv(Socket, 0, 30000),
    battle_client_pb:decode_msg(Frame, 'ServerMessage').

battle() ->
    Socket = connect(),
    start_battle(Socket, 1, 1, [{1, 1}, {2, 2}, {3, 3}], false),
    #{request_id := 1, body := {battle_report, Report}} = reply(Socket),
    ?assertNotEqual('WINNER_UNSPECIFIED', maps:get(winner, Report)),
    ?assert(length(maps:get(events, Report)) > 10),
    ?assertEqual(8, length(maps:get(units, Report))),
    gen_tcp:close(Socket).

summary_only() ->
    Socket = connect(),
    start_battle(Socket, 2, 1, [{1, 1}], true),
    #{request_id := 2, body := {battle_report, Report}} = reply(Socket),
    ?assertEqual([], maps:get(events, Report, [])),
    ?assertEqual(6, length(maps:get(units, Report))),
    gen_tcp:close(Socket).

unknown_unit() ->
    Socket = connect(),
    start_battle(Socket, 3, 1, [{99, 1}], false),
    ?assertMatch(#{request_id := 3, body := {error, #{code := 'ERROR_CODE_INVALID_REQUEST'}}},
                 reply(Socket)),
    start_battle(Socket, 4, 42, [{1, 1}], false),
    ?assertMatch(#{request_id := 4, body := {error, #{code := 'ERROR_CODE_INVALID_REQUEST'}}},
                 reply(Socket)),
    gen_tcp:close(Socket).

bad_frame() ->
    Socket = connect(),
    ok = gen_tcp:send(Socket, <<16#0a, 200>>),
    ?assertMatch(#{body := {error, #{code := 'ERROR_CODE_BAD_MESSAGE'}}}, reply(Socket)),
    gen_tcp:close(Socket).

busy() ->
    Socket = connect(),
    Lineup = [{N, N} || N <- lists:seq(1, 7)],
    start_battle(Socket, 5, 3, Lineup, true),
    start_battle(Socket, 6, 3, Lineup, true),
    Replies = [reply(Socket), reply(Socket)],
    ?assertMatch([#{request_id := 6, body := {error, #{code := 'ERROR_CODE_RETRY_LATER'}}},
                  #{request_id := 5, body := {battle_report, _}}],
                 Replies),
    gen_tcp:close(Socket).
