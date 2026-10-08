-module(gamebattle_gauntlet).

-export([run/3, run/4]).

-type adapter() :: port | nif.
-type log_level() :: result_only | summary | full.
-type wave() :: #{
    battle_id := non_neg_integer(),
    seed := non_neg_integer(),
    defender := map(),
    max_rounds => pos_integer(),
    max_execution_steps => pos_integer(),
    max_logged_events => non_neg_integer(),
    log_level => log_level(),
    first_side => automatic | attacker | defender
}.
-type result() :: {ok, map()} | {error, map()}.

-spec run(adapter(), map(), [wave()]) -> result().
run(Adapter, AttackerFormation, Waves) ->
    run(Adapter, AttackerFormation, Waves, #{}).

-spec run(adapter(), map(), [wave()], map()) -> result().
run(Adapter, AttackerFormation, Waves, Options)
        when (Adapter =:= port orelse Adapter =:= nif),
             is_map(AttackerFormation), is_list(Waves), is_map(Options) ->
    case validate_waves(Waves, 1, #{}) of
        ok ->
            case initial_conditions(Options) of
                {ok, InitialConditions} ->
                    run_waves(
                        Adapter,
                        AttackerFormation,
                        Waves,
                        Options,
                        InitialConditions,
                        1,
                        length(Waves),
                        []
                    );
                {error, Reason} ->
                    {error, #{type => invalid_gauntlet_options, reason => Reason}}
            end;
        {error, Error} ->
            {error, Error}
    end;
run(_Adapter, _AttackerFormation, _Waves, _Options) ->
    {error, #{type => invalid_gauntlet_arguments}}.

-spec run_waves(
    adapter(), map(), [wave()], map(), map(), pos_integer(), non_neg_integer(), [map()]
) -> result().
run_waves(_Adapter, _Attacker, [], _Options, Carryover, _Index, Total, Results) ->
    {ok, summary(completed, attacker, Total, 0, Carryover, Results)};
run_waves(Adapter, Attacker, [Wave | Remaining], Options, Carryover, Index, Total, Results) ->
    InitialConditions = Carryover#{
        first_side => maps:get(first_side, Wave, automatic)
    },
    Request = #{
        battle_id => maps:get(battle_id, Wave),
        seed => maps:get(seed, Wave),
        max_rounds => maps:get(max_rounds, Wave, maps:get(max_rounds, Options, 50)),
        max_execution_steps => maps:get(
            max_execution_steps,
            Wave,
            maps:get(max_execution_steps, Options, 100000)
        ),
        max_logged_events => maps:get(
            max_logged_events,
            Wave,
            maps:get(max_logged_events, Options, 10000)
        ),
        log_level => maps:get(
            log_level,
            Wave,
            maps:get(log_level, Options, full)
        ),
        attacker => Attacker,
        defender => maps:get(defender, Wave),
        initial_conditions => InitialConditions
    },
    case safe_simulate(Adapter, Request) of
        {ok, Result = #{winner := attacker}} ->
            NextCarryover = gamebattle:carryover(attacker, Result),
            run_waves(
                Adapter,
                Attacker,
                Remaining,
                Options,
                NextCarryover,
                Index + 1,
                Total,
                [Result | Results]
            );
        {ok, Result = #{winner := defender}} ->
            FinalCarryover = gamebattle:carryover(attacker, Result),
            {ok, summary(defeated, defender, Index - 1, Index, FinalCarryover,
                         [Result | Results], Total)};
        {ok, Result = #{winner := draw}} ->
            FinalCarryover = gamebattle:carryover(attacker, Result),
            {ok, summary(draw, draw, Index - 1, Index, FinalCarryover,
                         [Result | Results], Total)};
        {ok, Unexpected} ->
            execution_error(Index, Wave, {invalid_battle_result, Unexpected},
                            Carryover, Results);
        {error, Reason} ->
            execution_error(Index, Wave, Reason, Carryover, Results)
    end.

-spec safe_simulate(adapter(), map()) -> {ok, map()} | {error, term()}.
safe_simulate(Adapter, Request) ->
    try gamebattle:simulate(Adapter, Request) of
        Response -> Response
    catch
        Class:Reason ->
            {error, #{type => adapter_exception, class => Class, reason => Reason}}
    end.

-spec execution_error(pos_integer(), wave(), term(), map(), [map()]) -> {error, map()}.
execution_error(Index, Wave, Reason, Carryover, Results) ->
    {error, #{
        type => wave_execution_failed,
        wave_index => Index,
        battle_id => maps:get(battle_id, Wave),
        reason => Reason,
        completed_waves => Index - 1,
        wave_results => lists:reverse(Results),
        carryover => Carryover
    }}.

-spec summary(atom(), atom(), non_neg_integer(), non_neg_integer(), map(), [map()]) -> map().
summary(Status, Winner, Completed, StoppedAt, Carryover, Results) ->
    summary(Status, Winner, Completed, StoppedAt, Carryover, Results, Completed).

-spec summary(atom(), atom(), non_neg_integer(), non_neg_integer(), map(), [map()], non_neg_integer()) -> map().
summary(Status, Winner, Completed, StoppedAt, Carryover, Results, Total) ->
    OrderedResults = lists:reverse(Results),
    #{
        status => Status,
        winner => Winner,
        completed_waves => Completed,
        fought_waves => length(OrderedResults),
        total_waves => Total,
        stopped_at_wave => StoppedAt,
        wave_results => OrderedResults,
        carryover => Carryover
    }.

-spec initial_conditions(map()) -> {ok, map()} | {error, term()}.
initial_conditions(Options) ->
    case maps:get(initial_conditions, Options, #{}) of
        Conditions when is_map(Conditions) ->
            UnitStates = maps:get(unit_states, Conditions, []),
            FirstSide = maps:get(first_side, Conditions, automatic),
            SourceBattleId = maps:get(source_battle_id, Conditions, 0),
            case is_list(UnitStates) andalso is_integer(SourceBattleId) andalso
                 SourceBattleId >= 0 andalso
                 (FirstSide =:= automatic orelse FirstSide =:= attacker orelse
                  FirstSide =:= defender) of
                true ->
                    {ok, #{
                        source_battle_id => SourceBattleId,
                        first_side => FirstSide,
                        unit_states => UnitStates
                    }};
                false ->
                    {error, invalid_initial_conditions}
            end;
        _ ->
            {error, initial_conditions_must_be_a_map}
    end.

-spec validate_waves([term()], pos_integer(), map()) -> ok | {error, map()}.
validate_waves([], _Index, _SeenIds) ->
    ok;
validate_waves([Wave | Remaining], Index, SeenIds) when is_map(Wave) ->
    BattleId = maps:get(battle_id, Wave, invalid),
    Seed = maps:get(seed, Wave, invalid),
    Defender = maps:get(defender, Wave, invalid),
    FirstSide = maps:get(first_side, Wave, automatic),
    MaxRounds = maps:get(max_rounds, Wave, 50),
    MaxExecutionSteps = maps:get(max_execution_steps, Wave, 100000),
    MaxLoggedEvents = maps:get(max_logged_events, Wave, 10000),
    LogLevel = maps:get(log_level, Wave, full),
    Valid = is_integer(BattleId) andalso BattleId >= 0 andalso
            is_integer(Seed) andalso Seed >= 0 andalso
            is_map(Defender) andalso
            (FirstSide =:= automatic orelse FirstSide =:= attacker orelse
             FirstSide =:= defender) andalso
            is_integer(MaxRounds) andalso MaxRounds > 0 andalso
            is_integer(MaxExecutionSteps) andalso
            MaxExecutionSteps >= 100 andalso MaxExecutionSteps =< 10000000 andalso
            is_integer(MaxLoggedEvents) andalso
            MaxLoggedEvents >= 0 andalso MaxLoggedEvents =< 1000000 andalso
            (LogLevel =:= result_only orelse LogLevel =:= summary orelse
             LogLevel =:= full),
    case Valid of
        false ->
            {error, #{type => invalid_wave, wave_index => Index}};
        true ->
            case maps:is_key(BattleId, SeenIds) of
                true ->
                    {error, #{
                        type => duplicate_battle_id,
                        wave_index => Index,
                        battle_id => BattleId
                    }};
                false ->
                    validate_waves(Remaining, Index + 1, SeenIds#{BattleId => true})
            end
    end;
validate_waves([_ | _], Index, _SeenIds) ->
    {error, #{type => invalid_wave, wave_index => Index}}.
