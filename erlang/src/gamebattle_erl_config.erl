-module(gamebattle_erl_config).

%% Loads a .gbcfg config pack for the pure-Erlang engine: a port of
%% ConfigStore::load_file (src/config_store.cpp) that reads, checks and
%% reports errors in the same order and with the same messages.

-include("gamebattle_erl.hrl").

-export([load/1]).

-define(MAX_PACK_BYTES, (64 * 1024 * 1024)).
-define(MAX_RECORDS, 1000000).
-define(MAX_REFERENCES, 4096).
-define(MAX_STRING_BYTES, (1024 * 1024)).
-define(TRIGGERS, {battle_start, round_start, before_action, on_attack, on_hit, on_damaged,
                   unit_death, after_action, round_end, enemy_activate, ally_activate}).
-define(ATTRIBUTES, {attack, defense, speed, crit_rate_bp, crit_damage_bp, hit_rate_bp,
                     dodge_rate_bp, damage_bonus_bp, damage_reduction_bp}).
-define(EFFECT_KINDS, {damage, heal, add_buff, remove_buff, direct_damage, negate}).
-define(TARGETS, {self, trigger_unit, enemy_front, enemy_lowest_hp, ally_lowest_hp,
                  all_enemies, all_allies}).

-spec load(file:filename_all()) -> {ok, #config{}} | {error, binary()}.
load(Path) ->
    try
        {ok, read(Path)}
    catch
        throw:{config_error, Message} -> {error, iolist_to_binary(Message)}
    end.

-spec fail(iodata()) -> no_return().
fail(Message) ->
    throw({config_error, Message}).

-spec read(file:filename_all()) -> #config{}.
read(Path) ->
    Bytes = case file:read_file(Path) of
                {ok, B} -> B;
                {error, _} ->
                    fail([<<"cannot open gamebattle config pack: ">>,
                          unicode:characters_to_binary(Path)])
            end,
    byte_size(Bytes) > ?MAX_PACK_BYTES andalso
        fail(<<"gamebattle config pack exceeds the 64 MiB limit">>),
    byte_size(Bytes) < 16 andalso
        fail(<<"gamebattle config pack is smaller than its header">>),
    <<Magic:4/binary, Major:16/little, Minor:16/little, PayloadSize:32/little,
      Crc:32/little, Payload/binary>> = Bytes,
    Magic =:= <<"GBCF">> orelse fail(<<"invalid gamebattle config magic">>),
    {Major, Minor} =:= {2, 0} orelse
        fail(io_lib:format("unsupported gamebattle config format ~b.~b", [Major, Minor])),
    PayloadSize =:= byte_size(Payload) orelse
        fail(<<"gamebattle config payload length does not match its header">>),
    erlang:crc32(Payload) =:= Crc orelse fail(<<"gamebattle config CRC32 check failed">>),
    parse(Payload).

%%% Reader --------------------------------------------------------------------

-spec u8(binary()) -> {non_neg_integer(), binary()}.
u8(<<V, Rest/binary>>) -> {V, Rest};
u8(_) -> truncated().

-spec u32(binary()) -> {non_neg_integer(), binary()}.
u32(<<V:32/little, Rest/binary>>) -> {V, Rest};
u32(_) -> truncated().

-spec i32(binary()) -> {integer(), binary()}.
i32(<<V:32/little-signed, Rest/binary>>) -> {V, Rest};
i32(_) -> truncated().

-spec i64(binary()) -> {integer(), binary()}.
i64(<<V:64/little-signed, Rest/binary>>) -> {V, Rest};
i64(_) -> truncated().

-spec string(binary()) -> {binary(), binary()}.
string(Bin0) ->
    {Length, Bin1} = u32(Bin0),
    Length > ?MAX_STRING_BYTES andalso fail(<<"config string exceeds the 1 MiB limit">>),
    case Bin1 of
        <<Text:Length/binary, Rest/binary>> -> {Text, Rest};
        _ -> truncated()
    end.

-spec truncated() -> no_return().
truncated() ->
    fail(<<"truncated gamebattle config pack">>).

-spec enum(binary(), tuple(), non_neg_integer(), binary()) -> {atom(), binary()}.
enum(Bin0, Values, Maximum, Field) ->
    {Value, Bin1} = u8(Bin0),
    Value > Maximum andalso fail([Field, <<" contains an unknown enum value">>]),
    {element(Value + 1, Values), Bin1}.

-spec ids(binary()) -> {[pos_integer()], binary()}.
ids(Bin0) ->
    {Count, Bin1} = u32(Bin0),
    (Count =:= 0 orelse Count > ?MAX_REFERENCES) andalso
        fail(<<"effect id list must contain between 1 and 4096 entries">>),
    ids(Count, Bin1, []).

-spec ids(non_neg_integer(), binary(), [pos_integer()]) -> {[pos_integer()], binary()}.
ids(0, Bin, Acc) -> {lists:reverse(Acc), Bin};
ids(Count, Bin0, Acc) ->
    {Id, Bin1} = u32(Bin0),
    Id =:= 0 andalso fail(<<"effect id references must be non-zero">>),
    ids(Count - 1, Bin1, [Id | Acc]).

-spec times(non_neg_integer(), fun((binary()) -> {term(), binary()}), binary()) ->
          {[term()], binary()}.
times(Count, Read, Bin) -> times(Count, Read, Bin, []).

-spec times(non_neg_integer(), fun((binary()) -> {term(), binary()}), binary(), [term()]) ->
          {[term()], binary()}.
times(0, _, Bin, Acc) -> {lists:reverse(Acc), Bin};
times(Count, Read, Bin0, Acc) ->
    {Item, Bin1} = Read(Bin0),
    times(Count - 1, Read, Bin1, [Item | Acc]).

%%% Records -------------------------------------------------------------------

-spec parse(binary()) -> #config{}.
parse(Payload0) ->
    {[BuffCount, ModifierCount, ReactionCount, EffectCount, SkillCount, PassiveCount],
     Payload1} = times(6, fun u32/1, Payload0),
    lists:foreach(fun({Count, Table}) ->
                      Count > ?MAX_RECORDS andalso fail([Table, <<" record count exceeds the limit">>])
                  end,
                  [{BuffCount, <<"buff">>}, {ModifierCount, <<"buff modifier">>},
                   {ReactionCount, <<"buff reaction">>}, {EffectCount, <<"effect">>},
                   {SkillCount, <<"skill">>}, {PassiveCount, <<"passive">>}]),

    {Buffs, Payload2} = read_table(BuffCount, fun read_buff/2, #{}, Payload1),
    {Modifiers, Payload3} =
        read_table(ModifierCount, fun(Bin, Acc) -> read_modifier(Bin, Acc, Buffs) end, #{},
                   Payload2),
    {Reactions, Payload4} =
        read_table(ReactionCount, fun(Bin, Acc) -> read_reaction(Bin, Acc, Buffs) end, #{},
                   Payload3),
    {Effects, Payload5} = read_table(EffectCount, fun read_effect/2, #{}, Payload4),
    {Skills, Payload6} = times(SkillCount, fun read_skill/1, Payload5),
    {Passives, Payload7} = times(PassiveCount, fun read_passive/1, Payload6),
    Payload7 =:= <<>> orelse fail(<<"gamebattle config pack contains trailing bytes">>),

    check_references(Buffs, Reactions, Effects),
    check_negate(Reactions, Effects, Skills, Passives),
    Order = buff_order(Buffs, Reactions, Effects),
    link(Order, Buffs, Modifiers, Reactions, Effects, Skills, Passives).

%% Reads Count records in order. Each Read returns the record's key and value
%% and receives what was read so far, to report duplicates.
-spec read_table(non_neg_integer(), fun((binary(), map()) -> {term(), term(), binary()}),
                 map(), binary()) -> {map(), binary()}.
read_table(0, _, Acc, Bin) -> {Acc, Bin};
read_table(Count, Read, Acc, Bin0) ->
    {Key, Value, Bin1} = Read(Bin0, Acc),
    read_table(Count - 1, Read, Acc#{Key => {map_size(Acc), Value}}, Bin1).

-spec read_buff(binary(), map()) -> {pos_integer(), #buff{}, binary()}.
read_buff(Bin0, Seen) ->
    {Id, Bin1} = u32(Bin0),
    {Name, Bin2} = string(Bin1),
    {Permanent, Bin3} = u8(Bin2),
    Permanent > 1 andalso fail(<<"buff.lifetime.permanent must be 0 or 1">>),
    {Duration, Bin4} = i32(Bin3),
    {DecrementOn, Bin5} = enum(Bin4, ?TRIGGERS, 8, <<"buff.lifetime.decrement_on">>),
    {MaxStacks, Bin6} = i32(Bin5),
    {Mode, Bin7} = enum(Bin6, {stack, refresh}, 1, <<"buff.stacking.mode">>),
    {Refresh, Bin8} = enum(Bin7, {reset, extend, keep}, 2, <<"buff.stacking.refresh">>),
    IsPermanent = Permanent =:= 1,
    ValidLifetime = (IsPermanent andalso Duration =:= 0) orelse
                    (not IsPermanent andalso Duration >= 1 andalso Duration =< 10000),
    (Id =:= 0 orelse Name =:= <<>> orelse not ValidLifetime orelse
     MaxStacks < 1 orelse MaxStacks > 1000 orelse
     (Mode =:= refresh andalso MaxStacks =/= 1)) andalso
        fail(<<"buff fields are outside supported bounds">>),
    maps:is_key(Id, Seen) andalso fail(<<"duplicate id in buffs">>),
    {Id, #buff{id = Id, permanent = IsPermanent, duration = Duration,
               decrement_on = DecrementOn, max_stacks = MaxStacks, mode = Mode,
               refresh = Refresh},
     Bin8}.

-spec read_modifier(binary(), map(), map()) -> {{pos_integer(), pos_integer()}, tuple(), binary()}.
read_modifier(Bin0, Seen, Buffs) ->
    {BuffId, Bin1} = u32(Bin0),
    {Sequence, Bin2} = u32(Bin1),
    {Attribute, Bin3} = enum(Bin2, ?ATTRIBUTES, 8, <<"buff_modifier.attribute">>),
    {Operation, Bin4} = enum(Bin3, {add, scale_bp}, 1, <<"buff_modifier.operation">>),
    {Value, Bin5} = i64(Bin4),
    (BuffId =:= 0 orelse Sequence =:= 0 orelse Sequence > ?MAX_REFERENCES orelse
     Value < -1000000000000 orelse Value > 1000000000000 orelse
     (Operation =:= scale_bp andalso (Value < -1000000 orelse Value > 1000000))) andalso
        fail(<<"buff modifier fields are outside supported bounds">>),
    maps:is_key(BuffId, Buffs) orelse fail(<<"buff modifier references an unknown buff">>),
    maps:is_key({BuffId, Sequence}, Seen) andalso fail(<<"duplicate buff modifier sequence">>),
    {{BuffId, Sequence}, {Attribute, Operation, Value}, Bin5}.

-spec read_reaction(binary(), map(), map()) ->
          {{pos_integer(), pos_integer()}, {#reaction{}, [pos_integer()]}, binary()}.
read_reaction(Bin0, Seen, Buffs) ->
    {BuffId, Bin1} = u32(Bin0),
    {Sequence, Bin2} = u32(Bin1),
    {Trigger, Bin3} = enum(Bin2, ?TRIGGERS, 8, <<"buff_reaction.trigger">>),
    {Source, Bin4} = enum(Bin3, {owner, applier}, 1, <<"buff_reaction.source">>),
    {Scaling, Bin5} = enum(Bin4, {once, per_stack}, 1, <<"buff_reaction.stack_scaling">>),
    {Chance, Bin6} = i32(Bin5),
    {MaxTriggers, Bin7} = i32(Bin6),
    {EffectIds, Bin8} = ids(Bin7),
    (BuffId =:= 0 orelse Sequence =:= 0 orelse Sequence > ?MAX_REFERENCES orelse
     Chance < 0 orelse Chance > 10000 orelse MaxTriggers < 0 orelse MaxTriggers > 10000) andalso
        fail(<<"buff reaction fields are outside supported bounds">>),
    maps:is_key(BuffId, Buffs) orelse fail(<<"buff reaction references an unknown buff">>),
    maps:is_key({BuffId, Sequence}, Seen) andalso fail(<<"duplicate buff reaction sequence">>),
    {{BuffId, Sequence},
     {#reaction{trigger = Trigger, source = Source, stack_scaling = Scaling, chance_bp = Chance,
                max_triggers_per_round = MaxTriggers},
      EffectIds},
     Bin8}.

-spec read_effect(binary(), map()) -> {pos_integer(), {#effect{}, non_neg_integer()}, binary()}.
read_effect(Bin0, Seen) ->
    {Id, Bin1} = u32(Bin0),
    {Kind, Bin2} = enum(Bin1, ?EFFECT_KINDS, 5, <<"effect.kind">>),
    {Target, Bin3} = enum(Bin2, ?TARGETS, 6, <<"effect.target">>),
    {Count, Bin4} = i32(Bin3),
    {AttackBp, Bin5} = i32(Bin4),
    {Flat, Bin6} = i64(Bin5),
    {BuffId, Bin7} = u32(Bin6),
    {RemoveId, Bin8} = u32(Bin7),
    (Id =:= 0 orelse Count < 1 orelse Count > 256 orelse AttackBp < 0 orelse
     AttackBp > 1000000 orelse Flat < -1000000000000 orelse Flat > 1000000000000) andalso
        fail(<<"effect fields are outside supported bounds">>),
    Kind =:= add_buff andalso BuffId =:= 0 andalso fail(<<"add_buff effect has no buff reference">>),
    Kind =/= add_buff andalso BuffId =/= 0 andalso
        fail(<<"only add_buff effects may contain a buff reference">>),
    Kind =:= remove_buff andalso RemoveId =:= 0 andalso
        fail(<<"remove_buff effect has no buff reference">>),
    Kind =/= remove_buff andalso RemoveId =/= 0 andalso
        fail(<<"only remove_buff effects may contain remove_buff_id">>),
    maps:is_key(Id, Seen) andalso fail(<<"duplicate id in effects">>),
    {Id, {#effect{kind = Kind, target = Target, target_count = Count, attack_bp = AttackBp,
                  flat = Flat, remove_buff_id = RemoveId},
          BuffId},
     Bin8}.

-spec read_skill(binary()) -> {{#skill{}, [pos_integer()]}, binary()}.
read_skill(Bin0) ->
    {Id, Bin1} = u32(Bin0),
    {Name, Bin2} = string(Bin1),
    {Chance, Bin3} = i32(Bin2),
    {Priority, Bin4} = i32(Bin3),
    {EffectIds, Bin5} = ids(Bin4),
    (Id =:= 0 orelse Name =:= <<>> orelse Chance < 0 orelse Chance > 10000) andalso
        fail(<<"skill fields are outside supported bounds">>),
    {{#skill{id = Id, chance_bp = Chance, priority = Priority}, EffectIds}, Bin5}.

-spec read_passive(binary()) -> {{#passive{}, [pos_integer()]}, binary()}.
read_passive(Bin0) ->
    {Id, Bin1} = u32(Bin0),
    {Name, Bin2} = string(Bin1),
    {Trigger, Bin3} = enum(Bin2, ?TRIGGERS, 10, <<"passive.trigger">>),
    {Chance, Bin4} = i32(Bin3),
    {MaxTriggers, Bin5} = i32(Bin4),
    {EffectIds, Bin6} = ids(Bin5),
    (Id =:= 0 orelse Name =:= <<>> orelse Chance < 0 orelse Chance > 10000 orelse
     MaxTriggers < 0 orelse MaxTriggers > 10000) andalso
        fail(<<"passive fields are outside supported bounds">>),
    {{#passive{id = Id, trigger = Trigger, chance_bp = Chance,
               max_triggers_per_round = MaxTriggers},
      EffectIds},
     Bin6}.

%%% Cross-checks --------------------------------------------------------------

-spec check_references(map(), map(), map()) -> ok.
check_references(Buffs, Reactions, Effects) ->
    lists:foreach(
      fun({_, {_, {#effect{kind = Kind, remove_buff_id = RemoveId}, BuffId}}}) ->
              Kind =:= add_buff andalso not maps:is_key(BuffId, Buffs) andalso
                  fail(<<"add_buff effect references an unknown buff">>),
              Kind =:= remove_buff andalso not maps:is_key(RemoveId, Buffs) andalso
                  fail(<<"remove_buff effect references an unknown buff">>)
      end,
      in_file_order(Effects)),
    lists:foreach(
      fun({_, {_, {_, EffectIds}}}) ->
              lists:all(fun(Id) -> maps:is_key(Id, Effects) end, EffectIds) orelse
                  fail(<<"buff reaction references an unknown effect">>)
      end,
      in_file_order(Reactions)).

-spec check_negate(map(), map(), list(), list()) -> ok.
check_negate(Reactions, Effects, Skills, Passives) ->
    AnyNegate = fun(Ids) ->
                    lists:any(fun(Id) ->
                                  case Effects of
                                      #{Id := {_, {#effect{kind = negate}, _}}} -> true;
                                      _ -> false
                                  end
                              end,
                              Ids)
                end,
    lists:foreach(
      fun({_, {_, {#reaction{trigger = Trigger}, Ids}}}) ->
              (response(Trigger) orelse AnyNegate(Ids)) andalso
                  fail(<<"buff reactions cannot use response triggers or negate effects">>)
      end,
      in_file_order(Reactions)),
    lists:foreach(fun({_, Ids}) ->
                      AnyNegate(Ids) andalso fail(<<"skills cannot contain negate effects">>)
                  end,
                  Skills),
    lists:foreach(fun({#passive{trigger = Trigger}, Ids}) ->
                      not response(Trigger) andalso AnyNegate(Ids) andalso
                          fail(<<"negate effects require an enemy_activate or ally_activate passive">>)
                  end,
                  Passives).

-spec response(atom()) -> boolean().
response(Trigger) -> Trigger =:= enemy_activate orelse Trigger =:= ally_activate.

-spec in_file_order(map()) -> [{term(), {non_neg_integer(), term()}}].
in_file_order(Table) ->
    lists:sort(fun({_, {A, _}}, {_, {B, _}}) -> A =< B end, maps:to_list(Table)).

%% A reaction's add_buff effects own the buff they add, so the graph must be
%% acyclic. Returns the buffs in an order where every buff comes after the
%% buffs its reactions add, so it can be built from them.
-spec buff_order(map(), map(), map()) -> [pos_integer()].
buff_order(Buffs, Reactions, Effects) ->
    Edges = lists:usort(
              [{BuffId, Added}
               || {{BuffId, _}, {_, {_, EffectIds}}} <- maps:to_list(Reactions),
                  EffectId <- EffectIds,
                  {_, {#effect{kind = add_buff}, Added}} <- [maps:get(EffectId, Effects)]]),
    InDegree0 = maps:from_list([{Id, 0} || Id <- maps:keys(Buffs)]),
    InDegree = lists:foldl(fun({_, To}, Acc) -> maps:update_with(To, fun(D) -> D + 1 end, Acc) end,
                           InDegree0, Edges),
    Out = lists:foldl(fun({From, To}, Acc) ->
                          maps:update_with(From, fun(L) -> [To | L] end, [To], Acc)
                      end,
                      #{}, Edges),
    Ready = [Id || {Id, 0} <- lists:sort(maps:to_list(InDegree))],
    Visited = kahn(Ready, InDegree, Out, []),
    length(Visited) =:= map_size(Buffs) orelse
        fail(<<"buff reaction add_buff graph contains an ownership cycle">>),
    lists:reverse(Visited).

-spec kahn([pos_integer()], map(), map(), [pos_integer()]) -> [pos_integer()].
kahn([], _, _, Visited) -> lists:reverse(Visited);
kahn([Id | Rest], InDegree0, Out, Visited) ->
    {Next, InDegree} =
        lists:foldl(fun(To, {Acc, Degrees}) ->
                        D = maps:get(To, Degrees) - 1,
                        {case D of 0 -> [To | Acc]; _ -> Acc end, Degrees#{To := D}}
                    end,
                    {[], InDegree0}, lists:reverse(maps:get(Id, Out, []))),
    kahn(Rest ++ lists:reverse(Next), InDegree, Out, [Id | Visited]).

%%% Linking -------------------------------------------------------------------

-spec link([pos_integer()], map(), map(), map(), map(), list(), list()) -> #config{}.
link(Order, Buffs, Modifiers, Reactions, Effects, Skills, Passives) ->
    ModifiersByBuff = group(Modifiers),
    ReactionsByBuff = group(Reactions),
    Built = lists:foldl(
              fun(BuffId, Acc) ->
                  {_, Buff} = maps:get(BuffId, Buffs),
                  BuffReactions =
                      [Reaction#reaction{effects = [effect(Id, Effects, Acc) || Id <- Ids]}
                       || {Reaction, Ids} <- maps:get(BuffId, ReactionsByBuff, [])],
                  Acc#{BuffId => Buff#buff{ident = make_ref(),
                                           modifiers = maps:get(BuffId, ModifiersByBuff, []),
                                           reactions = BuffReactions}}
              end,
              #{}, Order),
    SkillMap = lists:foldl(
                 fun({#skill{id = Id} = Skill, Ids}, Acc) ->
                     Linked = [effect(E, Effects, Built) || E <- Ids],
                     maps:is_key(Id, Acc) andalso fail(<<"duplicate id in skills">>),
                     Acc#{Id => Skill#skill{effects = Linked}}
                 end,
                 #{}, Skills),
    PassiveMap = lists:foldl(
                   fun({#passive{id = Id} = Passive, Ids}, Acc) ->
                       Linked = [effect(E, Effects, Built) || E <- Ids],
                       maps:is_key(Id, Acc) andalso fail(<<"duplicate id in passives">>),
                       Acc#{Id => Passive#passive{effects = Linked}}
                   end,
                   #{}, Passives),
    #config{buffs = map_size(Buffs), effects = map_size(Effects),
            skills = SkillMap, passives = PassiveMap}.

%% Values grouped by buff id, in sequence order.
-spec group(map()) -> #{pos_integer() => [term()]}.
group(Table) ->
    lists:foldr(fun({{BuffId, _}, {_, Value}}, Acc) ->
                    maps:update_with(BuffId, fun(L) -> [Value | L] end, [Value], Acc)
                end,
                #{}, lists:sort(maps:to_list(Table))).

-spec effect(pos_integer(), map(), map()) -> #effect{}.
effect(Id, Effects, Built) ->
    case Effects of
        #{Id := {_, {#effect{kind = add_buff} = Effect, BuffId}}} ->
            Effect#effect{buff = maps:get(BuffId, Built)};
        #{Id := {_, {Effect, _}}} ->
            Effect;
        _ ->
            fail([<<"unknown effect id ">>, integer_to_binary(Id)])
    end.
