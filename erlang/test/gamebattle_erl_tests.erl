-module(gamebattle_erl_tests).

-include_lib("eunit/include/eunit.hrl").

%% The pure-Erlang engine must give exactly what the C++ engine gives. The
%% differential tests generate random requests that use every feature (and
%% random invalid ones) and compare gamebattle_erl with the C++ Port result
%% for result. They run when GAMEBATTLE_PORT points at a built Port; with
%% GAMEBATTLE_TEST_CONFIG (a pack compiled from config/example) they also cover
%% skill_ids/passive_ids. differential/2 runs any number of cases by hand.
%% Requests with the `report` option compare the client report bytes too.

-export([differential/2, random_request/1, invalid_request/1, config_request/1]).

-define(TRIGGERS, [battle_start, round_start, before_action, on_attack, on_hit, on_damaged,
                   unit_death, after_action, round_end]).
-define(TARGETS, [self, trigger_unit, enemy_front, enemy_lowest_hp, ally_lowest_hp,
                  all_enemies, all_allies]).
-define(ATTRIBUTES, [attack, defense, speed, crit_rate_bp, crit_damage_bp, hit_rate_bp,
                     dodge_rate_bp, damage_bonus_bp, damage_reduction_bp]).

%%% Without the Port ------------------------------------------------------------

