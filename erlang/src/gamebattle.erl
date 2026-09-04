-module(gamebattle).

-export([
    simulate/1,
    simulate/2,
    load_config/1,
    load_config/2,
    carryover/2,
    run_gauntlet/3,
    run_gauntlet/4,
    example_request/0
]).

-type adapter() :: port | nif.
-type result() :: {ok, map()} | {error, map()}.

-spec simulate(map()) -> result().
simulate(Request) ->
    simulate(port, Request).

-spec simulate(adapter(), map()) -> result().
simulate(port, Request) when is_map(Request) ->
    gamebattle_port:simulate(Request);
simulate(nif, Request) when is_map(Request) ->
    gamebattle_nif:simulate(Request).

-spec load_config(file:filename_all()) -> {ok, map()} | {error, map()}.
load_config(Path) ->
    load_config(port, Path).

-spec load_config(adapter(), file:filename_all()) -> {ok, map()} | {error, map()}.
load_config(port, Path) ->
    gamebattle_port:load_config(Path);
load_config(nif, Path) ->
    gamebattle_nif:load_config(unicode:characters_to_binary(Path)).

%% Build sparse runtime overrides for the next battle. Formation definitions
%% remain authoritative for max HP and every other configured attribute.
-spec carryover(attacker | defender, map()) -> map().
carryover(Side, #{battle_id := BattleId, units := Units})
        when Side =:= attacker; Side =:= defender ->
    UnitStates = [
        #{unit_id => maps:get(id, Unit), current_hp => maps:get(hp, Unit)}
        || Unit <- Units, maps:get(side, Unit) =:= Side
    ],
    #{
        source_battle_id => BattleId,
        first_side => automatic,
        unit_states => UnitStates
    }.

-spec run_gauntlet(adapter(), map(), [map()]) -> {ok, map()} | {error, map()}.
run_gauntlet(Adapter, AttackerFormation, Waves) ->
    gamebattle_gauntlet:run(Adapter, AttackerFormation, Waves).

-spec run_gauntlet(adapter(), map(), [map()], map()) -> {ok, map()} | {error, map()}.
run_gauntlet(Adapter, AttackerFormation, Waves, Options) ->
    gamebattle_gauntlet:run(Adapter, AttackerFormation, Waves, Options).

-spec example_request() -> map().
example_request() ->
    #{
        battle_id => 20260902001,
        seed => 778899,
        max_rounds => 20,
        max_events => 5000,
        initial_conditions => #{
            source_battle_id => 0,
            first_side => automatic,
            unit_states => []
        },
        attacker => #{
            formation => crane_wing,
            initiative_bonus => 15,
            units => [
                hero(1001, 1, 120, 1800, 260, 80, [flame_skill()], [poison_passive()]),
                hero(1002, 2, 95, 2100, 220, 105, [], []),
                support(1101, beauty, 10, #{charm => 12}, [beauty_passive()]),
                support(1201, pet, 11, #{star => 8}, []),
                support(1301, artifact, 12, #{refine => 15}, [])
            ]
        },
        defender => #{
            formation => shield_wall,
            initiative_bonus => 5,
            units => [
                hero(2001, 1, 105, 2300, 235, 115, [], [counter_passive()]),
                hero(2002, 2, 90, 1900, 245, 90, [], []),
                support(2101, beauty, 10, #{charm => 10}, [])
            ]
        }
    }.

-spec hero(integer(), integer(), integer(), integer(), integer(), integer(), list(), list()) -> map().
hero(Id, Position, Speed, Hp, Attack, Defense, Skills, Passives) ->
    #{
        id => Id,
        kind => hero,
        position => Position,
        level => 80,
        growth_levels => #{star => 10, breakthrough => 6, equipment => 75},
        final_stats => #{
            hp => Hp,
            attack => Attack,
            defense => Defense,
            speed => Speed,
            crit_rate_bp => 1500,
            crit_damage_bp => 15000,
            hit_rate_bp => 10000,
            dodge_rate_bp => 300,
            damage_bonus_bp => 0,
            damage_reduction_bp => 0
        },
        skills => Skills,
        passives => Passives
    }.

-spec support(integer(), atom(), integer(), map(), list()) -> map().
support(Id, Kind, Position, Growth, Passives) ->
    #{
        id => Id,
        kind => Kind,
        position => Position,
        level => 80,
        can_act => false,
        targetable => false,
        growth_levels => Growth,
        final_stats => #{hp => 1, attack => 0, defense => 0, speed => 0},
        skills => [],
        passives => Passives
    }.

-spec flame_skill() -> map().
flame_skill() ->
    #{
        id => 501,
        name => <<"烈焰斩">>,
        priority => 10,
        chance_bp => 3500,
        effects => [
            #{type => damage, target => all_enemies, attack_bp => 11500, flat => 20}
        ]
    }.

-spec poison_passive() -> map().
poison_passive() ->
    #{
        id => 701,
        name => <<"淬毒">>,
        trigger => on_hit,
        chance_bp => 4000,
        max_triggers_per_round => 1,
        effects => [#{
            type => add_buff,
            target => trigger_unit,
            buff => #{
                id => 801,
                name => <<"中毒">>,
                lifetime => #{
                    type => finite,
                    duration => 2,
                    decrement_on => round_end
                },
                stacking => #{
                    max_stacks => 3,
                    policy => stack,
                    refresh => reset
                },
                modifiers => [],
                reactions => [#{
                    trigger => round_end,
                    source => applier,
                    stack_scaling => per_stack,
                    chance_bp => 10000,
                    max_triggers_per_round => 0,
                    effects => [#{
                        type => direct_damage,
                        target => self,
                        attack_bp => 0,
                        flat => 35
                    }]
                }]
            }
        }]
    }.

-spec beauty_passive() -> map().
beauty_passive() ->
    #{
        id => 702,
        name => <<"鼓舞">>,
        trigger => battle_start,
        chance_bp => 10000,
        effects => [#{
            type => add_buff,
            target => all_allies,
            buff => #{
                id => 802,
                name => <<"攻击提升">>,
                lifetime => #{
                    type => finite,
                    duration => 3,
                    decrement_on => round_end
                },
                stacking => #{
                    max_stacks => 1,
                    policy => refresh,
                    refresh => reset
                },
                modifiers => [#{
                    attribute => attack,
                    operation => add,
                    value => 25
                }],
                reactions => []
            }
        }]
    }.

-spec counter_passive() -> map().
counter_passive() ->
    #{
        id => 703,
        name => <<"反击">>,
        trigger => on_damaged,
        chance_bp => 2500,
        max_triggers_per_round => 1,
        effects => [
            #{type => damage, target => trigger_unit, attack_bp => 5000}
        ]
    }.
