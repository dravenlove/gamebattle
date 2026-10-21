-module(gamebattle_erl_request).

%% Request decoding and validation for the pure-Erlang engine. It accepts and
%% rejects exactly what the C++ side does, with the same messages:
%%   check_term/1  the ETF reader (src/term.cpp): what any request may contain
%%   report_detail/1  parse_report_detail (src/wire.cpp): the `report` option
%%   parse/2       parse_request (src/wire.cpp): fields, defaults, types
%%   validate/1    validate_request (src/battle_state.cpp): value bounds
%% Errors are thrown as {gamebattle_erl_invalid, Message}.

-include("gamebattle_erl.hrl").

-export([parse/2, validate/1, fail/1]).

-define(MAX_TERM_DEPTH, 128).
-define(MAX_CONTAINER_ITEMS, 1000000).
-define(MAX_EMBEDDED_DEPTH, 32).

-spec fail(binary()) -> no_return().
fail(Message) ->
    throw({gamebattle_erl_invalid, Message}).

-spec parse(term(), #config{} | undefined) -> #request{}.
parse(Value, Config) ->
    check_term(Value, 0),
    Detail = report_detail(Value),
    Request = parse_request(Value, Config),
    Request#request{report = Detail}.

-spec report_detail(term()) -> none | gamebattle_report:detail().
report_detail(Value) when is_map(Value) ->
    case find(Value, report) of
        {ok, Detail} ->
            case as_string(Detail, <<"report">>) of
                <<"summary">> -> summary;
                <<"actions">> -> actions;
                <<"events">> -> events;
                _ -> fail(<<"report must be summary, actions, or events">>)
            end;
        error -> none
    end;
report_detail(_) ->
    none.

%%% ETF-level checks ----------------------------------------------------------

-spec check_term(term(), non_neg_integer()) -> ok.
check_term(_, Depth) when Depth > ?MAX_TERM_DEPTH ->
    fail(<<"ETF nesting is too deep">>);
check_term(Value, _) when is_integer(Value) ->
    check_integer(Value);
check_term(Value, _) when is_atom(Value); is_float(Value) ->
    ok;
check_term(Value, _) when is_binary(Value) ->
    check_count(byte_size(Value));
check_term([], _) ->
    ok;
check_term(Value, Depth) when is_list(Value) ->
    %% term_to_binary writes short lists of bytes as STRING_EXT, whose
    %% elements the reader takes as raw bytes, not as nested values.
    case is_string_ext(Value, 0) of
        true -> ok;
        false -> check_list(Value, Depth)
    end;
check_term(Value, Depth) when is_tuple(Value) ->
    check_count(tuple_size(Value)),
    lists:foreach(fun(Item) -> check_term(Item, Depth + 1) end, tuple_to_list(Value));
check_term(Value, Depth) when is_map(Value) ->
    check_count(map_size(Value)),
    lists:foreach(
        fun({Key, Item}) ->
            check_term(Key, Depth + 1),
            is_atom(Key) orelse is_binary(Key) orelse
                fail(<<"ETF map keys must be atoms or binaries">>),
            check_term(Item, Depth + 1)
        end,
        maps:to_list(Value));
check_term(Value, _) ->
    Tag = binary:at(term_to_binary(Value), 1),
    fail(<<"unsupported ETF tag: ", (integer_to_binary(Tag))/binary>>).

-spec check_integer(integer()) -> ok.
check_integer(Value) when Value >= ?INT64_MIN, Value =< ?INT64_MAX -> ok;
check_integer(Value) when Value > 0, Value =< ?UINT64_MAX ->
    fail(<<"positive integer does not fit into int64">>);
check_integer(Value) when Value < 0, -Value =< ?UINT64_MAX ->
    fail(<<"negative integer does not fit into int64">>);
check_integer(_) ->
    fail(<<"integer does not fit into 64 bits">>).

-spec check_count(non_neg_integer()) -> ok.
check_count(Count) when Count > ?MAX_CONTAINER_ITEMS ->
    fail(<<"ETF container is too large">>);
check_count(_) ->
    ok.

-spec is_string_ext(term(), non_neg_integer()) -> boolean().
is_string_ext([], Length) -> Length =< 16#FFFF;
is_string_ext([Byte | Rest], Length) when is_integer(Byte), Byte >= 0, Byte =< 255 ->
    Length < 16#FFFF andalso is_string_ext(Rest, Length + 1);
is_string_ext(_, _) -> false.

-spec check_list(list(), non_neg_integer()) -> ok.
check_list(List, Depth) ->
    {Count, Tail} = list_shape(List, 0),
    check_count(Count),
    check_elements(List, Depth + 1),
    case Tail of
        [] -> ok;
        _ ->
            check_term(Tail, Depth + 1),
            fail(<<"improper ETF lists are not supported">>)
    end.

-spec list_shape(term(), non_neg_integer()) -> {non_neg_integer(), term()}.
list_shape([_ | Rest], Count) -> list_shape(Rest, Count + 1);
list_shape(Tail, Count) -> {Count, Tail}.

-spec check_elements(term(), non_neg_integer()) -> ok.
check_elements([Item | Rest], Depth) ->
    check_term(Item, Depth),
    check_elements(Rest, Depth);
check_elements(_, _) ->
    ok.

%%% Field access (src/term.cpp) -----------------------------------------------

%% Keys may be atoms or binaries; an atom key is found first, as in the
%% reader's map order.
-spec find(map(), atom()) -> {ok, term()} | error.
find(Map, Key) ->
    case Map of
        #{Key := Value} -> {ok, Value};
        _ -> maps:find(atom_to_binary(Key), Map)
    end.

-spec type_error(binary(), binary()) -> no_return().
type_error(Path, Expected) ->
    fail(<<Path/binary, " must be ", Expected/binary>>).

-spec as_list(term(), binary()) -> list().
as_list(Value, _) when is_list(Value) -> Value;
as_list(_, Path) -> type_error(Path, <<"a list">>).

-spec as_object(term(), binary()) -> map().
as_object(Value, _) when is_map(Value) -> Value;
as_object(_, Path) -> type_error(Path, <<"a map">>).

-spec as_string(term(), binary()) -> binary().
as_string(Value, _) when is_atom(Value) -> atom_to_binary(Value);
as_string(Value, _) when is_binary(Value) -> Value;
as_string(_, Path) -> type_error(Path, <<"an atom or binary">>).

-spec as_int(term(), binary()) -> integer().
as_int(Value, _) when is_integer(Value) -> Value;
as_int(_, Path) -> type_error(Path, <<"an integer">>).

-spec as_bool(term(), binary()) -> boolean().
as_bool(true, _) -> true;
as_bool(false, _) -> false;
as_bool(_, Path) -> type_error(Path, <<"true or false">>).

-spec get_string(map(), atom(), binary()) -> binary().
get_string(Map, Key, Default) ->
    case find(Map, Key) of
        {ok, Value} -> as_string(Value, atom_to_binary(Key));
        error -> Default
    end.

-spec get_int(map(), atom(), integer()) -> integer().
get_int(Map, Key, Default) ->
    case find(Map, Key) of
        {ok, Value} -> as_int(Value, atom_to_binary(Key));
        error -> Default
    end.

-spec get_bool(map(), atom(), boolean()) -> boolean().
get_bool(Map, Key, Default) ->
    case find(Map, Key) of
        {ok, Value} -> as_bool(Value, atom_to_binary(Key));
        error -> Default
    end.

-spec checked_int32(integer(), binary()) -> integer().
checked_int32(Value, _) when Value >= ?INT32_MIN, Value =< ?INT32_MAX -> Value;
checked_int32(_, Path) -> out_of_range(Path).

-spec checked_uint32(integer(), binary()) -> non_neg_integer().
checked_uint32(Value, _) when Value >= 0, Value =< ?UINT32_MAX -> Value;
checked_uint32(_, Path) -> out_of_range(Path).

-spec out_of_range(binary()) -> no_return().
out_of_range(Path) ->
    fail(<<Path/binary, " is outside the supported integer range">>).

-spec nonnegative(integer(), binary()) -> non_neg_integer().
nonnegative(Value, Path) when Value < 0 -> fail(<<Path/binary, " must not be negative">>);
nonnegative(Value, _) -> Value.

-spec basis_points(map(), atom(), integer()) -> integer().
basis_points(Map, Key, Default) ->
    checked_int32(get_int(Map, Key, Default), atom_to_binary(Key)).

-spec require_field(map(), atom(), binary()) -> term().
require_field(Map, Key, Path) ->
    case find(Map, Key) of
        {ok, Value} -> Value;
        error ->
            fail(<<Path/binary, " requires field '", (atom_to_binary(Key))/binary, "'">>)
    end.

-spec require_only_fields(term(), binary(), [binary()]) -> map().
require_only_fields(Value, Path, Supported) ->
    Map = as_object(Value, Path),
    lists:foreach(
        fun(Key) ->
            Text = key_text(Key),
            lists:member(Text, Supported) orelse
                fail(<<Path/binary, " contains unsupported field '", Text/binary, "'">>)
        end,
        maps:keys(Map)),
    Map.

-spec key_text(atom() | binary()) -> binary().
key_text(Key) when is_atom(Key) -> atom_to_binary(Key);
key_text(Key) -> Key.

-spec check_embedded_depth(non_neg_integer()) -> ok.
check_embedded_depth(Depth) when Depth > ?MAX_EMBEDDED_DEPTH ->
    fail(<<"embedded buff/effect nesting exceeds the supported depth of 32">>);
check_embedded_depth(_) ->
    ok.

%%% Enumerations (src/wire.cpp) -----------------------------------------------

-spec kind(binary()) -> hero | beauty | pet | artifact.
kind(<<"hero">>) -> hero;
kind(<<"beauty">>) -> beauty;
kind(<<"pet">>) -> pet;
kind(<<"artifact">>) -> artifact;
kind(<<"divine_weapon">>) -> artifact;
kind(_) -> fail(<<"unit.kind must be hero, beauty, pet, or artifact">>).

-spec target(binary()) -> atom().
target(<<"self">>) -> self;
target(<<"trigger_unit">>) -> trigger_unit;
target(<<"enemy_front">>) -> enemy_front;
target(<<"enemy_lowest_hp">>) -> enemy_lowest_hp;
target(<<"ally_lowest_hp">>) -> ally_lowest_hp;
target(<<"all_enemies">>) -> all_enemies;
target(<<"all_allies">>) -> all_allies;
target(_) -> fail(<<"effect.target has an unsupported value">>).

-spec trigger(binary()) -> atom().
trigger(<<"battle_start">>) -> battle_start;
trigger(<<"round_start">>) -> round_start;
trigger(<<"before_action">>) -> before_action;
trigger(<<"on_attack">>) -> on_attack;
trigger(<<"on_hit">>) -> on_hit;
trigger(<<"on_damaged">>) -> on_damaged;
trigger(<<"unit_death">>) -> unit_death;
trigger(<<"after_action">>) -> after_action;
trigger(<<"round_end">>) -> round_end;
trigger(<<"enemy_activate">>) -> enemy_activate;
trigger(<<"ally_activate">>) -> ally_activate;
trigger(_) -> fail(<<"passive.trigger has an unsupported value">>).

-spec effect_kind(binary()) -> atom().
effect_kind(<<"damage">>) -> damage;
effect_kind(<<"heal">>) -> heal;
effect_kind(<<"add_buff">>) -> add_buff;
effect_kind(<<"remove_buff">>) -> remove_buff;
effect_kind(<<"direct_damage">>) -> direct_damage;
effect_kind(<<"negate">>) -> negate;
effect_kind(_) ->
    fail(<<"effect.type must be damage, direct_damage, heal, add_buff, remove_buff, or negate">>).

-spec attribute(binary()) -> atom().
attribute(<<"attack">>) -> attack;
attribute(<<"defense">>) -> defense;
attribute(<<"speed">>) -> speed;
attribute(<<"crit_rate_bp">>) -> crit_rate_bp;
attribute(<<"crit_damage_bp">>) -> crit_damage_bp;
attribute(<<"hit_rate_bp">>) -> hit_rate_bp;
attribute(<<"dodge_rate_bp">>) -> dodge_rate_bp;
attribute(<<"damage_bonus_bp">>) -> damage_bonus_bp;
attribute(<<"damage_reduction_bp">>) -> damage_reduction_bp;
attribute(_) -> fail(<<"buff.modifier.attribute has an unsupported value">>).

-spec modifier_operation(binary()) -> add | scale_bp.
modifier_operation(<<"add">>) -> add;
modifier_operation(<<"scale_bp">>) -> scale_bp;
modifier_operation(_) -> fail(<<"buff.modifier.operation must be add or scale_bp">>).

-spec stack_policy(binary()) -> stack | refresh.
stack_policy(<<"stack">>) -> stack;
stack_policy(<<"refresh">>) -> refresh;
stack_policy(_) -> fail(<<"buff.stacking.policy must be stack or refresh">>).

-spec refresh_policy(binary()) -> reset | extend | keep.
refresh_policy(<<"reset">>) -> reset;
refresh_policy(<<"extend">>) -> extend;
refresh_policy(<<"keep">>) -> keep;
refresh_policy(_) -> fail(<<"buff.stacking.refresh must be reset, extend, or keep">>).

-spec effect_source(binary()) -> owner | applier.
effect_source(<<"owner">>) -> owner;
effect_source(<<"applier">>) -> applier;
effect_source(_) -> fail(<<"buff.reaction.source must be owner or applier">>).

-spec stack_scaling(binary()) -> once | per_stack.
stack_scaling(<<"once">>) -> once;
stack_scaling(<<"per_stack">>) -> per_stack;
stack_scaling(_) -> fail(<<"buff.reaction.stack_scaling must be once or per_stack">>).

%%% parse_request (src/wire.cpp) ----------------------------------------------

-spec parse_request(term(), #config{} | undefined) -> #request{}.
parse_request(Value, Config) ->
    Map = as_object(Value, <<"request">>),
    BattleId = nonnegative(get_int(Map, battle_id, 0), <<"battle_id">>),
    Seed = nonnegative(get_int(Map, seed, 1), <<"seed">>),
    MaxRounds = checked_int32(get_int(Map, max_rounds, 50), <<"max_rounds">>),
    MaxEvents = checked_int32(get_int(Map, max_events, 10000), <<"max_events">>),
    {AttackerMap, DefenderMap} =
        case {find(Map, attacker), find(Map, defender)} of
            {{ok, A}, {ok, D}} -> {A, D};
            _ -> fail(<<"request requires attacker and defender formation maps">>)
        end,
    {AttackerBonus, Attacker} = parse_formation(AttackerMap, <<"attacker">>, Config),
    {DefenderBonus, Defender} = parse_formation(DefenderMap, <<"defender">>, Config),
    Request = #request{battle_id = BattleId, seed = Seed,
                       max_rounds = MaxRounds, max_events = MaxEvents,
                       attacker_bonus = AttackerBonus, attacker = Attacker,
                       defender_bonus = DefenderBonus, defender = Defender},
    case find(Map, initial_conditions) of
        {ok, Initial} -> parse_initial_conditions(Initial, Request);
        error -> Request
    end.

-spec parse_formation(term(), binary(), #config{} | undefined) -> {integer(), [#unit_config{}]}.
parse_formation(Value, Path, Config) ->
    Map = as_object(Value, Path),
    _ = get_string(Map, formation, <<"default">>),
    Bonus = get_int(Map, initiative_bonus, 0),
    Units = case find(Map, units) of
                {ok, U} -> U;
                error -> fail(<<Path/binary, ".units is required">>)
            end,
    {Bonus, [parse_unit(Unit, Config) || Unit <- as_list(Units, <<"units">>)]}.

-spec parse_unit(term(), #config{} | undefined) -> #unit_config{}.
parse_unit(Value, Config) ->
    Map = as_object(Value, <<"unit">>),
    Id = nonnegative(get_int(Map, id, 0), <<"unit.id">>),
    Kind = kind(get_string(Map, kind, <<"hero">>)),
    Position = checked_int32(get_int(Map, position, 0), <<"unit.position">>),
    Level = checked_int32(get_int(Map, level, 1), <<"unit.level">>),
    HeroDefaults = Kind =:= hero,
    CanAct = get_bool(Map, can_act, HeroDefaults),
    Targetable = get_bool(Map, targetable, HeroDefaults),
    case find(Map, growth_levels) of
        {ok, Growth} ->
            maps:foreach(
                fun(Key, Item) ->
                    Text = key_text(Key),
                    checked_int32(as_int(Item, Text), Text)
                end,
                as_object(Growth, <<"growth_levels">>));
        error ->
            ok
    end,
    Stats = case find(Map, final_stats) of
                {ok, S} -> parse_stats(S);
                error -> fail(<<"unit.final_stats is required">>)
            end,
    EmbeddedSkills = find(Map, skills),
    ConfiguredSkills = find(Map, skill_ids),
    EmbeddedSkills =/= error andalso ConfiguredSkills =/= error andalso
        fail(<<"unit cannot contain both skills and skill_ids">>),
    Skills = case EmbeddedSkills of
                 {ok, SkillList} -> [parse_skill(Skill) || Skill <- as_list(SkillList, <<"skills">>)];
                 error -> []
             end,
    EmbeddedPassives = find(Map, passives),
    ConfiguredPassives = find(Map, passive_ids),
    EmbeddedPassives =/= error andalso ConfiguredPassives =/= error andalso
        fail(<<"unit cannot contain both passives and passive_ids">>),
    Passives = case EmbeddedPassives of
                   {ok, PassiveList} ->
                       [parse_passive(Passive) || Passive <- as_list(PassiveList, <<"passives">>)];
                   error -> []
               end,
    {LoadoutSkills, LoadoutPassives} =
        case ConfiguredSkills =/= error orelse ConfiguredPassives =/= error of
            false -> {[], []};
            true when Config =:= undefined ->
                fail(<<"skill_ids/passive_ids require a loaded battle config pack">>);
            true ->
                SkillIds = config_ids(ConfiguredSkills, <<"skill_ids">>),
                ConfigSkills = [loadout(Id1, Config#config.skills, <<"skill">>)
                                || Id1 <- SkillIds],
                PassiveIds = config_ids(ConfiguredPassives, <<"passive_ids">>),
                ConfigPassives = [loadout(Id2, Config#config.passives, <<"passive">>)
                                  || Id2 <- PassiveIds],
                {ConfigSkills, ConfigPassives}
        end,
    #unit_config{id = Id, kind = Kind, position = Position, level = Level,
                 can_act = CanAct, targetable = Targetable, stats = Stats,
                 skills = Skills ++ LoadoutSkills,
                 passives = Passives ++ LoadoutPassives}.

-spec config_ids({ok, term()} | error, binary()) -> [non_neg_integer()].
config_ids(error, _) -> [];
config_ids({ok, List}, Key) ->
    [checked_uint32(as_int(Item, Key), Key) || Item <- as_list(List, Key)].

-spec loadout(non_neg_integer(), map(), binary()) -> #skill{} | #passive{}.
loadout(Id, Table, Kind) ->
    case Table of
        #{Id := Item} -> Item;
        _ -> fail(<<"unit loadout: unknown ", Kind/binary, " id ", (integer_to_binary(Id))/binary>>)
    end.

-spec parse_stats(term()) -> #stats{}.
parse_stats(Value) ->
    Map = as_object(Value, <<"final_stats">>),
    case {find(Map, hp), find(Map, attack), find(Map, defense), find(Map, speed)} of
        {{ok, Hp}, {ok, Attack}, {ok, Defense}, {ok, Speed}} ->
            #stats{hp = as_int(Hp, <<"final_stats.hp">>),
                   attack = as_int(Attack, <<"final_stats.attack">>),
                   defense = as_int(Defense, <<"final_stats.defense">>),
                   speed = as_int(Speed, <<"final_stats.speed">>),
                   crit_rate_bp = basis_points(Map, crit_rate_bp, 0),
                   crit_damage_bp = basis_points(Map, crit_damage_bp, 15000),
                   hit_rate_bp = basis_points(Map, hit_rate_bp, 10000),
                   dodge_rate_bp = basis_points(Map, dodge_rate_bp, 0),
                   damage_bonus_bp = basis_points(Map, damage_bonus_bp, 0),
                   damage_reduction_bp = basis_points(Map, damage_reduction_bp, 0)};
        _ ->
            fail(<<"final_stats requires hp, attack, defense, and speed">>)
    end.

-spec parse_skill(term()) -> #skill{}.
parse_skill(Value) ->
    Map = as_object(Value, <<"skill">>),
    Id = checked_uint32(get_int(Map, id, 0), <<"skill.id">>),
    _ = get_string(Map, name, <<>>),
    Chance = basis_points(Map, chance_bp, 10000),
    Priority = checked_int32(get_int(Map, priority, 0), <<"skill.priority">>),
    Effects = parse_effects(Map, 0),
    Effects =:= [] andalso fail(<<"each skill must contain at least one effect">>),
    #skill{id = Id, chance_bp = Chance, priority = Priority, effects = Effects}.

-spec parse_passive(term()) -> #passive{}.
parse_passive(Value) ->
    Map = as_object(Value, <<"passive">>),
    Id = checked_uint32(get_int(Map, id, 0), <<"passive.id">>),
    _ = get_string(Map, name, <<>>),
    Trigger = trigger(get_string(Map, trigger, <<"on_damaged">>)),
    Chance = basis_points(Map, chance_bp, 10000),
    MaxTriggers = checked_int32(get_int(Map, max_triggers_per_round, 0),
                                <<"passive.max_triggers_per_round">>),
    Effects = parse_effects(Map, 0),
    Effects =:= [] andalso fail(<<"each passive must contain at least one effect">>),
    #passive{id = Id, trigger = Trigger, chance_bp = Chance,
             max_triggers_per_round = MaxTriggers, effects = Effects}.

-spec parse_effects(map(), non_neg_integer()) -> [#effect{}].
parse_effects(Map, Depth) ->
    check_embedded_depth(Depth),
    case find(Map, effects) of
        {ok, List} -> [parse_effect(Effect, Depth) || Effect <- as_list(List, <<"effects">>)];
        error -> []
    end.

-spec parse_effect(term(), non_neg_integer()) -> #effect{}.
parse_effect(Value, Depth) ->
    check_embedded_depth(Depth),
    Map = as_object(Value, <<"effect">>),
    Kind = effect_kind(get_string(Map, type, <<"damage">>)),
    DefaultTarget = case Kind of
                        heal -> <<"ally_lowest_hp">>;
                        _ -> <<"enemy_front">>
                    end,
    Target = target(get_string(Map, target, DefaultTarget)),
    Count = checked_int32(get_int(Map, target_count, 1), <<"effect.target_count">>),
    AttackBp = basis_points(Map, attack_bp, case Kind of damage -> 10000; _ -> 0 end),
    Flat = get_int(Map, flat, 0),
    Effect = #effect{kind = Kind, target = Target, target_count = Count,
                     attack_bp = AttackBp, flat = Flat},
    case Kind of
        add_buff ->
            case find(Map, buff) of
                {ok, Buff} -> Effect#effect{buff = parse_buff(Buff, Depth + 1)};
                error -> fail(<<"add_buff effect requires a buff map">>)
            end;
        remove_buff ->
            Effect#effect{remove_buff_id =
                              checked_uint32(get_int(Map, buff_id, 0), <<"effect.buff_id">>)};
        _ ->
            Effect
    end.

-spec parse_buff(term(), non_neg_integer()) -> #buff{}.
parse_buff(Value, Depth) ->
    check_embedded_depth(Depth),
    Map = require_only_fields(Value, <<"buff">>,
                              [<<"id">>, <<"name">>, <<"lifetime">>, <<"stacking">>,
                               <<"modifiers">>, <<"reactions">>]),
    Id = checked_uint32(as_int(require_field(Map, id, <<"buff">>), <<"buff.id">>), <<"buff.id">>),
    _ = as_string(require_field(Map, name, <<"buff">>), <<"buff.name">>),

    Lifetime = require_only_fields(require_field(Map, lifetime, <<"buff">>), <<"buff.lifetime">>,
                                   [<<"type">>, <<"duration">>, <<"decrement_on">>]),
    Permanent =
        case as_string(require_field(Lifetime, type, <<"buff.lifetime">>), <<"buff.lifetime.type">>) of
            <<"finite">> -> false;
            <<"permanent">> -> true;
            _ -> fail(<<"buff.lifetime.type must be finite or permanent">>)
        end,
    Duration = checked_int32(as_int(require_field(Lifetime, duration, <<"buff.lifetime">>),
                                    <<"buff.lifetime.duration">>),
                             <<"buff.lifetime.duration">>),
    DecrementOn = trigger(as_string(require_field(Lifetime, decrement_on, <<"buff.lifetime">>),
                                    <<"buff.lifetime.decrement_on">>)),
    ((Permanent andalso Duration =/= 0) orelse (not Permanent andalso Duration < 1)) andalso
        fail(<<"permanent buffs require duration 0; finite buffs require duration >= 1">>),

    Stacking = require_only_fields(require_field(Map, stacking, <<"buff">>), <<"buff.stacking">>,
                                   [<<"max_stacks">>, <<"policy">>, <<"refresh">>]),
    MaxStacks = checked_int32(as_int(require_field(Stacking, max_stacks, <<"buff.stacking">>),
                                     <<"buff.stacking.max_stacks">>),
                              <<"buff.stacking.max_stacks">>),
    Mode = stack_policy(as_string(require_field(Stacking, policy, <<"buff.stacking">>),
                                  <<"buff.stacking.policy">>)),
    Refresh = refresh_policy(as_string(require_field(Stacking, refresh, <<"buff.stacking">>),
                                       <<"buff.stacking.refresh">>)),
    Mode =:= refresh andalso MaxStacks =/= 1 andalso
        fail(<<"buff.stacking policy refresh requires max_stacks 1">>),

    Modifiers = [parse_modifier(Modifier)
                 || Modifier <- as_list(require_field(Map, modifiers, <<"buff">>),
                                        <<"buff.modifiers">>)],
    Reactions = [parse_reaction(Reaction, Depth)
                 || Reaction <- as_list(require_field(Map, reactions, <<"buff">>),
                                        <<"buff.reactions">>)],
    #buff{id = Id, ident = make_ref(), permanent = Permanent, duration = Duration,
          decrement_on = DecrementOn, max_stacks = MaxStacks, mode = Mode,
          refresh = Refresh, modifiers = Modifiers, reactions = Reactions}.

