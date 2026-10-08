-module(gamebattle_report).

%% Encodes an engine result as a protobuf BattleReport (proto/battle_client.proto)
%% with the detail a client asked for: summary, actions or events. It is a port
%% of src/report.cpp and writes the same bytes: fields in number order and zero
%% values left out, which is also what gpb writes for the same message. The
%% aggregation rules for actions are described in include/gamebattle/report.hpp.
%%
%% gamebattle_erl uses it for the `report` request option; writing the bytes
%% directly takes about a third of the time gpb takes for the same message.

-export([encode/2]).

-export_type([detail/0]).

-type detail() :: summary | actions | events.

-define(INT64_MIN, -16#8000000000000000).
-define(INT64_MAX, 16#7FFFFFFFFFFFFFFF).

%% EventType values in proto/battle_client.proto.
-define(ACTION_START, 2).
-define(ACTION_END, 3).
-define(SKILL, 4).
-define(DAMAGE, 6).
-define(DIRECT_DAMAGE, 7).
-define(MISS, 8).
-define(HEAL, 9).
-define(DEATH, 10).
-define(BUFF_ADD, 11).
-define(BUFF_REMOVE, 12).
-define(BUFF_EXPIRE, 14).

%% One step of the actions report while it is being added up.
-record(step, {
    round :: integer(),
    phase :: non_neg_integer(),
    action :: boolean(),
    side = 0 :: non_neg_integer(),
    actor = 0 :: non_neg_integer(),
    skill_id = 0 :: non_neg_integer(),
    count = 0 :: non_neg_integer(),
    %% {Unit, Damage, Heal, Hits, Crits, Misses, Hp, Died}, first affected first
    units = [] :: [tuple()],
    %% {Type, Unit, SourceId} => {Order, Count, Value}
    effects = #{} :: #{tuple() => {non_neg_integer(), pos_integer(), integer()}}
}).

-spec encode(map(), detail()) -> binary().
encode(#{battle_id := BattleId, seed := Seed, source_battle_id := SourceBattleId,
         winner := Winner, reason := Reason, rounds := Rounds,
         attacker_initiative := AttackerInitiative,
         defender_initiative := DefenderInitiative,
         units := Units, events := Events}, Detail) ->
    Summary = [uint(1, BattleId), uint(2, Seed), uint(3, SourceBattleId),
               uint(4, winner(Winner)), uint(5, reason(Reason)), sint(6, Rounds),
               uint(7, AttackerInitiative), uint(8, DefenderInitiative),
               [message(9, unit_state(Unit)) || Unit <- Units]],
    Log = case Detail of
              summary -> [];
              actions -> actions(Events, undefined, []);
              events -> [message(10, event(Event)) || Event <- Events]
          end,
    iolist_to_binary([Summary | Log]).

%%% Messages -------------------------------------------------------------------

-spec unit_state(map()) -> iodata().
unit_state(#{id := Id, side := Side, initial_hp := InitialHp, hp := Hp,
             max_hp := MaxHp, alive := Alive}) ->
    [uint(1, Id), uint(2, side(Side)), sint(3, InitialHp), sint(4, Hp), sint(5, MaxHp),
     boolean(6, Alive)].

-spec event(map()) -> iodata().
event(#{seq := Seq, round := Round, phase := Phase, type := Type, side := Side,
        actor := Actor, target := Target, source_id := SourceId, value := Value,
        hp_before := HpBefore, hp_after := HpAfter, critical := Critical}) ->
    [uint(1, Seq), sint(2, Round), uint(3, phase(Phase)), uint(4, event_type(Type)),
     uint(5, side(Side)), uint(6, Actor), uint(7, Target), uint(8, SourceId),
     sint(9, Value), sint(10, HpBefore), sint(11, HpAfter), boolean(12, Critical)].

%%% Actions (write_actions in src/report.cpp) -------------------------------------