%% Facts about the example battle as the C++ engine computes it.
example_request_test() ->
    {ok, Result} = gamebattle_erl:simulate(gamebattle:example_request(), undefined),
    ?assertMatch(#{winner := attacker, reason := all_units_defeated, rounds := 19,
                   attacker_initiative := 230, defender_initiative := 200},
                 Result),
    ?assertEqual(226, length(maps:get(events, Result))),
    ?assertEqual(lists:seq(1, 226), [Seq || #{seq := Seq} <- maps:get(events, Result)]).

same_request_same_result_test() ->
    Request = random_request(7),
    ?assertEqual(gamebattle_erl:simulate(Request, undefined),
                 gamebattle_erl:simulate(Request, undefined)).

invalid_requests_test() ->
    Example = gamebattle:example_request(),
    Invalid = fun(Message, Request) ->
                  ?assertEqual({error, #{type => invalid_request, message => Message}},
                               gamebattle_erl:simulate(Request, undefined))
              end,
    Invalid(<<"max_rounds must be between 1 and 10000">>, Example#{max_rounds => 0}),
    Invalid(<<"seed must not be negative">>, Example#{seed => -1}),
    Invalid(<<"max_events must be an integer">>, Example#{max_events => 1.5}),
    Invalid(<<"request requires attacker and defender formation maps">>,
            maps:remove(defender, Example)),
    Invalid(<<"ETF map keys must be atoms or binaries">>, Example#{1 => 2}),
    Invalid(<<"positive integer does not fit into int64">>, Example#{battle_id => 1 bsl 63}),
    Invalid(<<"skill_ids/passive_ids require a loaded battle config pack">>,
            with_first_unit(Example, fun(U) -> (maps:remove(skills, U))#{skill_ids => [501]} end)),
    Invalid(<<"report must be summary, actions, or events">>, Example#{report => all}),
    Invalid(<<"report must be an atom or binary">>, Example#{report => 2}),
    %% The report option is read before the rest of the request.
    Invalid(<<"report must be summary, actions, or events">>,
            (maps:remove(defender, Example))#{<<"report">> => <<"full">>}).

%% The actions report adds up every event, step by step.
report_test() ->
    {ok, Full} = gamebattle_erl:simulate(gamebattle:example_request(), undefined),
    Events = maps:get(events, Full),
    Decode = fun(Detail) ->
                 {ok, Result} = gamebattle_erl:simulate(
                                  (gamebattle:example_request())#{report => Detail}, undefined),
                 ?assertEqual(maps:remove(events, Full), maps:remove(report, Result)),
                 battle_client_pb:decode_msg(maps:get(report, Result), 'BattleReport')
             end,
    Summary = Decode(summary),
    ?assertEqual(length(maps:get(units, Full)), length(maps:get(units, Summary))),
    ?assertEqual([], maps:get(events, Summary)),
    ?assertEqual([], maps:get(actions, Summary)),
    ?assertEqual(length(Events), length(maps:get(events, Decode(events)))),
    #{actions := Actions, events := []} = Decode(actions),
    ?assertEqual(length(Events), lists:sum([N || #{event_count := N} <- Actions])),
    ?assertEqual(lists:sum([V || #{type := T, value := V} <- Events,
                                 T =:= damage orelse T =:= direct_damage]),
                 lists:sum([D || #{units := Units} <- Actions, #{damage := D} <- Units])),
    ?assertEqual(length([E || #{type := action_start} = E <- Events]),
                 length([A || #{actor := Actor} = A <- Actions, Actor =/= 0])),
    %% The bytes are canonical: what gpb writes for the same message.
    [?assertEqual(Bytes, battle_client_pb:encode_msg(
                           battle_client_pb:decode_msg(Bytes, 'BattleReport'), 'BattleReport'))
     || Detail <- [summary, actions, events],
        Bytes <- [gamebattle_report:encode(Full, Detail)]],
    ?assertEqual(battle_client_pb:encode_msg(gamebattle_client:battle_report(Full), 'BattleReport'),
                 gamebattle_report:encode(Full, events)),
    %% gamebattle_client sends the bytes as they are.
    {ok, Result} = gamebattle_erl:simulate((gamebattle:example_request())#{report => actions},
                                           undefined),
    Message = gamebattle_client:encode_battle_report(7, Result),
    ?assertMatch(#{request_id := 7, body := {battle_report, #{actions := [_ | _]}}},
                 battle_client_pb:decode_msg(Message, 'ServerMessage')),
    ?assertEqual(battle_client_pb:encode_msg(
                   #{request_id => 7,
                     body => {battle_report,
                              battle_client_pb:decode_msg(maps:get(report, Result),
                                                          'BattleReport')}},
                   'ServerMessage'),
                 Message).

missing_config_pack_test() ->
    ?assertEqual({error, #{type => config_load_failed,
                           message => <<"cannot open gamebattle config pack: /nonexistent.gbcfg">>}},
                 gamebattle_erl:load_config("/nonexistent.gbcfg")).

%%% Against the C++ Port --------------------------------------------------------

port_test_() ->
    case os:getenv("GAMEBATTLE_PORT") of
        false -> [];
        _ ->
            {setup,
             fun() -> {ok, Apps} = application:ensure_all_started(gamebattle), Apps end,
             fun(Apps) -> [application:stop(App) || App <- lists:reverse(Apps)] end,
             [{timeout, 300, fun() -> ?assertEqual([], differential(1, 400)) end},
              {timeout, 300, fun reports_match/0},
              {timeout, 300, fun invalid_requests_match/0},
              {timeout, 300, fun config_requests_match/0},
              fun gauntlet_matches/0]}
    end.

%% Compact results: the summary and the client report bytes.
reports_match() ->
    Mismatches = [{Seed, Detail} || Seed <- lists:seq(1, 300),
                                    Detail <- [summary, actions, events, <<"actions">>],
                                    not same((random_request(Seed))#{report => Detail})],
    ?assertEqual([], Mismatches).

invalid_requests_match() ->
    Mismatches = [Seed || Seed <- lists:seq(1, 400),
                          not same(invalid_request(Seed))],
    ?assertEqual([], Mismatches).

config_requests_match() ->
    case os:getenv("GAMEBATTLE_TEST_CONFIG") of
        false -> ok;
        Path ->
            ?assertEqual(gamebattle:load_config(port, Path), gamebattle:load_config(erlang, Path)),
            Mismatches = [Seed || Seed <- lists:seq(1, 200),
                                  not same(config_request(Seed))],
            ?assertEqual([], Mismatches)
    end.

gauntlet_matches() ->
    Example = gamebattle:example_request(),
    Waves = [#{battle_id => N, seed => N * 7919, defender => maps:get(defender, random_request(N))}
             || N <- lists:seq(1, 5)],
    Attacker = maps:get(attacker, Example),
    ?assertEqual(gamebattle:run_gauntlet(port, Attacker, Waves),
                 gamebattle:run_gauntlet(erlang, Attacker, Waves)).

%% Runs Count random requests from FirstSeed on and returns the seeds whose
%% results differ between the Port and the Erlang engine.
-spec differential(integer(), non_neg_integer()) -> [integer()].
differential(FirstSeed, Count) ->
    [Seed || Seed <- lists:seq(FirstSeed, FirstSeed + Count - 1),
             not same(random_request(Seed))].

same(Request) ->
    gamebattle:simulate(port, Request) =:= gamebattle_erl:simulate(Request).

%%% Request generator -----------------------------------------------------------

%% A random valid request, using every feature of the engine.
-spec random_request(integer()) -> map().
random_request(Seed) ->
    rand:seed(exsss, {Seed, Seed * 31 + 7, Seed * 131 + 11}),
    put(next_buff_id, 9000),
    put(buff_ids, []),
    Attacker = [unit(1000 + N) || N <- lists:seq(1, int(1, 6))],
    Defender = [unit(2000 + N) || N <- lists:seq(1, int(1, 6))],
    Request = #{battle_id => int(0, 1 bsl 40),
                seed => case chance(5) of true -> 0; false -> int(0, (1 bsl 63) - 1) end,
                max_rounds => pick([1, 3, 10, 20, 50]),
                max_events => pick([100, 150, 400, 2000, 10000]),
                attacker => formation(Attacker),
                defender => formation(Defender)},
    maybe_keys_as_binaries(maybe_initial_conditions(Request, Attacker ++ Defender)).

formation(Units) ->
    maps:merge(#{units => Units},
               optional([{formation, pick([crane_wing, <<"shield_wall">>])},
                         {initiative_bonus, int(-300, 300)}])).

unit(Id) ->
    Kind = case chance(80) of
               true -> hero;
               false -> pick([beauty, pet, artifact, <<"divine_weapon">>])
           end,
    Base = #{id => Id, kind => Kind, position => int(0, 8), level => int(1, 100),
             final_stats => stats(),
             skills => [skill(N) || N <- lists:seq(1, int(0, 3))],
             passives => [passive(N) || N <- lists:seq(1, int(0, 3))]},
    maps:merge(Base, optional([{can_act, chance(70)}, {targetable, chance(80)},
                               {growth_levels, #{star => int(0, 10)}}])).

stats() ->
    maps:merge(#{hp => int(1, 4000), attack => int(0, 500), defense => int(0, 300),
                 speed => int(0, 200)},
               optional([{crit_rate_bp, int(0, 10000)}, {crit_damage_bp, int(10000, 30000)},
                         {hit_rate_bp, int(5000, 12000)}, {dodge_rate_bp, int(0, 4000)},
                         {damage_bonus_bp, int(-5000, 5000)},
                         {damage_reduction_bp, int(-3000, 9000)}])).

skill(N) ->
    #{id => 500 + N, name => <<"skill">>, chance_bp => int(0, 10000), priority => int(-2, 5),
      effects => [effect(0) || _ <- lists:seq(1, int(1, 3))]}.

passive(N) ->
    Response = chance(25),
    Trigger = case Response of
                  true -> pick([enemy_activate, ally_activate]);
                  false -> pick(?TRIGGERS)
              end,
    Effects = [effect(0) || _ <- lists:seq(1, int(1, 2))] ++
              case Response andalso chance(50) of
                  true -> [#{type => negate}];
                  false -> []
              end,
    #{id => 700 + N, name => <<"passive">>, trigger => Trigger, chance_bp => int(2000, 10000),
      max_triggers_per_round => int(0, 3), effects => shuffle(Effects)}.

effect(Depth) ->
    Kind = case Depth < 2 of
               true -> pick([damage, damage, heal, direct_damage, add_buff, add_buff, remove_buff]);
               false -> pick([damage, heal, direct_damage, remove_buff])
           end,
    Base = maps:merge(#{type => Kind},
                      optional([{target, pick(?TARGETS)}, {target_count, int(1, 3)},
                                {attack_bp, int(0, 20000)}, {flat, int(-100, 300)}])),
    case Kind of
        add_buff -> Base#{buff => buff(Depth + 1)};
        remove_buff -> Base#{buff_id => remove_target()};
        _ -> Base
    end.

buff(Depth) ->
    Id = get(next_buff_id),
    put(next_buff_id, Id + 1),
    put(buff_ids, [Id | get(buff_ids)]),
    Permanent = chance(25),
    Policy = pick([stack, refresh]),
    #{id => Id, name => <<"buff">>,
      lifetime => #{type => case Permanent of true -> permanent; false -> finite end,
                    duration => case Permanent of true -> 0; false -> int(1, 4) end,
                    decrement_on => pick(?TRIGGERS)},
      stacking => #{max_stacks => case Policy of refresh -> 1; stack -> int(1, 5) end,
                    policy => Policy, refresh => pick([reset, extend, keep])},
      modifiers => [modifier() || _ <- lists:seq(1, int(0, 3))],
      reactions => [reaction(Depth) || _ <- lists:seq(1, int(0, 2))]}.

modifier() ->
    case chance(50) of
        true -> #{attribute => pick(?ATTRIBUTES), operation => add, value => int(-200, 300)};
        false -> #{attribute => pick(?ATTRIBUTES), operation => scale_bp, value => int(-6000, 6000)}
    end.

reaction(Depth) ->
    maps:merge(#{effects => [effect(Depth) || _ <- lists:seq(1, int(1, 2))]},
               optional([{trigger, pick(?TRIGGERS)}, {source, pick([owner, applier])},
                         {stack_scaling, pick([once, per_stack])},
                         {chance_bp, int(3000, 10000)}, {max_triggers_per_round, int(0, 2)}])).

remove_target() ->
    case get(buff_ids) of
        [] -> int(9000, 9010);
        Ids -> pick(Ids)
    end.

maybe_initial_conditions(Request, Units) ->
    case chance(30) of
        false -> Request;
        true ->
            States = [#{unit_id => Id, current_hp => int(0, Hp)}
                      || #{id := Id, final_stats := #{hp := Hp}} <- Units, chance(30)],
            Request#{initial_conditions =>
                         #{source_battle_id => int(0, 1000),
                           first_side => pick([automatic, attacker, defender]),
                           unit_states => States}}
    end.

%% Keys and enum values may also be binaries.
maybe_keys_as_binaries(Request) ->
    case chance(10) of
        true -> to_binaries(Request);
        false -> Request
    end.

to_binaries(Map) when is_map(Map) ->
    maps:from_list([{atom_to_binary(K), to_binaries(V)} || {K, V} <- maps:to_list(Map)]);
to_binaries(List) when is_list(List) -> [to_binaries(V) || V <- List];
to_binaries(Atom) when is_atom(Atom), Atom =/= true, Atom =/= false -> atom_to_binary(Atom);
to_binaries(Value) -> Value.

%% A random request with one random defect.
-spec invalid_request(integer()) -> map().
invalid_request(Seed) ->
    Request = random_request(Seed),
    rand:seed(exsss, {Seed, 99, 7}),
    mutate(Request, int(1, 15)).

mutate(R, 1) -> R#{max_rounds => pick([0, 10001, -5, 1 bsl 33])};
mutate(R, 2) -> R#{max_events => pick([99, 1000001, <<"many">>, 2.5])};
mutate(R, 3) -> R#{battle_id => pick([-1, 1 bsl 64, [1, 2], {tuple}, self()])};
mutate(R, 4) -> with_first_unit(R, fun(U) -> U#{kind => pick([dragon, <<"x">>, 7])} end);
mutate(R, 5) -> with_first_unit(R, fun(U) -> maps:remove(final_stats, U) end);
mutate(R, 6) ->
    with_first_unit(R, fun(#{final_stats := S} = U) ->
                           U#{final_stats => S#{pick([hp, crit_damage_bp, hit_rate_bp]) => pick([0, -1, 5000, 200000])}}
                       end);
mutate(R, 7) ->
    with_first_unit(R, fun(U) -> U#{skills => [#{id => 1, effects => [#{type => pick([negate, laser])}]}]} end);
mutate(R, 8) ->
    with_first_unit(R, fun(U) ->
                           U#{passives => [#{id => 9, trigger => pick([on_hit, sometimes]),
                                             effects => [#{type => negate}]}]}
                       end);
mutate(R, 9) ->
    Buff = #{id => 1, name => b, lifetime => #{type => finite, duration => pick([0, 1]), decrement_on => round_end},
             stacking => #{max_stacks => pick([1, 2, 0]), policy => pick([refresh, stack]), refresh => keep},
             modifiers => [], reactions => [], pick([extra, name]) => x},
    with_first_unit(R, fun(U) -> U#{skills => [#{id => 1, effects => [#{type => add_buff, buff => Buff}]}]} end);
mutate(R, 10) ->
    with_first_unit(R, fun(U) -> U#{id => pick([0, 2001, 1002, -3])} end);
mutate(R, 11) ->
    R#{initial_conditions => #{unit_states => [#{unit_id => pick([1001, 4242]), current_hp => pick([-1, 1 bsl 50, 1])}],
                               first_side => pick([automatic, sideways])}};
mutate(R, 12) ->
    Effect = #{type => pick([damage, heal, add_buff, remove_buff]), target_count => pick([0, 1, 300]),
               attack_bp => pick([-1, 0, 2000000]), flat => pick([0, 1 bsl 45])},
    with_first_unit(R, fun(U) -> U#{skills => [#{id => 3, effects => [Effect]}]} end);
mutate(R, 13) ->
    %% The same buff id defined twice.
    Buff = maps:get(buff, effect_with_buff()),
    E = #{type => add_buff, buff => Buff},
    with_first_unit(R, fun(U) -> U#{skills => [#{id => 3, effects => [E, E]}]} end);
mutate(R, 14) ->
    R#{attacker => #{units => pick([[], not_a_list, [not_a_map]])}};
mutate(R, 15) ->
    (mutate(R, int(1, 14)))#{report => pick([full, <<"Actions">>, 3, [], summary, <<"events">>])}.

effect_with_buff() ->
    put(next_buff_id, 100),
    put(buff_ids, []),
    (effect(0))#{type => add_buff, buff => buff(1)}.

with_first_unit(#{attacker := #{units := [First | Rest]} = A} = R, Fun) ->
    R#{attacker := A#{units := [Fun(First) | Rest]}};
with_first_unit(R, _) -> R.

%% Requests that load skills and passives from a pack compiled from
%% config/example.
-spec config_request(integer()) -> map().
config_request(Seed) ->
    Request = random_request(Seed),
    rand:seed(exsss, {Seed, 5, 3}),
    with_first_unit(Request,
                    fun(U) ->
                        U1 = maps:without([skills, passives], U),
                        U1#{skill_ids => pick([[501], [], [501, 501], [999]]),
                            passive_ids => pick([[701], [701, 702, 703], [702], [4]])}
                    end).

%%% Random helpers ----------------------------------------------------------------

int(Low, High) -> Low + rand:uniform(High - Low + 1) - 1.
chance(Percent) -> rand:uniform(100) =< Percent.
pick(List) -> lists:nth(rand:uniform(length(List)), List).
shuffle(List) -> [X || {_, X} <- lists:sort([{rand:uniform(), X} || X <- List])].
optional(Pairs) -> maps:from_list([Pair || Pair <- Pairs, chance(60)]).