-spec parse_modifier(term()) -> {atom(), add | scale_bp, integer()}.
parse_modifier(Value) ->
    Map = require_only_fields(Value, <<"buff.modifier">>,
                              [<<"attribute">>, <<"operation">>, <<"value">>]),
    case {find(Map, attribute), find(Map, operation), find(Map, value)} of
        {{ok, Attribute}, {ok, Operation}, {ok, Amount}} ->
            A = attribute(as_string(Attribute, <<"buff.modifier.attribute">>)),
            O = modifier_operation(as_string(Operation, <<"buff.modifier.operation">>)),
            {A, O, as_int(Amount, <<"buff.modifier.value">>)};
        _ ->
            fail(<<"each buff modifier requires attribute, operation, and value">>)
    end.

-spec parse_reaction(term(), non_neg_integer()) -> #reaction{}.
parse_reaction(Value, Depth) ->
    Map = require_only_fields(Value, <<"buff.reaction">>,
                              [<<"trigger">>, <<"source">>, <<"stack_scaling">>,
                               <<"chance_bp">>, <<"max_triggers_per_round">>, <<"effects">>]),
    Trigger = trigger(get_string(Map, trigger, <<"round_end">>)),
    Source = effect_source(get_string(Map, source, <<"owner">>)),
    Scaling = stack_scaling(get_string(Map, stack_scaling, <<"once">>)),
    Chance = basis_points(Map, chance_bp, 10000),
    MaxTriggers = checked_int32(get_int(Map, max_triggers_per_round, 0),
                                <<"buff.reaction.max_triggers_per_round">>),
    Effects = parse_effects(Map, Depth + 1),
    Effects =:= [] andalso fail(<<"each buff reaction must contain at least one effect">>),
    #reaction{trigger = Trigger, source = Source, stack_scaling = Scaling,
              chance_bp = Chance, max_triggers_per_round = MaxTriggers, effects = Effects}.