-spec actions([map()], #step{} | undefined, [iodata()]) -> [iodata()].
actions([], undefined, Done) ->
    lists:reverse(Done);
actions([], Step, Done) ->
    lists:reverse([action(Step) | Done]);
actions([#{type := TypeName, round := Round, phase := PhaseName} = Event | Rest], Step, Done0) ->
    Type = event_type(TypeName),
    Phase = phase(PhaseName),
    {Open, Done} =
        case Step of
            _ when Type =:= ?ACTION_START -> {start(Event, Phase, true), flush(Step, Done0)};
            #step{action = true} -> {Step, Done0};
            #step{round = Round, phase = Phase} -> {Step, Done0};
            _ -> {start(Event, Phase, false), flush(Step, Done0)}
        end,
    case add(Type, Event, Open) of
        #step{action = true} = Ended when Type =:= ?ACTION_END ->
            actions(Rest, undefined, [action(Ended) | Done]);
        Next ->
            actions(Rest, Next, Done)
    end.

-spec flush(#step{} | undefined, [iodata()]) -> [iodata()].
flush(undefined, Done) -> Done;
flush(Step, Done) -> [action(Step) | Done].

-spec start(map(), non_neg_integer(), boolean()) -> #step{}.
start(#{round := Round, side := Side, actor := Actor}, Phase, true) ->
    #step{round = Round, phase = Phase, action = true, side = side(Side), actor = Actor};
start(#{round := Round}, Phase, false) ->
    #step{round = Round, phase = Phase, action = false}.

-spec add(non_neg_integer(), map(), #step{}) -> #step{}.
add(Type, Event, #step{count = Count} = Step0) ->
    Step = Step0#step{count = Count + 1},
    case Type of
        ?ACTION_START -> Step;
        ?ACTION_END -> Step;
        ?SKILL when Step#step.action -> Step#step{skill_id = maps:get(source_id, Event)};
        ?DAMAGE -> hit(Event, Step);
        ?DIRECT_DAMAGE -> hit(Event, Step);
        ?MISS ->
            #{target := Target, hp_after := HpAfter} = Event,
            {_, D, H, Hits, C, M, _, Died} = change(Target, Step),
            store({Target, D, H, Hits, C, M + 1, HpAfter, Died}, Step);
        ?HEAL ->
            #{target := Target, value := Value, hp_after := HpAfter} = Event,
            {_, D, H, Hits, C, M, _, Died} = change(Target, Step),
            store({Target, D, sat_add(H, Value), Hits, C, M, HpAfter, Died}, Step);
        ?DEATH ->
            #{target := Target} = Event,
            store(setelement(8, change(Target, Step), true), Step);
        _ ->
            #{actor := Actor, target := Target, source_id := SourceId, value := Value} = Event,
            Unit = case Type of
                       ?BUFF_ADD -> Target;
                       ?BUFF_REMOVE -> Target;
                       ?BUFF_EXPIRE -> Target;
                       _ -> Actor
                   end,
            Key = {Type, Unit, SourceId},
            Effects = Step#step.effects,
            Entry = case Effects of
                        #{Key := {Order, N, _}} -> {Order, N + 1, Value};
                        #{} -> {map_size(Effects), 1, Value}
                    end,
            Step#step{effects = Effects#{Key => Entry}}
    end.

-spec hit(map(), #step{}) -> #step{}.
hit(#{target := Target, value := Value, critical := Critical, hp_after := HpAfter}, Step) ->
    {_, D, H, Hits, C, M, _, Died} = change(Target, Step),
    Crit = case Critical of true -> 1; false -> 0 end,
    store({Target, sat_add(D, Value), H, Hits + 1, C + Crit, M, HpAfter, Died}, Step).

-spec change(non_neg_integer(), #step{}) -> tuple().
change(Unit, #step{units = Units}) ->
    case lists:keyfind(Unit, 1, Units) of
        false -> {Unit, 0, 0, 0, 0, 0, 0, false};
        Found -> Found
    end.

-spec store(tuple(), #step{}) -> #step{}.
store(Change, #step{units = Units} = Step) ->
    Step#step{units = lists:keystore(element(1, Change), 1, Units, Change)}.

-spec action(#step{}) -> iodata().
action(#step{round = Round, phase = Phase, side = Side, actor = Actor, skill_id = SkillId,
             count = Count, units = Units, effects = Effects}) ->
    Changes = [message(6, [uint(1, Unit), sint(2, Damage), sint(3, Heal), uint(4, Hits),
                           uint(5, Crits), uint(6, Misses), sint(7, Hp), boolean(8, Died)])
               || {Unit, Damage, Heal, Hits, Crits, Misses, Hp, Died} <- Units],
    Counts = [message(7, [uint(1, Type), uint(2, Unit), uint(3, SourceId), uint(4, N),
                          sint(5, Value)])
              || {_, {Type, Unit, SourceId}, N, Value}
                     <- lists:sort([{Order, Key, N, Value}
                                    || {Key, {Order, N, Value}} <- maps:to_list(Effects)])],
    message(11, [sint(1, Round), uint(2, Phase), uint(3, Side), uint(4, Actor),
                 uint(5, SkillId), Changes, Counts, uint(8, Count)]).

-spec sat_add(integer(), integer()) -> integer().
sat_add(Left, Right) -> max(?INT64_MIN, min(?INT64_MAX, Left + Right)).

%%% Protobuf writer (Writer in src/report.cpp) --------------------------------------

-spec uint(pos_integer(), non_neg_integer()) -> iodata().
uint(_, 0) -> [];
uint(Field, Value) when Value < 16#80 -> [Field bsl 3, Value];
uint(Field, Value) -> [Field bsl 3 | varint(Value)].

%% int32, int64: negative values take ten bytes, as in protobuf.
-spec sint(pos_integer(), integer()) -> iodata().
sint(Field, Value) when Value < 0 -> uint(Field, Value + (1 bsl 64));
sint(Field, Value) -> uint(Field, Value).

-spec boolean(pos_integer(), boolean()) -> iodata().
boolean(Field, true) -> [Field bsl 3, 1];
boolean(_, false) -> [].

-spec message(pos_integer(), iodata()) -> iodata().
message(Field, Body) ->
    [varint(Field bsl 3 bor 2), varint(iolist_size(Body)) | Body].

-spec varint(non_neg_integer()) -> iodata().
varint(Value) when Value < 16#80 -> [Value];
varint(Value) -> [Value band 16#7F bor 16#80 | varint(Value bsr 7)].

%%% Enum values ---------------------------------------------------------------------

-spec event_type(atom()) -> non_neg_integer().
event_type(initiative) -> 1;
event_type(action_start) -> 2;
event_type(action_end) -> 3;
event_type(skill) -> 4;
event_type(passive) -> 5;
event_type(damage) -> 6;
event_type(direct_damage) -> 7;
event_type(miss) -> 8;
event_type(heal) -> 9;
event_type(death) -> 10;
event_type(buff_add) -> 11;
event_type(buff_remove) -> 12;
event_type(buff_reaction) -> 13;
event_type(buff_expire) -> 14;
event_type(chain) -> 15;
event_type(negate) -> 16;
event_type(fizzle) -> 17;
event_type(_) -> 0.

-spec phase(atom()) -> non_neg_integer().
phase(battle) -> 1;
phase(round_start) -> 2;
phase(first_side) -> 3;
phase(second_side) -> 4;
phase(round_end) -> 5;
phase(_) -> 0.

-spec reason(atom()) -> non_neg_integer().
reason(initial_state) -> 1;
reason(battle_start) -> 2;
reason(round_start) -> 3;
reason(all_units_defeated) -> 4;
reason(round_end) -> 5;
reason(max_rounds) -> 6;
reason(event_limit) -> 7;
reason(_) -> 0.

-spec side(attacker | defender) -> 1 | 2.
side(attacker) -> 1;
side(defender) -> 2.

-spec winner(attacker | defender | draw) -> 1..3.
winner(attacker) -> 1;
winner(defender) -> 2;
winner(draw) -> 3.
