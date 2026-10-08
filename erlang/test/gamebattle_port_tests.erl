-module(gamebattle_port_tests).

-include_lib("eunit/include/eunit.hrl").

%% Config loading across Port restarts. Runs only when GAMEBATTLE_PORT points
%% at a built gamebattle_port and GAMEBATTLE_TEST_CONFIG at a package compiled
%% from config/example, e.g.
%%   gamebattle_config_compiler --input-dir config/example --output example.gbcfg

port_config_test_() ->
    case {os:type(), os:getenv("GAMEBATTLE_PORT"), os:getenv("GAMEBATTLE_TEST_CONFIG")} of
        {{unix, _}, Port, Config} when Port =/= false, Config =/= false ->
            {foreach, fun setup/0, fun cleanup/1,
             [fun() -> config_survives_port_restart(Config) end,
              fun() -> loaded_config_is_remembered(Config) end,
              fun bad_config_stops_startup/0]};
        _ ->
            []
    end.

setup() ->
    %% A killed Port makes the supervisor log crash reports; keep them out of
    %% the test output.
    #{level := Level} = logger:get_primary_config(),
    ok = logger:set_primary_config(level, none),
    GamebattleConfig = os:getenv("GAMEBATTLE_CONFIG"),
    true = os:unsetenv("GAMEBATTLE_CONFIG"),
    {Level, GamebattleConfig}.

cleanup({Level, GamebattleConfig}) ->
    _ = application:stop(gamebattle),
    ok = application:unset_env(gamebattle, config_path),
    case GamebattleConfig of
        false -> ok;
        Value -> true = os:putenv("GAMEBATTLE_CONFIG", Value)
    end,
    ok = logger:set_primary_config(level, Level).

config_survives_port_restart(Config) ->
    ok = application:set_env(gamebattle, config_path, Config),
    {ok, _} = application:ensure_all_started(gamebattle),
    ?assertMatch({ok, _}, gamebattle:simulate(port, id_request())),
    kill_port(),
    ?assertMatch({ok, _}, gamebattle:simulate(port, id_request())).

loaded_config_is_remembered(Config) ->
    {ok, _} = application:ensure_all_started(gamebattle),
    ?assertMatch({error, #{type := invalid_request}},
                 gamebattle:simulate(port, id_request())),
    {ok, _} = gamebattle:load_config(port, Config),
    kill_port(),
    ?assertMatch({ok, _}, gamebattle:simulate(port, id_request())).

bad_config_stops_startup() ->
    ok = application:set_env(gamebattle, config_path, "/nonexistent/battle.gbcfg"),
    ?assertMatch({error, _}, application:ensure_all_started(gamebattle)).

%% The example request, with the first unit's skill and passive given as IDs
%% from config/example instead of inline definitions.
id_request() ->
    #{attacker := Attacker = #{units := [First | Rest]}} = Request =
        gamebattle:example_request(),
    ById = maps:without([skills, passives],
                        First#{skill_ids => [501], passive_ids => [701]}),
    Request#{attacker := Attacker#{units := [ById | Rest]}}.

%% Kills the C++ process and waits until the supervisor has restarted the worker.
kill_port() ->
    Worker = whereis(gamebattle_port),
    {state, Port, _, _} = sys:get_state(Worker),
    {os_pid, OsPid} = erlang:port_info(Port, os_pid),
    _ = os:cmd("kill -9 " ++ integer_to_list(OsPid)),
    wait_for_new_worker(Worker, 50).

wait_for_new_worker(_Old, 0) ->
    error(worker_not_restarted);
wait_for_new_worker(Old, Tries) ->
    case whereis(gamebattle_port) of
        New when is_pid(New), New =/= Old -> ok;
        _ -> timer:sleep(100), wait_for_new_worker(Old, Tries - 1)
    end.
