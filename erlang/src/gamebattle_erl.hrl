%% Records shared by the pure-Erlang engine (gamebattle_erl*). They mirror the
%% structs in include/gamebattle/engine.hpp; see that file for field meanings.

-define(BASIS_POINTS, 10000).
-define(MAX_TRIGGER_DEPTH, 32).
-define(INT32_MIN, -16#80000000).
-define(INT32_MAX, 16#7FFFFFFF).
-define(INT64_MIN, -16#8000000000000000).
-define(INT64_MAX, 16#7FFFFFFFFFFFFFFF).
-define(UINT32_MAX, 16#FFFFFFFF).
-define(UINT64_MAX, 16#FFFFFFFFFFFFFFFF).

-record(stats, {
    hp = 1 :: integer(),
    attack = 0 :: integer(),
    defense = 0 :: integer(),
    speed = 0 :: integer(),
    crit_rate_bp = 0 :: integer(),
    crit_damage_bp = 15000 :: integer(),
    hit_rate_bp = 10000 :: integer(),
    dodge_rate_bp = 0 :: integer(),
    damage_bonus_bp = 0 :: integer(),
    damage_reduction_bp = 0 :: integer()
}).

%% `ident` stands in for the C++ BuffSpec pointer: two definitions are the same
%% object only when their idents are equal.
-record(buff, {
    id = 0 :: integer(),
    ident :: term(),
    permanent = false :: boolean(),
    duration = 1 :: integer(),
    decrement_on = round_end :: atom(),
    max_stacks = 1 :: integer(),
    mode = stack :: stack | refresh,
    refresh = reset :: reset | extend | keep,
    modifiers = [] :: [{atom(), add | scale_bp, integer()}],
    reactions = [] :: [tuple()]
}).

-record(effect, {
    kind = damage :: atom(),
    target = enemy_front :: atom(),
    target_count = 1 :: integer(),
    attack_bp = 10000 :: integer(),
    flat = 0 :: integer(),
    buff = undefined :: #buff{} | undefined,
    remove_buff_id = 0 :: integer()
}).

-record(reaction, {
    trigger = round_end :: atom(),
    source = owner :: owner | applier,
    stack_scaling = once :: once | per_stack,
    chance_bp = 10000 :: integer(),
    max_triggers_per_round = 0 :: integer(),
    effects = [] :: [#effect{}]
}).

-record(skill, {
    id = 0 :: integer(),
    chance_bp = 10000 :: integer(),
    priority = 0 :: integer(),
    effects = [] :: [#effect{}]
}).

-record(passive, {
    id = 0 :: integer(),
    trigger = on_damaged :: atom(),
    chance_bp = 10000 :: integer(),
    max_triggers_per_round = 0 :: integer(),
    effects = [] :: [#effect{}]
}).

-record(unit_config, {
    id = 0 :: integer(),
    kind = hero :: hero | beauty | pet | artifact,
    position = 0 :: integer(),
    level = 1 :: integer(),
    can_act = true :: boolean(),
    targetable = true :: boolean(),
    stats = #stats{} :: #stats{},
    skills = [] :: [#skill{}],
    passives = [] :: [#passive{}]
}).

-record(request, {
    battle_id = 0 :: integer(),
    seed = 1 :: integer(),
    max_rounds = 50 :: integer(),
    max_events = 10000 :: integer(),
    attacker_bonus = 0 :: integer(),
    attacker = [] :: [#unit_config{}],
    defender_bonus = 0 :: integer(),
    defender = [] :: [#unit_config{}],
    source_battle_id = 0 :: integer(),
    forced_first_side = undefined :: attacker | defender | undefined,
    unit_states = [] :: [{integer(), integer()}],
    %% The request's `report` option (wire::parse_report_detail).
    report = none :: none | gamebattle_report:detail()
}).

%% A loaded config pack (gamebattle_erl_config).
-record(config, {
    buffs = 0 :: non_neg_integer(),
    effects = 0 :: non_neg_integer(),
    skills = #{} :: #{integer() => #skill{}},
    passives = #{} :: #{integer() => #passive{}}
}).
