-module(gamebattle_bench).

%% Compares the three ways to run a battle: the NIF, the Port (C++ in its own
%% OS process) and gamebattle_erl (plain Erlang). Install a Release build of the
%% Port and the NIF into erlang/priv first, then from erlang/:
%%
%%   rebar3 as test compile
%%   erl -noshell -pa _build/test/lib/gamebattle/ebin _build/test/lib/gamebattle/test \
%%       -eval 'gamebattle_bench:run(), halt().'
%%
%% Every adapter runs the same requests; run/0 first checks that all three
%% return identical results. Times include everything a caller pays: encoding
%% the request, the simulation and decoding the result.
%%
%% With the report option (summary, actions or events) every request asks for
%% a compact result, which carries the client's BattleReport bytes instead of
%% event maps:
%%
%%   gamebattle_bench:run(#{report => actions, scenarios => [example, long, stage2]})

-export([run/0, run/1, long_battle/1]).

-define(ADAPTERS, [nif, port, erlang]).
-define(SCENARIOS, [example, random_mix, long]).

-spec run() -> map().
run() ->
    run(#{}).

%% Options: sequential_calls (per scenario, default 2000), concurrent_calls
%% (default 4000), processes (concurrent callers, default the number of
%% schedulers), report (none, the default, or summary, actions or events) and
%% scenarios (default [example, random_mix, long]; stage2 and stage3 are the
%% gateway's stress stages).
-spec run(map()) -> map().
run(Options) ->
    {ok, _} = application:ensure_all_started(gamebattle),
    Processes = maps:get(processes, Options, erlang:system_info(schedulers_online)),
    Report = maps:get(report, Options, none),
    io:format("OTP ~s, ~b schedulers, ~b dirty CPU schedulers, report: ~s~n",
              [erlang:system_info(otp_release), erlang:system_info(schedulers_online),
               erlang:system_info(dirty_cpu_schedulers_online), Report]),
    maps:from_list([{Name, scenario(Name, requests(Name), Report, Processes, Options)}
                    || Name <- maps:get(scenarios, Options, ?SCENARIOS)]).

requests(example) -> [gamebattle:example_request()];
requests(random_mix) -> [gamebattle_erl_tests:random_request(S) || S <- lists:seq(1, 200)];
requests(long) -> [long_battle(S) || S <- lists:seq(1, 20)];
requests(stage2) -> [stage(2, S) || S <- lists:seq(1, 3)];
requests(stage3) -> [stage(3, S) || S <- lists:seq(1, 3)].

stage(Id, Seed) ->
    Lineup = [#{unit_id => N, position => N} || N <- lists:seq(1, 7)],
    {ok, Request} = gamebattle_demo:request(Id, Lineup, Seed),
    Request.

-spec scenario(atom(), [map()], atom(), pos_integer(), map()) -> map().
scenario(Name, Full, Report, Processes, Options) ->
    Events = lists:sum([length(maps:get(events, Result))
                        || {ok, Result} <- [simulate(erlang, R) || R <- Full]])
             div length(Full),
    Requests = case Report of
                   none -> Full;
                   _ -> [R#{report => Report} || R <- Full]
               end,
    Expected = [simulate(nif, R) || R <- Requests],
    [Expected = [simulate(Adapter, R) || R <- Requests] || Adapter <- [port, erlang]],
    Calls = scale_calls(maps:get(sequential_calls, Options, 2000), Events),
    io:format("~n== ~s: ~b request(s), ~b events per battle on average~n",
              [Name, length(Requests), Events]),
    io:format("~-8s ~12s ~12s ~12s ~14s~n",
              ["adapter", "mean (us)", "p50 (us)", "p99 (us)", "ns per event"]),
    Sequential =
        maps:from_list(
          [begin
               Times = sequential(Adapter, Requests, Calls),
               Mean = lists:sum(Times) / length(Times),
               io:format("~-8s ~12.1f ~12.1f ~12.1f ~14b~n",
                         [Adapter, Mean / 1000, percentile(Times, 50) / 1000,
                          percentile(Times, 99) / 1000, round(Mean / max(1, Events))]),
               {Adapter, #{mean_us => Mean / 1000,
                           p50_us => percentile(Times, 50) / 1000,
                           p99_us => percentile(Times, 99) / 1000}}
           end
           || Adapter <- ?ADAPTERS]),
    ConcurrentCalls = scale_calls(maps:get(concurrent_calls, Options, 4000), Events),
    io:format("~b concurrent callers, battles per second:~n", [Processes]),
    Pool = [Worker || _ <- lists:seq(1, Processes),
                      {ok, Worker} <- [gen_server:start(gamebattle_port, #{}, [])]],
    Concurrent =
        maps:from_list(
          [begin
               Rate = concurrent(Adapter, Requests, ConcurrentCalls, Processes, Pool),
               io:format("  ~-28s ~10b~n", [label(Adapter, Processes), round(Rate)]),
               {Adapter, Rate}
           end
           || Adapter <- ?ADAPTERS ++ [port_pool]]),
    [gen_server:stop(Worker) || Worker <- Pool],
    #{events => Events, sequential => Sequential, concurrent => Concurrent}.

label(port, _) -> "port (one worker)";
label(port_pool, N) -> lists:flatten(io_lib:format("port (~b workers)", [N]));
label(Adapter, _) -> atom_to_list(Adapter).

%% Long battles get fewer calls so each scenario takes similar time.
scale_calls(Calls, Events) ->
    max(50, Calls * 250 div max(250, Events)).

simulate(port_pool, {Worker, Request}) ->
    gen_server:call(Worker, {request, Request}, infinity);
simulate(Adapter, Request) ->
    gamebattle:simulate(Adapter, Request).

sequential(Adapter, Requests, Calls) ->
    [simulate(Adapter, R) || R <- Requests],   % warm up
    Cycle = list_to_tuple(Requests),
    [begin
         Request = element((N rem tuple_size(Cycle)) + 1, Cycle),
         T0 = erlang:monotonic_time(nanosecond),
         {ok, _} = simulate(Adapter, Request),
         erlang:monotonic_time(nanosecond) - T0
     end
     || N <- lists:seq(1, Calls)].

concurrent(Adapter, Requests, Calls, Processes, Pool) ->
    Parent = self(),
    Cycle = list_to_tuple(Requests),
    PerProcess = max(1, Calls div Processes),
    T0 = erlang:monotonic_time(nanosecond),
    Pids = [spawn_link(
              fun() ->
                  [begin
                       Request = element(((P + N) rem tuple_size(Cycle)) + 1, Cycle),
                       Arg = case Adapter of
                                 port_pool -> {lists:nth(P, Pool), Request};
                                 _ -> Request
                             end,
                       {ok, _} = simulate(Adapter, Arg)
                   end
                   || N <- lists:seq(1, PerProcess)],
                  Parent ! {done, self()}
              end)
            || P <- lists:seq(1, Processes)],
    [receive {done, Pid} -> ok end || Pid <- Pids],
    Seconds = (erlang:monotonic_time(nanosecond) - T0) / 1.0e9,
    PerProcess * Processes / Seconds.

percentile(Times, P) ->
    Sorted = lists:sort(Times),
    lists:nth(max(1, round(length(Sorted) * P / 100)), Sorted).

%% Six heroes a side with area skills, stacking damage-over-time buffs and
%% healing passives: a battle that runs to the round limit with thousands of
%% events.
-spec long_battle(integer()) -> map().
long_battle(Seed) ->
    Side = fun(Base) -> [hero(Base + N, N, Seed) || N <- lists:seq(1, 6)] end,
    #{battle_id => Seed, seed => Seed * 7919, max_rounds => 50, max_events => 20000,
      attacker => #{formation => wedge, initiative_bonus => 10, units => Side(1000)},
      defender => #{formation => wall, initiative_bonus => 0, units => Side(2000)}}.

hero(Id, Position, Seed) ->
    Burn = #{id => 90000 + Id, name => <<"burn">>,
             lifetime => #{type => finite, duration => 3, decrement_on => round_end},
             stacking => #{max_stacks => 3, policy => stack, refresh => reset},
             modifiers => [#{attribute => defense, operation => scale_bp, value => -500}],
             reactions => [#{trigger => round_end, source => applier,
                             stack_scaling => per_stack,
                             effects => [#{type => direct_damage, target => self,
                                           attack_bp => 1500}]}]},
    #{id => Id, kind => hero, position => Position, level => 80,
      final_stats => #{hp => 60000 + Position * 1000 + Seed, attack => 300 + Position * 20,
                       defense => 120, speed => 90 + (Position * 7 + Seed) rem 40,
                       crit_rate_bp => 1500, crit_damage_bp => 16000, dodge_rate_bp => 500},
      skills => [#{id => 500 + Position, name => <<"sweep">>, chance_bp => 3000, priority => 1,
                   effects => [#{type => damage, target => all_enemies, attack_bp => 6000},
                               #{type => add_buff, target => enemy_lowest_hp, buff => Burn}]}],
      passives => [#{id => 700 + Position, name => <<"second_wind">>, trigger => on_damaged,
                     chance_bp => 2000, max_triggers_per_round => 2,
                     effects => [#{type => heal, target => self, flat => 300}]}]}.