-spec parse_initial_conditions(term(), #request{}) -> #request{}.
parse_initial_conditions(Value, Request) ->
    Map = as_object(Value, <<"initial_conditions">>),
    Source = nonnegative(get_int(Map, source_battle_id, 0),
                         <<"initial_conditions.source_battle_id">>),
    FirstSide = case get_string(Map, first_side, <<"automatic">>) of
                    <<"attacker">> -> attacker;
                    <<"defender">> -> defender;
                    <<"automatic">> -> undefined;
                    _ -> fail(<<"initial_conditions.first_side must be automatic, attacker, or defender">>)
                end,
    States = case find(Map, unit_states) of
                 {ok, List} ->
                     [parse_unit_state(Item)
                      || Item <- as_list(List, <<"initial_conditions.unit_states">>)];
                 error -> []
             end,
    Request#request{source_battle_id = Source, forced_first_side = FirstSide,
                    unit_states = States}.

-spec parse_unit_state(term()) -> {non_neg_integer(), integer()}.
parse_unit_state(Value) ->
    Map = as_object(Value, <<"initial_conditions.unit_state">>),
    case {find(Map, unit_id), find(Map, current_hp)} of
        {{ok, UnitId}, {ok, CurrentHp}} ->
            {nonnegative(as_int(UnitId, <<"initial_conditions.unit_id">>),
                         <<"initial_conditions.unit_id">>),
             as_int(CurrentHp, <<"initial_conditions.current_hp">>)};
        _ ->
            fail(<<"each initial unit state requires unit_id and current_hp">>)
    end.

