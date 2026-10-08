-module(gamebattle_demo).

%% Demo stages for the gateway and the test client (client/). A real game
%% builds each battle from the player's own data; these stages stand in for
%% that, so the whole path can be tried out and load-tested:
%%
%%   1  standard      the lineup against five heroes; ends within a few rounds
%%   2  stress-mixed  7v7, 40 passives per hero on mixed triggers, at most
%%                    3 times a round each; 30 rounds, nobody dies
%%   3  stress-chain  7v7, 20 passives per hero, all set off by hits and
%%                    dealing damage, at most 3 times a round; 30 rounds
%%
%% In the stress stages nobody dies, which stands in for revives keeping
%% everyone in the fight. The lineup picks heroes 1-7 of the demo roster and
%% their positions; the defenders are fixed.

-export([request/3, stage_ids/0]).

-define(ROSTER_SIZE, 7).

-type lineup() :: [#{unit_id := pos_integer(), position := integer()}].

-spec stage_ids() -> [pos_integer()].
stage_ids() -> [1, 2, 3].

-spec request(non_neg_integer(), lineup(), non_neg_integer()) -> {ok, map()} | {error, binary()}.
request(StageId, Lineup, Seed) ->
    case lists:member(StageId, stage_ids()) of
        false ->
            {error, <<"unknown stage">>};
        true when length(Lineup) > ?ROSTER_SIZE ->
            {error, <<"too many units">>};
        true ->
            case [Id || #{unit_id := Id} <- Lineup, Id > ?ROSTER_SIZE] of
                [] -> {ok, battle(StageId, Lineup, Seed)};
                _ -> {error, <<"unknown unit">>}
            end
    end.

-spec battle(pos_integer(), lineup(), non_neg_integer()) -> map().
battle(StageId, Lineup, Seed) ->
    Defenders = case StageId of
                    1 -> 5;
                    _ -> 7
                end,
    #{battle_id => erlang:unique_integer([positive, monotonic]),
      seed => Seed,
      max_rounds => case StageId of 1 -> 50; _ -> 30 end,
      max_events => 1000000,
      attacker => #{units => [hero(StageId, 1000 + Id, Id, Position)
                              || #{unit_id := Id, position := Position} <- Lineup]},
      defender => #{units => [hero(StageId, 2000 + N, N, N) || N <- lists:seq(1, Defenders)]}}.

-spec hero(pos_integer(), pos_integer(), pos_integer(), integer()) -> map().
hero(StageId, Id, Template, Position) ->
    Hp = case StageId of
             1 -> 3000 + Template * 400;
             _ -> 1000000000
         end,
    #{id => Id, kind => hero, position => Position, level => 80,
      final_stats => #{hp => Hp, attack => 280 + Template * 20,
                       defense => 80 + (Template rem 3) * 30, speed => 90 + Template * 5,
                       crit_rate_bp => 1500, crit_damage_bp => 15000, dodge_rate_bp => 500},
      skills => [#{id => 500 + Template, chance_bp => 3000,
                   effects => [#{type => damage, target => all_enemies, attack_bp => 6000}]}],
      passives => passives(StageId, Id)}.

-spec passives(pos_integer(), pos_integer()) -> [map()].
passives(1, Id) ->
    [#{id => 701, trigger => on_hit, chance_bp => 3000, max_triggers_per_round => 2,
       effects => [#{type => heal, target => self, flat => 120}]},
     #{id => 702, trigger => on_damaged, chance_bp => 2500, max_triggers_per_round => 1,
       effects => [#{type => damage, target => trigger_unit, attack_bp => 4000}]},
     #{id => 703, trigger => battle_start,
       effects => [#{type => add_buff, target => self, buff => attack_up(Id * 100 + 1)}]}];
passives(2, Id) ->
    Triggers = [on_damaged, on_hit, on_attack, before_action, after_action, round_start,
                unit_death],
    [#{id => 700 + K, trigger => lists:nth((K rem length(Triggers)) + 1, Triggers),
       chance_bp => 5000, max_triggers_per_round => 3,
       effects => [case K rem 4 of
                       0 -> hit(all_enemies);
                       1 -> hit(enemy_lowest_hp);
                       2 -> #{type => heal, target => ally_lowest_hp, flat => 50};
                       3 -> #{type => add_buff, target => self, buff => attack_up(Id * 100 + K)}
                   end]}
     || K <- lists:seq(1, 40)];
passives(3, _Id) ->
    [#{id => 700 + K, trigger => case K rem 2 of 0 -> on_damaged; 1 -> on_hit end,
       chance_bp => 5000, max_triggers_per_round => 3,
       effects => [case K rem 2 of
                       0 -> hit(all_enemies);
                       1 -> hit(enemy_lowest_hp)
                   end]}
     || K <- lists:seq(1, 20)].

-spec hit(atom()) -> map().
hit(Target) ->
    #{type => direct_damage, target => Target, flat => 10}.

-spec attack_up(pos_integer()) -> map().
attack_up(BuffId) ->
    #{id => BuffId, name => <<"attack_up">>,
      lifetime => #{type => finite, duration => 2, decrement_on => round_end},
      stacking => #{max_stacks => 3, policy => stack, refresh => reset},
      modifiers => [#{attribute => attack, operation => scale_bp, value => 200}],
      reactions => []}.
