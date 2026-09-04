-module(gamebattle_nif).
-on_load(init/0).

-export([simulate/1, load_config/1]).

-spec init() -> ok | {error, term()}.
init() ->
    erlang:load_nif(filename:join(resolve_priv_dir(), "gamebattle_nif"), 0).

-spec simulate(map()) -> {ok, map()} | {error, map()}.
simulate(_Request) ->
    erlang:nif_error(nif_not_loaded).

-spec load_config(binary()) -> {ok, map()} | {error, map()}.
load_config(_Path) ->
    erlang:nif_error(nif_not_loaded).

-spec resolve_priv_dir() -> file:filename_all().
resolve_priv_dir() ->
    case code:priv_dir(gamebattle) of
        {error, bad_name} ->
            Beam = code:which(?MODULE),
            filename:absname(filename:join([filename:dirname(Beam), "..", "priv"]));
        Directory -> Directory
    end.
