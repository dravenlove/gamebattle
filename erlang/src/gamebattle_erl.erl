-module(gamebattle_erl).

%% The battle engine written in plain Erlang. It is a line-by-line port of the
%% C++ engine (src/engine.cpp, battle_state.cpp, target_selector.cpp and
%% effect_system.cpp): the same request gives the same result, event for
%% event, as gamebattle:simulate(port | nif, Request). It runs in the calling
%% process. gamebattle_erl_request decodes and validates requests;
%% gamebattle_erl_config loads .gbcfg packs.

-include("gamebattle_erl.hrl").

-export([simulate/1, simulate/2, load_config/1]).

-define(CONFIG_KEY, {?MODULE, config}).
-define(MASK64, 16#FFFFFFFFFFFFFFFF).

-record(active, {
    def :: #buff{},
    instance :: pos_integer(),
    remaining :: integer(),
    stacks = 1 :: integer(),
    source :: integer(),
    counts = #{} :: #{pos_integer() => non_neg_integer()}
}).

-record(unit, {
    id :: integer(),
    kind :: atom(),
    position :: integer(),
    can_act :: boolean(),
    targetable :: boolean(),
    side :: attacker | defender,
    max_hp :: integer(),
    base :: #stats{},
    skills :: [#skill{}],
    by_trigger :: #{atom() => [#passive{}]},
    initial_hp :: integer(),
    hp :: integer(),
    buffs = [] :: [#active{}],
    counts = #{} :: #{integer() => non_neg_integer()},
    cached :: #stats{},
    dirty = false :: boolean()
}).

-record(link, {
    source :: pos_integer(),
    source_id :: integer(),
    effects :: [#effect{}],
    answered :: pos_integer() | none,
    negated = false :: boolean()
}).

-record(st, {
    units :: tuple(),
    index :: #{integer() => pos_integer()},
    rng :: non_neg_integer(),
    events = [] :: [map()],
    event_count = 0 :: non_neg_integer(),
    max_events :: integer(),
    max_rounds :: integer(),
    event_limit = false :: boolean(),
    round = 0 :: integer(),
    phase = battle :: atom(),
    first_side = attacker :: attacker | defender,
    decided = false :: boolean(),
    winner = draw :: attacker | defender | draw,
    reason = undefined :: atom(),
    rounds = 0 :: integer(),
    next_buff_id = 1 :: pos_integer(),
    responses_possible = false :: boolean(),
    chain = #{} :: #{pos_integer() => #link{}},
    chain_size = 0 :: non_neg_integer(),
    resolving = undefined :: pos_integer() | undefined
}).

-type side() :: attacker | defender.
-type target_unit() :: pos_integer() | none.

%%% API ------------------------------------------------------------------------

%% Uses the config pack loaded with load_config/1, if any.
-spec simulate(map()) -> {ok, map()} | {error, map()}.
simulate(Request) ->
    simulate(Request, persistent_term:get(?CONFIG_KEY, undefined)).

-spec simulate(map(), #config{} | undefined) -> {ok, map()} | {error, map()}.
simulate(Request, Config) ->
    try
        Parsed = gamebattle_erl_request:parse(Request, Config),
        ok = gamebattle_erl_request:validate(Parsed),
        {ok, run(Parsed)}
    catch
        throw:{gamebattle_erl_invalid, Message} ->
            {error, #{type => invalid_request, message => Message}}
    end.

%% Loads a .gbcfg pack for this node's Erlang engine. The Port and the NIF
%% keep their own copies; load each one you use.
-spec load_config(file:filename_all()) -> {ok, map()} | {error, map()}.
load_config(Path) ->
    case gamebattle_erl_config:load(Path) of
        {ok, Config} ->
            persistent_term:put(?CONFIG_KEY, Config),
            {ok, #{format_major => 2, format_minor => 0,
                   buffs => Config#config.buffs, effects => Config#config.effects,
                   skills => map_size(Config#config.skills),
                   passives => map_size(Config#config.passives)}};
        {error, Message} ->
            {error, #{type => config_load_failed, message => Message}}
    end.

%%% BattleRunner (src/engine.cpp) ----------------------------------------------

-spec run(#request{}) -> map().
run(#request{} = Request) ->
    St0 = init_state(Request),
    AttackerInitiative = initiative(attacker, Request#request.attacker_bonus, St0),
    DefenderInitiative = initiative(defender, Request#request.defender_bonus, St0),
    {First, St1} =
        case Request#request.forced_first_side of
            undefined when AttackerInitiative =:= DefenderInitiative ->
                {Value, S} = next(St0),
                {case Value band 1 of 0 -> attacker; 1 -> defender end, S};
            undefined when AttackerInitiative > DefenderInitiative -> {attacker, St0};
            undefined -> {defender, St0};
            Forced -> {Forced, St0}
        end,
    FirstInitiative = case First of
                          attacker -> AttackerInitiative;
                          defender -> DefenderInitiative
                      end,
    St2 = emit(battle, initiative, First, 0, 0, 0, FirstInitiative,
               St1#st{first_side = First}),
    Final =
        case finish_if_decided(initial_state, St2) of
            {true, St3} -> St3;
            {false, St3} ->
                St4 = trigger_all(battle_start, none, 0, 0, St3),
                case finish_if_decided(battle_start, St4) of
                    {true, St5} -> St5;
                    {false, St5} -> undecided(rounds(St5#st{round = 1}))
                end
        end,
    finish(Final, Request, AttackerInitiative, DefenderInitiative).

-spec rounds(#st{}) -> #st{}.
rounds(#st{round = Round, max_rounds = MaxRounds} = St) when Round > MaxRounds -> St;
rounds(#st{event_limit = true} = St) -> St;
rounds(St0) ->
    St1 = trigger_all(round_start, none, 0, 0, (reset_round_trigger_counts(St0))#st{phase = round_start}),
    case finish_if_decided(round_start, St1) of
        {true, St2} -> St2;
        {false, St2} ->
            First = St2#st.first_side,
            St3 = sides([First, other(First)], St2),
            case St3#st.decided orelse St3#st.event_limit of
                true -> St3;
                false ->
                    St4 = trigger_all(round_end, none, 0, 0, St3#st{phase = round_end}),
                    case finish_if_decided(round_end, St4) of
                        {true, St5} -> St5;
                        {false, St5} -> rounds(St5#st{round = St5#st.round + 1})
                    end
            end
    end.

-spec undecided(#st{}) -> #st{}.
undecided(#st{decided = true} = St) -> St;
undecided(#st{event_limit = Limit, round = Round, max_rounds = MaxRounds} = St) ->
    St#st{winner = draw,
          reason = case Limit of true -> event_limit; false -> max_rounds end,
          rounds = min(Round, MaxRounds)}.

-spec sides([side()], #st{}) -> #st{}.
sides([], St) -> St;
sides([Side | Rest], St0) ->
    case side_defeated(Side, St0) orelse St0#st.event_limit of
        true -> sides(Rest, St0);
        false ->
            Phase = case Side =:= St0#st.first_side of
                        true -> first_side;
                        false -> second_side
                    end,
            St1 = take_side_turn(Side, St0#st{phase = Phase}),
            case finish_if_decided(all_units_defeated, St1) of
                {true, St2} -> St2;
                {false, St2} -> sides(Rest, St2)
            end
    end.

%% The order is a snapshot: speed changes apply from the next side turn.
-spec take_side_turn(side(), #st{}) -> #st{}.
take_side_turn(Side, St0) ->
    {Order, St1} = acting_order(Side, St0),
    act(Order, Side, St1).

-spec act([pos_integer()], side(), #st{}) -> #st{}.
act([], _Side, St) -> St;
act([Actor | Rest], Side, St0) ->
    case St0#st.event_limit orelse side_defeated(other(Side), St0) of
        true -> St0;
        false ->
            case alive(Actor, St0) of
                false -> act(Rest, Side, St0);
                true ->
                    St1 = trigger_owner(Actor, before_action, Actor, 0, 0, St0),
                    case alive(Actor, St1) of
                        false -> act(Rest, Side, St1);
                        true ->
                            Id = (unit(Actor, St1))#unit.id,
                            St2 = emit(St1#st.phase, action_start, Side, Id, 0, 0, 0, St1),
                            St3 = execute_action(Actor, St2),
                            St4 = case alive(Actor, St3) of
                                      true -> trigger_owner(Actor, after_action, Actor, 0, 0, St3);
                                      false -> St3
                                  end,
                            act(Rest, Side, emit(St4#st.phase, action_end, Side, Id, 0, 0, 0, St4))
                    end
            end
    end.

%%% BattleState (src/battle_state.cpp) -----------------------------------------

-spec init_state(#request{}) -> #st{}.
init_state(#request{attacker = Attacker, defender = Defender, unit_states = States} = Request) ->
    Configs = [{attacker, Config} || Config <- Attacker] ++
              [{defender, Config} || Config <- Defender],
    Units0 = [make_unit(Config, Side) || {Side, Config} <- Configs],
    Index = maps:from_list(lists:zip([U#unit.id || U <- Units0], lists:seq(1, length(Units0)))),
    Units = lists:foldl(
              fun({UnitId, CurrentHp}, Acc) ->
                  Position = maps:get(UnitId, Index),
                  U = element(Position, Acc),
                  setelement(Position, Acc, U#unit{initial_hp = CurrentHp, hp = CurrentHp})
              end,
              list_to_tuple(Units0), States),
    ResponsesPossible =
        lists:any(fun(#unit{by_trigger = ByTrigger}) ->
                      maps:is_key(enemy_activate, ByTrigger) orelse
                          maps:is_key(ally_activate, ByTrigger)
                  end,
                  Units0),
    #st{units = Units, index = Index, rng = Request#request.seed,
        max_events = Request#request.max_events, max_rounds = Request#request.max_rounds,
        responses_possible = ResponsesPossible}.

-spec make_unit(#unit_config{}, side()) -> #unit{}.
make_unit(#unit_config{id = Id, kind = Kind, position = Position, can_act = CanAct,
                       targetable = Targetable, stats = Stats, skills = Skills,
                       passives = Passives},
          Side) ->
    %% Stable sort by descending priority.
    Sorted = [Skill || {_, Skill} <- lists:keysort(1, [{-S#skill.priority, S} || S <- Skills])],
    ByTrigger = lists:foldr(
                  fun(#passive{trigger = Trigger} = Passive, Acc) ->
                      Acc#{Trigger => [Passive | maps:get(Trigger, Acc, [])]}
                  end,
                  #{}, Passives),
    #unit{id = Id, kind = Kind, position = Position, can_act = CanAct,
          targetable = Targetable, side = Side, max_hp = Stats#stats.hp, base = Stats,
          skills = Sorted, by_trigger = ByTrigger, initial_hp = Stats#stats.hp,
          hp = Stats#stats.hp, cached = Stats}.

-spec unit(pos_integer(), #st{}) -> #unit{}.
unit(Index, #st{units = Units}) -> element(Index, Units).

-spec set_unit(pos_integer(), #unit{}, #st{}) -> #st{}.
set_unit(Index, Unit, #st{units = Units} = St) -> St#st{units = setelement(Index, Units, Unit)}.

-spec alive(pos_integer(), #st{}) -> boolean().
alive(Index, St) -> (unit(Index, St))#unit.hp > 0.

-spec other(side()) -> side().
other(attacker) -> defender;
other(defender) -> attacker.

-spec effective_stats(pos_integer(), #st{}) -> {#stats{}, #st{}}.
effective_stats(Index, St) ->
    case unit(Index, St) of
        #unit{dirty = false, cached = Cached} -> {Cached, St};
        #unit{base = Base, buffs = Buffs} = U ->
            Stats = compute_stats(Base, Buffs),
            {Stats, set_unit(Index, U#unit{cached = Stats, dirty = false}, St)}
    end.

%% An attribute without modifiers keeps its base value: scale(V, 10000) is V
%% and base values are already within bounds.
-spec compute_stats(#stats{}, [#active{}]) -> #stats{}.
compute_stats(Base, []) -> Base;
compute_stats(Base, Buffs) ->
    {Additions, Deltas} =
        lists:foldl(
          fun(#active{def = #buff{modifiers = Modifiers}, stacks = Stacks}, Acc0) ->
              lists:foldl(
                fun({Attribute, Operation, Value}, {Add, Scale}) ->
                    Amount = sat_mul(Value, Stacks),
                    case Operation of
                        add -> {Add#{Attribute => sat_add(maps:get(Attribute, Add, 0), Amount)}, Scale};
                        scale_bp -> {Add, Scale#{Attribute => sat_add(maps:get(Attribute, Scale, 0), Amount)}}
                    end
                end,
                Acc0, Modifiers)
          end,
          {#{}, #{}}, Buffs),
    Attributes = maps:keys(maps:merge(Additions, Deltas)),
    lists:foldl(
      fun(Attribute, Stats) ->
          AfterAdd = sat_add(attribute(Attribute, Base), maps:get(Attribute, Additions, 0)),
          AfterScale = scale(AfterAdd, sat_add(?BASIS_POINTS, maps:get(Attribute, Deltas, 0))),
          set_attribute(Attribute, AfterScale, Stats)
      end,
      Base, Attributes).

-spec attribute(atom(), #stats{}) -> integer().
attribute(attack, S) -> S#stats.attack;
attribute(defense, S) -> S#stats.defense;
attribute(speed, S) -> S#stats.speed;
attribute(crit_rate_bp, S) -> S#stats.crit_rate_bp;
attribute(crit_damage_bp, S) -> S#stats.crit_damage_bp;
attribute(hit_rate_bp, S) -> S#stats.hit_rate_bp;
attribute(dodge_rate_bp, S) -> S#stats.dodge_rate_bp;
attribute(damage_bonus_bp, S) -> S#stats.damage_bonus_bp;
attribute(damage_reduction_bp, S) -> S#stats.damage_reduction_bp.

-spec set_attribute(atom(), integer(), #stats{}) -> #stats{}.
set_attribute(attack, V, S) -> S#stats{attack = max(0, V)};
set_attribute(defense, V, S) -> S#stats{defense = max(0, V)};
set_attribute(speed, V, S) -> S#stats{speed = max(0, V)};
set_attribute(crit_rate_bp, V, S) -> S#stats{crit_rate_bp = clamp32(V)};
set_attribute(crit_damage_bp, V, S) -> S#stats{crit_damage_bp = clamp32(V)};
set_attribute(hit_rate_bp, V, S) -> S#stats{hit_rate_bp = clamp32(V)};
set_attribute(dodge_rate_bp, V, S) -> S#stats{dodge_rate_bp = clamp32(V)};
set_attribute(damage_bonus_bp, V, S) -> S#stats{damage_bonus_bp = clamp32(V)};
set_attribute(damage_reduction_bp, V, S) -> S#stats{damage_reduction_bp = clamp32(V)}.

-spec initiative(side(), integer(), #st{}) -> non_neg_integer().
initiative(Side, Bonus, #st{units = Units}) ->
    Total = lists:foldl(
              fun(#unit{side = S, hp = Hp, can_act = true, base = Base}, Acc)
                    when S =:= Side, Hp > 0 ->
                      sat_add(Acc, Base#stats.speed);
                 (_, Acc) -> Acc
              end,
              Bonus, tuple_to_list(Units)),
    max(0, Total).

-spec acting_order(side(), #st{}) -> {[pos_integer()], #st{}}.
acting_order(Side, #st{units = Units} = St) ->
    sort_by_speed([I || I <- lists:seq(1, tuple_size(Units)),
                        #unit{side = S, hp = Hp, can_act = true} <- [element(I, Units)],
                        S =:= Side, Hp > 0],
                  St).

%% Living units of one side, including ones that cannot act.
-spec response_order(side(), #st{}) -> {[pos_integer()], #st{}}.
response_order(Side, #st{units = Units} = St) ->
    sort_by_speed([I || I <- lists:seq(1, tuple_size(Units)),
                        #unit{side = S, hp = Hp} <- [element(I, Units)],
                        S =:= Side, Hp > 0],
                  St).

-spec sort_by_speed([pos_integer()], #st{}) -> {[pos_integer()], #st{}}.
sort_by_speed(Indexes, St0) ->
    {Keyed, St} = lists:mapfoldl(
                    fun(I, StAcc0) ->
                        {#stats{speed = Speed}, StAcc} = effective_stats(I, StAcc0),
                        #unit{position = Position, id = Id} = unit(I, StAcc),
                        {{-Speed, Position, Id, I}, StAcc}
                    end,
                    St0, Indexes),
    {[I || {_, _, _, I} <- lists:sort(Keyed)], St}.

%% A side with heroes is defeated when they are all dead; a side without
%% heroes when nothing on it can act any more.
-spec side_defeated(side(), #st{}) -> boolean().
side_defeated(Side, #st{units = Units}) ->
    side_defeated(Side, Units, tuple_size(Units), false, false, false).

-spec side_defeated(side(), tuple(), non_neg_integer(), boolean(), boolean(), boolean()) ->
          boolean().
side_defeated(_, _, 0, true, LivingHero, _) -> not LivingHero;
side_defeated(_, _, 0, false, _, LivingActor) -> not LivingActor;
side_defeated(Side, Units, I, HasHero, LivingHero, LivingActor) ->
    case element(I, Units) of
        #unit{side = Side, kind = Kind, hp = Hp, can_act = CanAct} ->
            Alive = Hp > 0,
            Actor = LivingActor orelse (Alive andalso CanAct),
            case Kind of
                hero -> side_defeated(Side, Units, I - 1, true, LivingHero orelse Alive, Actor);
                _ -> side_defeated(Side, Units, I - 1, HasHero, LivingHero, Actor)
            end;
        _ ->
            side_defeated(Side, Units, I - 1, HasHero, LivingHero, LivingActor)
    end.

-spec finish_if_decided(atom(), #st{}) -> {boolean(), #st{}}.
finish_if_decided(Reason, St) ->
    AttackerDead = side_defeated(attacker, St),
    DefenderDead = side_defeated(defender, St),
    case AttackerDead orelse DefenderDead of
        false -> {false, St};
        true ->
            Winner = if
                         AttackerDead =:= DefenderDead -> draw;
                         DefenderDead -> attacker;
                         true -> defender
                     end,
            {true, St#st{decided = true, winner = Winner, reason = Reason,
                         rounds = St#st.round}}
    end.

-spec reset_round_trigger_counts(#st{}) -> #st{}.
reset_round_trigger_counts(#st{units = Units} = St) ->
    St#st{units = list_to_tuple(
                    [U#unit{counts = #{},
                            buffs = [A#active{counts = #{}} || A <- Buffs]}
                     || #unit{buffs = Buffs} = U <- tuple_to_list(Units)])}.

-spec emit(atom(), atom(), side(), integer(), integer(), integer(), integer(), #st{}) -> #st{}.
emit(Phase, Type, Side, Actor, Target, SourceId, Value, St) ->
    emit(Phase, Type, Side, Actor, Target, SourceId, Value, 0, 0, false, St).

-spec emit(atom(), atom(), side(), integer(), integer(), integer(), integer(),
           integer(), integer(), boolean(), #st{}) -> #st{}.
emit(_, _, _, _, _, _, _, _, _, _, #st{event_count = Count, max_events = Max} = St)
        when Count >= Max ->
    St#st{event_limit = true};
emit(Phase, Type, Side, Actor, Target, SourceId, Value, HpBefore, HpAfter, Critical,
     #st{event_count = Count, events = Events, round = Round} = St) ->
    Event = #{seq => Count + 1, round => Round, phase => Phase, type => Type,
              side => Side, actor => Actor, target => Target, source_id => SourceId,
              value => Value, hp_before => HpBefore, hp_after => HpAfter,
              critical => Critical},
    St#st{events = [Event | Events], event_count = Count + 1}.

-spec finish(#st{}, #request{}, non_neg_integer(), non_neg_integer()) -> map().
finish(#st{} = St, #request{} = Request, AttackerInitiative, DefenderInitiative) ->
    Summary = #{battle_id => Request#request.battle_id,
                seed => Request#request.seed,
                source_battle_id => Request#request.source_battle_id,
                winner => St#st.winner,
                reason => St#st.reason,
                rounds => St#st.rounds,
                attacker_initiative => AttackerInitiative,
                defender_initiative => DefenderInitiative,
                units => [#{id => Id, side => Side, initial_hp => InitialHp, hp => Hp,
                            max_hp => MaxHp, alive => Hp > 0}
                          || #unit{id = Id, side = Side, initial_hp = InitialHp, hp = Hp,
                                   max_hp = MaxHp} <- tuple_to_list(St#st.units)]},
    Full = Summary#{events => lists:reverse(St#st.events)},
    %% wire::encode_compact_result: the client report replaces the event maps.
    case Request#request.report of
        none -> Full;
        Detail -> Summary#{report => gamebattle_report:encode(Full, Detail)}
    end.

%%% Arithmetic and Random (src/battle_state.cpp) --------------------------------

-spec sat_add(integer(), integer()) -> integer().
sat_add(Left, Right) -> clamp64(Left + Right).

-spec sat_mul(integer(), integer()) -> integer().
sat_mul(Left, Right) -> clamp64(Left * Right).

-spec scale(integer(), integer()) -> integer().
scale(Value, BasisPoints) ->
    sat_add(sat_mul(Value div ?BASIS_POINTS, BasisPoints),
            sat_mul(Value rem ?BASIS_POINTS, BasisPoints) div ?BASIS_POINTS).

-spec clamp64(integer()) -> integer().
clamp64(V) when V > ?INT64_MAX -> ?INT64_MAX;
clamp64(V) when V < ?INT64_MIN -> ?INT64_MIN;
clamp64(V) -> V.

-spec clamp32(integer()) -> integer().
clamp32(V) when V > ?INT32_MAX -> ?INT32_MAX;
clamp32(V) when V < ?INT32_MIN -> ?INT32_MIN;
clamp32(V) -> V.

%% splitmix64, as Random::next.
-spec next(#st{}) -> {non_neg_integer(), #st{}}.
next(#st{rng = State0} = St) ->
    State = (State0 + 16#9E3779B97F4A7C15) band ?MASK64,
    Z1 = ((State bxor (State bsr 30)) * 16#BF58476D1CE4E5B9) band ?MASK64,
    Z2 = ((Z1 bxor (Z1 bsr 27)) * 16#94D049BB133111EB) band ?MASK64,
    {Z2 bxor (Z2 bsr 31), St#st{rng = State}}.

-spec roll(integer(), #st{}) -> {boolean(), #st{}}.
roll(Chance, St) when Chance =< 0 -> {false, St};
roll(Chance, St) when Chance >= ?BASIS_POINTS -> {true, St};
roll(Chance, St0) ->
    {Value, St} = next(St0),
    {Value rem ?BASIS_POINTS < Chance, St}.

%%% TargetSelector (src/target_selector.cpp) -----------------------------------

-spec select_targets(pos_integer(), atom(), integer(), target_unit(), #st{}) -> [pos_integer()].
select_targets(Owner, self, _, _, St) ->
    case alive(Owner, St) of
        true -> [Owner];
        false -> []
    end;
select_targets(_, trigger_unit, _, none, _) -> [];
select_targets(_, trigger_unit, _, TriggerUnit, St) ->
    case unit(TriggerUnit, St) of
        #unit{hp = Hp, targetable = true} when Hp > 0 -> [TriggerUnit];
        _ -> []
    end;
select_targets(Owner, Rule, Count, _, #st{units = Units} = St) ->
    OwnerSide = (unit(Owner, St))#unit.side,
    TargetSide = case Rule of
                     ally_lowest_hp -> OwnerSide;
                     all_allies -> OwnerSide;
                     _ -> other(OwnerSide)
                 end,
    Sorted = [I || {_, I} <- lists:sort(candidates(Rule, TargetSide, Units, tuple_size(Units), []))],
    case Rule of
        all_enemies -> Sorted;
        all_allies -> Sorted;
        _ -> lists:sublist(Sorted, max(0, Count))
    end.

-spec candidates(atom(), side(), tuple(), non_neg_integer(), [{tuple(), pos_integer()}]) ->
          [{tuple(), pos_integer()}].
candidates(_, _, _, 0, Acc) -> Acc;
candidates(Rule, Side, Units, I, Acc) ->
    case element(I, Units) of
        #unit{side = Side, hp = Hp, targetable = true} = U when Hp > 0 ->
            candidates(Rule, Side, Units, I - 1, [{sort_key(Rule, U), I} | Acc]);
        _ ->
            candidates(Rule, Side, Units, I - 1, Acc)
    end.

-spec sort_key(atom(), #unit{}) -> tuple().
sort_key(Rule, #unit{hp = Hp, max_hp = MaxHp, position = Position, id = Id})
        when Rule =:= enemy_lowest_hp; Rule =:= ally_lowest_hp ->
    Ratio = (Hp div MaxHp) * ?BASIS_POINTS + ((Hp rem MaxHp) * ?BASIS_POINTS) div MaxHp,
    {Ratio, Position, Id};
sort_key(_, #unit{position = Position, id = Id}) ->
    {Position, Id}.

%%% EffectSystem (src/effect_system.cpp) ---------------------------------------

-spec execute_action(pos_integer(), #st{}) -> #st{}.
execute_action(Actor, St0) ->
    #unit{skills = Skills, side = Side, id = Id} = unit(Actor, St0),
    {Selected, St1} = select_skill(Skills, St0),
    {SkillId, Effects, Basic} = case Selected of
                                    none -> {0, [#effect{}], true};
                                    #skill{id = S, effects = E} -> {S, E, false}
                                end,
    St2 = emit(St1#st.phase, skill, Side, Id, 0, SkillId, 0, St1),
    St3 = trigger_owner(Actor, on_attack, Actor, SkillId, 0, St2),
    %% Only active skills open a chain; the basic attack resolves immediately.
    case Basic orelse not St3#st.responses_possible of
        true -> execute_effects(Actor, Actor, Effects, SkillId, 0, none, 1, St3);
        false -> run_chain(Actor, SkillId, Effects, St3)
    end.

-spec select_skill([#skill{}], #st{}) -> {#skill{} | none, #st{}}.
select_skill([], St) -> {none, St};
select_skill([#skill{chance_bp = Chance} = Skill | Rest], St0) ->
    case roll(Chance, St0) of
        {true, St1} -> {Skill, St1};
        {false, St1} -> select_skill(Rest, St1)
    end.

-spec run_chain(pos_integer(), integer(), [#effect{}], #st{}) -> #st{}.
run_chain(Actor, SkillId, Effects, St0) ->
    First = #link{source = Actor, source_id = SkillId, effects = Effects, answered = none},
    St1 = build_chain(St0#st{chain = #{1 => First}, chain_size = 1}),
    St2 = resolve_chain(St1#st.chain_size, St1),
    St2#st{chain = #{}, chain_size = 0, resolving = undefined}.

-spec build_chain(#st{}) -> #st{}.
build_chain(#st{event_limit = true} = St) -> St;
build_chain(St0) ->
    case add_response(St0) of
        {true, St1} -> build_chain(St1);
        {false, St1} -> St1
    end.

-spec resolve_chain(non_neg_integer(), #st{}) -> #st{}.
resolve_chain(0, St) -> St;
resolve_chain(_, #st{event_limit = true} = St) -> St;
resolve_chain(Number, #st{chain = Chain} = St0) ->
    case maps:get(Number, Chain) of
        #link{negated = true} ->
            resolve_chain(Number - 1, St0);
        #link{source = Source, source_id = SourceId, effects = Effects, answered = Answered} ->
            case St0#st.chain_size > 1 andalso not alive(Source, St0) of
                true ->
                    %% A response killed this link's owner before it could resolve.
                    #unit{side = Side, id = Id} = unit(Source, St0),
                    resolve_chain(Number - 1,
                                  emit(St0#st.phase, fizzle, Side, Id, 0, SourceId, Number, St0));
                false ->
                    St1 = execute_effects(Source, Source, Effects, SourceId, 0, Answered, 1,
                                          St0#st{resolving = Number}),
                    resolve_chain(Number - 1, St1)
            end
    end.

%% Offers the top link to the other side first, then to its allies. Each unit
%% adds at most one link per chain.
-spec add_response(#st{}) -> {boolean(), #st{}}.
add_response(#st{chain = Chain, chain_size = Size} = St) ->
    #link{source = Answered} = maps:get(Size, Chain),
    AnsweredSide = (unit(Answered, St))#unit.side,
    respond([{other(AnsweredSide), enemy_activate}, {AnsweredSide, ally_activate}],
            Answered, St).

-spec respond([{side(), atom()}], pos_integer(), #st{}) -> {boolean(), #st{}}.
respond([], _, St) -> {false, St};
respond([{Side, Trigger} | Rest], Answered, St0) ->
    {Order, St1} = response_order(Side, St0),
    case respond_units(Order, Trigger, Answered, St1) of
        {true, St2} -> {true, St2};
        {false, St2} -> respond(Rest, Answered, St2)
    end.

-spec respond_units([pos_integer()], atom(), pos_integer(), #st{}) -> {boolean(), #st{}}.
respond_units([], _, _, St) -> {false, St};
respond_units([Unit | Rest], Trigger, Answered, St0) ->
    InChain = lists:any(fun(#link{source = S}) -> S =:= Unit end, maps:values(St0#st.chain)),
    case InChain of
        true -> respond_units(Rest, Trigger, Answered, St0);
        false ->
            Passives = maps:get(Trigger, (unit(Unit, St0))#unit.by_trigger, []),
            case respond_passives(Passives, Unit, Answered, St0) of
                {true, St1} -> {true, St1};
                {false, St1} -> respond_units(Rest, Trigger, Answered, St1)
            end
    end.

-spec respond_passives([#passive{}], pos_integer(), pos_integer(), #st{}) -> {boolean(), #st{}}.
respond_passives([], _, _, St) -> {false, St};
respond_passives([#passive{id = PassiveId, chance_bp = Chance, effects = Effects} = Passive | Rest],
                 Unit, Answered, St0) ->
    case roll(Chance, St0) of
        {false, St1} -> respond_passives(Rest, Unit, Answered, St1);
        {true, St1} ->
            case count_passive(Unit, Passive, St1) of
                {false, St2} -> respond_passives(Rest, Unit, Answered, St2);
                {true, St2} ->
                    Number = St2#st.chain_size + 1,
                    Link = #link{source = Unit, source_id = PassiveId, effects = Effects,
                                 answered = Answered},
                    St3 = St2#st{chain = (St2#st.chain)#{Number => Link}, chain_size = Number},
                    #unit{side = Side, id = Id} = unit(Unit, St3),
                    AnsweredId = (unit(Answered, St3))#unit.id,
                    {true, emit(St3#st.phase, chain, Side, Id, AnsweredId, PassiveId, Number, St3)}
            end
    end.

-spec negate_answered_link(pos_integer(), #st{}) -> #st{}.
negate_answered_link(_, #st{resolving = undefined} = St) -> St;
negate_answered_link(_, #st{resolving = 1} = St) -> St;
negate_answered_link(Source, #st{resolving = Resolving, chain = Chain} = St0) ->
    Number = Resolving - 1,
    case maps:get(Number, Chain) of
        #link{negated = true} -> St0;
        #link{source = AnsweredSource, source_id = AnsweredId} = Answered ->
            St1 = St0#st{chain = Chain#{Number => Answered#link{negated = true}}},
            #unit{side = Side, id = Id} = unit(Source, St1),
            Target = (unit(AnsweredSource, St1))#unit.id,
            emit(St1#st.phase, negate, Side, Id, Target, AnsweredId, Number, St1)
    end.

-spec count_passive(pos_integer(), #passive{}, #st{}) -> {boolean(), #st{}}.
count_passive(Unit, #passive{id = Id, max_triggers_per_round = Max}, St) ->
    #unit{counts = Counts} = U = unit(Unit, St),
    Count = maps:get(Id, Counts, 0),
    case Max > 0 andalso Count >= Max of
        true -> {false, St};
        false -> {true, set_unit(Unit, U#unit{counts = Counts#{Id => Count + 1}}, St)}
    end.

-spec execute_effects(pos_integer(), pos_integer(), [#effect{}], integer(),
                      non_neg_integer(), target_unit(), integer(), #st{}) -> #st{}.
execute_effects(_, _, _, _, Depth, _, _, St) when Depth > ?MAX_TRIGGER_DEPTH -> St;
execute_effects(_, _, _, _, _, _, _, #st{event_limit = true} = St) -> St;
execute_effects(Source, Owner, Effects, SourceId, Depth, TriggerUnit, Stacks, St) ->
    effects(Effects, Source, Owner, SourceId, Depth, TriggerUnit, Stacks, St).

-spec effects([#effect{}], pos_integer(), pos_integer(), integer(), non_neg_integer(),
              target_unit(), integer(), #st{}) -> #st{}.
effects([], _, _, _, _, _, _, St) -> St;
effects([#effect{kind = negate} | Rest], Source, Owner, SourceId, Depth, TriggerUnit, Stacks, St) ->
    effects(Rest, Source, Owner, SourceId, Depth, TriggerUnit, Stacks,
            negate_answered_link(Source, St));
effects([Effect0 | Rest], Source, Owner, SourceId, Depth, TriggerUnit, Stacks, St0) ->
    Effect = scale_effect(Effect0, Stacks),
    Targets = select_targets(Owner, Effect#effect.target, Effect#effect.target_count,
                             TriggerUnit, St0),
    case apply_effect(Targets, Effect, Source, SourceId, Depth, St0) of
        {stop, St1} -> St1;
        {continue, St1} -> effects(Rest, Source, Owner, SourceId, Depth, TriggerUnit, Stacks, St1)
    end.

-spec scale_effect(#effect{}, integer()) -> #effect{}.
scale_effect(#effect{kind = Kind, flat = Flat, attack_bp = AttackBp} = Effect, Stacks)
        when Stacks > 1, (Kind =:= damage orelse Kind =:= heal orelse Kind =:= direct_damage) ->
    Effect#effect{flat = sat_mul(Flat, Stacks), attack_bp = clamp32(sat_mul(AttackBp, Stacks))};
scale_effect(Effect, _) ->
    Effect.

-spec apply_effect([pos_integer()], #effect{}, pos_integer(), integer(), non_neg_integer(),
                   #st{}) -> {stop | continue, #st{}}.
apply_effect([], _, _, _, _, St) -> {continue, St};
apply_effect(_, _, _, _, _, #st{event_limit = true} = St) -> {stop, St};
apply_effect([Target | Rest], #effect{kind = Kind} = Effect, Source, SourceId, Depth, St0) ->
    St1 = case Kind of
              damage -> apply_damage(Source, Target, Effect, SourceId, Depth + 1, St0);
              direct_damage -> apply_damage(Source, Target, Effect, SourceId, Depth + 1, St0);
              heal -> apply_heal(Source, Target, Effect, SourceId, St0);
              add_buff -> apply_buff(Source, Target, Effect#effect.buff, SourceId, St0);
              remove_buff -> remove_buff(Source, Target, Effect#effect.remove_buff_id, SourceId, St0)
          end,
    apply_effect(Rest, Effect, Source, SourceId, Depth, St1).

-spec apply_damage(pos_integer(), pos_integer(), #effect{}, integer(), non_neg_integer(),
                   #st{}) -> #st{}.
apply_damage(Actor, Target, #effect{kind = Kind, attack_bp = AttackBp, flat = Flat},
             SourceId, Depth, St0) ->
    Direct = Kind =:= direct_damage,
    case (not Direct andalso not alive(Actor, St0)) orelse not alive(Target, St0) of
        true -> St0;
        false ->
            {ActorStats, St1} = effective_stats(Actor, St0),
            Damage0 = sat_add(scale(ActorStats#stats.attack, AttackBp), Flat),
            case Direct of
                true ->
                    land_damage(Actor, Target, max(1, Damage0), false, true, SourceId, Depth, St1);
                false ->
                    {TargetStats, St2} = effective_stats(Target, St1),
                    HitChance = max(0, min(?BASIS_POINTS, ActorStats#stats.hit_rate_bp -
                                                          TargetStats#stats.dodge_rate_bp)),
                    case roll(HitChance, St2) of
                        {false, St3} ->
                            #unit{side = Side, id = ActorId} = unit(Actor, St3),
                            #unit{id = TargetId, hp = Hp} = unit(Target, St3),
                            emit(St3#st.phase, miss, Side, ActorId, TargetId, SourceId, 0, Hp, Hp,
                                 false, St3);
                        {true, St3} ->
                            Damage1 = max(1, sat_add(Damage0, -TargetStats#stats.defense)),
                            Damage2 = max(1, scale(Damage1, ?BASIS_POINTS +
                                                            ActorStats#stats.damage_bonus_bp)),
                            Damage3 = max(1, scale(Damage2, max(0, ?BASIS_POINTS -
                                                                   TargetStats#stats.damage_reduction_bp))),
                            {Critical, St4} = roll(ActorStats#stats.crit_rate_bp, St3),
                            Damage = case Critical of
                                         true ->
                                             max(1, scale(Damage3, max(?BASIS_POINTS,
                                                                       ActorStats#stats.crit_damage_bp)));
                                         false -> Damage3
                                     end,
                            land_damage(Actor, Target, Damage, Critical, false, SourceId, Depth, St4)
                    end
            end
    end.

-spec land_damage(pos_integer(), pos_integer(), integer(), boolean(), boolean(), integer(),
                  non_neg_integer(), #st{}) -> #st{}.
land_damage(Actor, Target, Damage0, Critical, Direct, SourceId, Depth, St0) ->
    #unit{hp = Before, id = TargetId} = TargetUnit = unit(Target, St0),
    Damage = min(Damage0, Before),
    After = Before - Damage,
    St1 = set_unit(Target, TargetUnit#unit{hp = After}, St0),
    #unit{side = Side, id = ActorId} = unit(Actor, St1),
    Type = case Direct of
               true -> direct_damage;
               false -> damage
           end,
    St2 = emit(St1#st.phase, Type, Side, ActorId, TargetId, SourceId, Damage, Before, After,
               Critical, St1),
    St3 = case Direct of
              false -> trigger_owner(Actor, on_hit, Target, SourceId, Depth, St2);
              true -> St2
          end,
    St4 = trigger_owner(Target, on_damaged, Actor, SourceId, Depth, St3),
    case alive(Target, St4) of
        true -> St4;
        false ->
            TargetSide = (unit(Target, St4))#unit.side,
            KillerId = (unit(Actor, St4))#unit.id,
            St5 = emit(St4#st.phase, death, TargetSide, KillerId, TargetId, SourceId, 0, St4),
            trigger_all(unit_death, Target, SourceId, Depth, St5)
    end.

-spec apply_heal(pos_integer(), pos_integer(), #effect{}, integer(), #st{}) -> #st{}.
apply_heal(Actor, Target, #effect{attack_bp = AttackBp, flat = Flat}, SourceId, St0) ->
    case alive(Actor, St0) andalso alive(Target, St0) of
        false -> St0;
        true ->
            {ActorStats, St1} = effective_stats(Actor, St0),
            Amount = max(0, sat_add(scale(ActorStats#stats.attack, AttackBp), Flat)),
            #unit{hp = Before, max_hp = MaxHp, id = TargetId} = TargetUnit = unit(Target, St1),
            After = min(MaxHp, sat_add(Before, Amount)),
            St2 = set_unit(Target, TargetUnit#unit{hp = After}, St1),
            #unit{side = Side, id = ActorId} = unit(Actor, St2),
            emit(St2#st.phase, heal, Side, ActorId, TargetId, SourceId, After - Before, Before,
                 After, false, St2)
    end.

-spec apply_buff(pos_integer(), pos_integer(), #buff{} | undefined, integer(), #st{}) -> #st{}.
apply_buff(_, _, undefined, _, St) -> St;
apply_buff(_, _, #buff{id = 0}, _, St) -> St;
apply_buff(_, _, #buff{max_stacks = MaxStacks}, _, St) when MaxStacks =< 0 -> St;
apply_buff(_, _, #buff{permanent = false, duration = Duration}, _, St) when Duration =< 0 -> St;
apply_buff(Actor, Target, #buff{id = BuffId} = Definition, SourceId, St0) ->
    #unit{buffs = Buffs, id = TargetId} = TargetUnit = unit(Target, St0),
    #unit{side = ActorSide, id = ActorId} = unit(Actor, St0),
    case lists:keyfind(BuffId, 1, [{Id, A} || #active{def = #buff{id = Id}} = A <- Buffs]) of
        false when St0#st.next_buff_id =:= ?UINT64_MAX ->
            St0#st{event_limit = true};
        false ->
            Instance = St0#st.next_buff_id,
            Remaining = case Definition#buff.permanent of
                            true -> 0;
                            false -> Definition#buff.duration
                        end,
            Active = #active{def = Definition, instance = Instance, remaining = Remaining,
                             stacks = 1, source = ActorId},
            St1 = set_unit(Target, TargetUnit#unit{buffs = Buffs ++ [Active], dirty = true},
                           St0#st{next_buff_id = Instance + 1}),
            emit(St1#st.phase, buff_add, ActorSide, ActorId, TargetId,
                 source_or(SourceId, BuffId), 1, St1);
        {_, #active{def = Spec, stacks = Stacks0, remaining = Remaining0} = Existing} ->
            Stacks = case Spec#buff.mode of
                         stack -> min(Spec#buff.max_stacks, Stacks0 + 1);
                         refresh -> Stacks0
                     end,
            Remaining = case {Spec#buff.permanent, Spec#buff.refresh} of
                            {true, _} -> Remaining0;
                            {false, reset} -> Spec#buff.duration;
                            {false, extend} -> min(sat_add(Remaining0, Spec#buff.duration), ?INT32_MAX);
                            {false, keep} -> Remaining0
                        end,
            Updated = Existing#active{stacks = Stacks, remaining = Remaining, source = ActorId},
            Instance = Existing#active.instance,
            NewBuffs = [case A of
                            #active{instance = Instance} -> Updated;
                            _ -> A
                        end || A <- Buffs],
            St1 = set_unit(Target, TargetUnit#unit{buffs = NewBuffs, dirty = true}, St0),
            emit(St1#st.phase, buff_add, ActorSide, ActorId, TargetId,
                 source_or(SourceId, Spec#buff.id), Stacks, St1)
    end.

-spec source_or(integer(), integer()) -> integer().
source_or(0, Default) -> Default;
source_or(SourceId, _) -> SourceId.

-spec remove_buff(pos_integer(), pos_integer(), integer(), integer(), #st{}) -> #st{}.
remove_buff(Actor, Target, BuffId, SourceId, St0) ->
    #unit{buffs = Buffs, id = TargetId} = TargetUnit = unit(Target, St0),
    {Removed, Kept} = lists:partition(fun(#active{def = #buff{id = Id}}) -> Id =:= BuffId end,
                                      Buffs),
    case Removed of
        [] -> St0;
        _ ->
            St1 = set_unit(Target, TargetUnit#unit{buffs = Kept, dirty = true}, St0),
            #unit{side = Side, id = ActorId} = unit(Actor, St1),
            emit(St1#st.phase, buff_remove, Side, ActorId, TargetId, SourceId, length(Removed), St1)
    end.

-spec trigger_owner(pos_integer(), atom(), target_unit(), integer(), non_neg_integer(), #st{}) ->
          #st{}.
trigger_owner(Owner, Trigger, EventUnit, SourceId, Depth, St) ->
    trigger_snapshot(Owner, Trigger, EventUnit, SourceId, Depth, St#st.next_buff_id - 1, St).

-spec trigger_all(atom(), target_unit(), integer(), non_neg_integer(), #st{}) -> #st{}.
trigger_all(Trigger, EventUnit, SourceId, Depth, St) ->
    Cutoff = St#st.next_buff_id - 1,
    lists:foldl(fun(Owner, Acc) ->
                    trigger_snapshot(Owner, Trigger, EventUnit, SourceId, Depth, Cutoff, Acc)
                end,
                St, lists:seq(1, tuple_size(St#st.units))).

%% Buffs added after Cutoff (during this trigger) neither react nor tick.
-spec trigger_snapshot(pos_integer(), atom(), target_unit(), integer(), non_neg_integer(),
                       non_neg_integer(), #st{}) -> #st{}.
trigger_snapshot(_, _, _, _, Depth, _, St) when Depth > ?MAX_TRIGGER_DEPTH -> St;
trigger_snapshot(_, _, _, _, _, _, #st{event_limit = true} = St) -> St;
trigger_snapshot(Owner, Trigger, EventUnit, SourceId, Depth, Cutoff, St0) ->
    case unit(Owner, St0) of
        #unit{hp = Hp} when Hp =< 0 -> St0;
        #unit{by_trigger = ByTrigger} ->
            St1 = run_passives(maps:get(Trigger, ByTrigger, []), Owner, EventUnit, SourceId,
                               Depth, St0),
            Pending = [{Instance, Index}
                       || #active{instance = Instance, def = #buff{reactions = Reactions}}
                              <- (unit(Owner, St1))#unit.buffs,
                          Instance =< Cutoff,
                          Index <- reaction_indexes(Reactions, Trigger, 1)],
            St2 = run_reactions(Pending, Owner, EventUnit, Depth, St1),
            expire_buffs(Owner, Trigger, Cutoff, St2)
    end.

%% Positions (from 1) of the reactions to Trigger.
-spec reaction_indexes([#reaction{}], atom(), pos_integer()) -> [pos_integer()].
reaction_indexes([], _, _) -> [];
reaction_indexes([#reaction{trigger = Trigger} | Rest], Trigger, Index) ->
    [Index | reaction_indexes(Rest, Trigger, Index + 1)];
reaction_indexes([_ | Rest], Trigger, Index) ->
    reaction_indexes(Rest, Trigger, Index + 1).

-spec run_passives([#passive{}], pos_integer(), target_unit(), integer(), non_neg_integer(),
                   #st{}) -> #st{}.
run_passives([], _, _, _, _, St) -> St;
run_passives([#passive{id = Id, chance_bp = Chance, effects = Effects} = Passive | Rest],
             Owner, EventUnit, SourceId, Depth, St0) ->
    case roll(Chance, St0) of
        {false, St1} -> run_passives(Rest, Owner, EventUnit, SourceId, Depth, St1);
        {true, St1} ->
            case count_passive(Owner, Passive, St1) of
                {false, St2} -> run_passives(Rest, Owner, EventUnit, SourceId, Depth, St2);
                {true, St2} ->
                    #unit{side = Side, id = OwnerId} = unit(Owner, St2),
                    St3 = emit(St2#st.phase, passive, Side, OwnerId,
                               event_unit_id(EventUnit, St2, 0), source_or(Id, SourceId), 0, St2),
                    St4 = execute_effects(Owner, Owner, Effects, Id, Depth + 1, EventUnit, 1, St3),
                    run_passives(Rest, Owner, EventUnit, SourceId, Depth, St4)
            end
    end.

-spec event_unit_id(target_unit(), #st{}, integer()) -> integer().
event_unit_id(none, _, Default) -> Default;
event_unit_id(Unit, St, _) -> (unit(Unit, St))#unit.id.

-spec run_reactions([{pos_integer(), pos_integer()}], pos_integer(), target_unit(),
                    non_neg_integer(), #st{}) -> #st{}.
run_reactions([], _, _, _, St) -> St;
run_reactions([{Instance, Index} | Rest], Owner, EventUnit, Depth, St0) ->
    case St0#st.event_limit orelse not alive(Owner, St0) of
        true -> St0;
        false ->
            #unit{buffs = Buffs} = OwnerUnit = unit(Owner, St0),
            case lists:keyfind(Instance, #active.instance, Buffs) of
                false -> run_reactions(Rest, Owner, EventUnit, Depth, St0);
                #active{def = Definition, counts = Counts, stacks = Stacks,
                        source = Applier} = Active ->
                    #reaction{chance_bp = Chance, max_triggers_per_round = Max, source = From,
                              stack_scaling = Scaling, effects = Effects} =
                        lists:nth(Index, Definition#buff.reactions),
                    case roll(Chance, St0) of
                        {false, St1} -> run_reactions(Rest, Owner, EventUnit, Depth, St1);
                        {true, St1} ->
                            Count = maps:get(Index, Counts, 0),
                            case Max > 0 andalso Count >= Max of
                                true -> run_reactions(Rest, Owner, EventUnit, Depth, St1);
                                false ->
                                    Updated = Active#active{counts = Counts#{Index => Count + 1}},
                                    St2 = set_unit(Owner,
                                                   OwnerUnit#unit{buffs = lists:keyreplace(
                                                                            Instance, #active.instance,
                                                                            Buffs, Updated)},
                                                   St1),
                                    EffectSource =
                                        case From of
                                            applier -> maps:get(Applier, St2#st.index, Owner);
                                            owner -> Owner
                                        end,
                                    #unit{side = Side, id = SourceUnitId} = unit(EffectSource, St2),
                                    St3 = emit(St2#st.phase, buff_reaction, Side, SourceUnitId,
                                               event_unit_id(EventUnit, St2, OwnerUnit#unit.id),
                                               Definition#buff.id, 0, St2),
                                    Magnitude = case Scaling of
                                                    per_stack -> Stacks;
                                                    once -> 1
                                                end,
                                    St4 = execute_effects(EffectSource, Owner, Effects,
                                                          Definition#buff.id, Depth + 1, EventUnit,
                                                          Magnitude, St3),
                                    run_reactions(Rest, Owner, EventUnit, Depth, St4)
                            end
                    end
            end
    end.

-spec expire_buffs(pos_integer(), atom(), non_neg_integer(), #st{}) -> #st{}.
expire_buffs(Owner, Trigger, Cutoff, St0) ->
    case unit(Owner, St0) of
        #unit{buffs = []} -> St0;
        #unit{buffs = Buffs, side = Side, id = OwnerId, dirty = Dirty} = OwnerUnit ->
            {Kept, Expired, St1} =
                lists:foldl(
                  fun(#active{instance = Instance, remaining = Remaining,
                              def = #buff{permanent = false, decrement_on = Trigger0,
                                          id = BuffId}} = Active,
                      {Acc, AnyExpired, StAcc})
                        when Instance =< Cutoff, Trigger0 =:= Trigger ->
                          case Remaining - 1 of
                              Left when Left =< 0 ->
                                  {Acc, true, emit(StAcc#st.phase, buff_expire, Side, OwnerId,
                                                   OwnerId, BuffId, 0, StAcc)};
                              Left ->
                                  {[Active#active{remaining = Left} | Acc], AnyExpired, StAcc}
                          end;
                     (Active, {Acc, AnyExpired, StAcc}) ->
                          {[Active | Acc], AnyExpired, StAcc}
                  end,
                  {[], false, St0}, Buffs),
            set_unit(Owner, OwnerUnit#unit{buffs = lists:reverse(Kept),
                                           dirty = Dirty orelse Expired},
                     St1)
    end.