%%% validate_request (src/battle_state.cpp) -----------------------------------

-define(VALID_PROBABILITY(V), (V >= 0 andalso V =< ?BASIS_POINTS)).
-define(VALID_MODIFIER(V), (V >= -1000000 andalso V =< 1000000)).
-define(VALID_FLAT(V), (V >= -1000000000000 andalso V =< 1000000000000)).
-define(RESPONSE(T), (T =:= enemy_activate orelse T =:= ally_activate)).

%% {Definitions, Validating, Validated}: buff id => ident, and the sets of
%% idents being validated and already validated.
-type buffs() :: {#{integer() => term()}, #{term() => true}, #{term() => true}}.

-spec validate(#request{}) -> ok.
validate(#request{max_rounds = MaxRounds, max_events = MaxEvents,
                  attacker = Attacker, defender = Defender, unit_states = States}) ->
    (MaxRounds < 1 orelse MaxRounds > 10000) andalso
        fail(<<"max_rounds must be between 1 and 10000">>),
    (MaxEvents < 100 orelse MaxEvents > 1000000) andalso
        fail(<<"max_events must be between 100 and 1000000">>),
    (Attacker =:= [] orelse Defender =:= []) andalso
        fail(<<"both formations must contain at least one unit">>),
    (length(Attacker) > 256 orelse length(Defender) > 256) andalso
        fail(<<"a formation cannot contain more than 256 units">>),
    {Configs, _} = lists:foldl(fun validate_unit/2, {#{}, {#{}, #{}, #{}}},
                               Attacker ++ Defender),
    length(States) > map_size(Configs) andalso
        fail(<<"initial_conditions contains more unit states than the formations">>),
    _ = lists:foldl(
        fun({UnitId, CurrentHp}, Seen) ->
            MaxHp = case Configs of
                        #{UnitId := Hp} when UnitId =/= 0 -> Hp;
                        _ -> fail(<<"initial_conditions references a unit that is not in either formation">>)
                    end,
            maps:is_key(UnitId, Seen) andalso
                fail(<<"initial_conditions contains duplicate unit ids">>),
            (CurrentHp < 0 orelse CurrentHp > MaxHp) andalso
                fail(<<"initial current_hp must be between zero and the unit's final_stats.hp">>),
            Seen#{UnitId => true}
        end,
        #{}, States),
    ok.

-spec validate_unit(#unit_config{}, {map(), buffs()}) -> {map(), buffs()}.
validate_unit(#unit_config{id = Id, position = Position, level = Level, stats = Stats,
                           skills = Skills, passives = Passives},
              {Configs, Buffs0}) ->
    (Id =:= 0 orelse maps:is_key(Id, Configs)) andalso
        fail(<<"unit ids must be non-zero and unique across both sides">>),
    (Position < 0 orelse Position > 1000) andalso
        fail(<<"unit position must be between 0 and 1000">>),
    (Level < 1 orelse Level > 1000000) andalso
        fail(<<"unit level must be between 1 and 1000000">>),
    #stats{hp = Hp, attack = Attack, defense = Defense, speed = Speed} = Stats,
    (Hp =< 0 orelse Hp > 1000000000000 orelse
     Attack < 0 orelse Attack > 1000000000000 orelse
     Defense < 0 orelse Defense > 1000000000000 orelse
     Speed < 0 orelse Speed > 1000000000000) andalso
        fail(<<"hp/attack/defense/speed attributes are outside supported bounds">>),
    #stats{crit_rate_bp = CritRate, crit_damage_bp = CritDamage, hit_rate_bp = Hit,
           dodge_rate_bp = Dodge, damage_bonus_bp = Bonus,
           damage_reduction_bp = Reduction} = Stats,
    (not ?VALID_PROBABILITY(CritRate) orelse
     CritDamage < ?BASIS_POINTS orelse CritDamage > 1000000 orelse
     Hit < 0 orelse Hit > 100000 orelse Dodge < 0 orelse Dodge > 100000 orelse
     not ?VALID_MODIFIER(Bonus) orelse not ?VALID_MODIFIER(Reduction)) andalso
        fail(<<"rate attributes are outside supported bounds">>),
    (length(Skills) > 128 orelse length(Passives) > 128) andalso
        fail(<<"a unit cannot contain more than 128 skills or passives">>),
    {_, Buffs1} = lists:foldl(fun validate_skill/2, {#{}, Buffs0}, Skills),
    {_, Buffs2} = lists:foldl(fun validate_passive/2, {#{}, Buffs1}, Passives),
    {Configs#{Id => Hp}, Buffs2}.

-spec validate_skill(#skill{}, {map(), buffs()}) -> {map(), buffs()}.
validate_skill(#skill{id = Id, chance_bp = Chance, effects = Effects}, {Seen, Buffs}) ->
    (Id =:= 0 orelse maps:is_key(Id, Seen) orelse not ?VALID_PROBABILITY(Chance) orelse
     Effects =:= [] orelse length(Effects) > 64) andalso
        fail(<<"skill id, chance, or effect count is invalid">>),
    contains_negate(Effects) andalso
        fail(<<"negate effects are only valid in response passives">>),
    {Seen#{Id => true}, validate_effects(Effects, 0, Buffs)}.

-spec validate_passive(#passive{}, {map(), buffs()}) -> {map(), buffs()}.
validate_passive(#passive{id = Id, trigger = Trigger, chance_bp = Chance,
                          max_triggers_per_round = MaxTriggers, effects = Effects},
                 {Seen, Buffs}) ->
    (Id =:= 0 orelse maps:is_key(Id, Seen) orelse not ?VALID_PROBABILITY(Chance) orelse
     MaxTriggers < 0 orelse MaxTriggers > 10000 orelse
     Effects =:= [] orelse length(Effects) > 64) andalso
        fail(<<"passive id, trigger, chance, trigger limit, or effect count is invalid">>),
    not ?RESPONSE(Trigger) andalso contains_negate(Effects) andalso
        fail(<<"negate effects are only valid in response passives">>),
    {Seen#{Id => true}, validate_effects(Effects, 0, Buffs)}.

-spec contains_negate([#effect{}]) -> boolean().
contains_negate(Effects) ->
    lists:any(fun(#effect{kind = Kind}) -> Kind =:= negate end, Effects).

-spec validate_effects([#effect{}], non_neg_integer(), buffs()) -> buffs().
validate_effects(Effects, Depth, Buffs) ->
    lists:foldl(fun(Effect, Acc) -> validate_effect(Effect, Depth, Acc) end, Buffs, Effects).

-spec validate_effect(#effect{}, non_neg_integer(), buffs()) -> buffs().
validate_effect(#effect{kind = Kind, target_count = Count, attack_bp = AttackBp,
                        flat = Flat, buff = Buff, remove_buff_id = RemoveId},
                Depth, Buffs0) ->
    (Count < 1 orelse Count > 256 orelse AttackBp < 0 orelse AttackBp > 1000000 orelse
     not ?VALID_FLAT(Flat)) andalso
        fail(<<"effect target_count, attack_bp, or flat value is outside supported bounds">>),
    Buffs1 = case Kind of
                 add_buff -> validate_buff(Buff, Depth + 1, Buffs0);
                 _ when Buff =/= undefined ->
                     fail(<<"only add_buff effects may contain a buff definition">>);
                 _ -> Buffs0
             end,
    case Kind of
        remove_buff when RemoveId =:= 0 ->
            fail(<<"remove_buff effect requires a non-zero buff id">>);
        remove_buff -> ok;
        _ when RemoveId =/= 0 ->
            fail(<<"only remove_buff effects may contain a remove buff id">>);
        _ -> ok
    end,
    Buffs1.

-spec validate_buff(#buff{} | undefined, non_neg_integer(), buffs()) -> buffs().
validate_buff(undefined, _, _) ->
    fail(<<"add_buff effect requires a buff definition">>);
validate_buff(_, Depth, _) when Depth > ?MAX_TRIGGER_DEPTH ->
    fail(<<"buff definition nesting is too deep">>);
validate_buff(#buff{ident = Ident} = Buff, Depth, {Definitions, Validating, Validated} = Buffs) ->
    case Validated of
        #{Ident := _} -> Buffs;
        _ ->
            maps:is_key(Ident, Validating) andalso
                fail(<<"buff reaction add_buff graph contains an ownership cycle">>),
            #buff{id = Id, permanent = Permanent, duration = Duration,
                  decrement_on = DecrementOn, max_stacks = MaxStacks, mode = Mode,
                  modifiers = Modifiers, reactions = Reactions} = Buff,
            (Id =:= 0 orelse length(Modifiers) > 256 orelse length(Reactions) > 128 orelse
             (not Permanent andalso (Duration < 1 orelse Duration > 10000)) orelse
             (Permanent andalso Duration =/= 0) orelse ?RESPONSE(DecrementOn) orelse
             MaxStacks < 1 orelse MaxStacks > 1000 orelse
             (Mode =:= refresh andalso MaxStacks =/= 1)) andalso
                fail(<<"buff policies are outside supported bounds">>),
            case Definitions of
                #{Id := Known} when Known =/= Ident ->
                    fail(<<"one battle request contains conflicting definitions for a buff id">>);
                _ -> ok
            end,
            lists:foreach(
                fun({_Attribute, add, Value}) when ?VALID_FLAT(Value) -> ok;
                   ({_Attribute, scale_bp, Value}) when ?VALID_MODIFIER(Value) -> ok;
                   (_) -> fail(<<"buff attribute modifier is outside supported bounds">>)
                end,
                Modifiers),
            Inner0 = {Definitions#{Id => Ident}, Validating#{Ident => true}, Validated},
            {Definitions2, Validating2, Validated2} =
                lists:foldl(fun(Reaction, Acc) -> validate_reaction(Reaction, Depth, Acc) end,
                            Inner0, Reactions),
            {Definitions2, maps:remove(Ident, Validating2), Validated2#{Ident => true}}
    end.

-spec validate_reaction(#reaction{}, non_neg_integer(), buffs()) -> buffs().
validate_reaction(#reaction{trigger = Trigger, chance_bp = Chance,
                            max_triggers_per_round = MaxTriggers, effects = Effects},
                  Depth, Buffs) ->
    (?RESPONSE(Trigger) orelse contains_negate(Effects)) andalso
        fail(<<"response triggers and negate effects are only valid in passives">>),
    (not ?VALID_PROBABILITY(Chance) orelse MaxTriggers < 0 orelse MaxTriggers > 10000 orelse
     Effects =:= [] orelse length(Effects) > 64) andalso
        fail(<<"buff reaction is outside supported bounds">>),
    validate_effects(Effects, Depth + 1, Buffs).
