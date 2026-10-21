-module(gamebattle_nif).
-on_load(init/0).

-export([simulate/1, load_config/1]).

-define(LOAD_ERROR, {?MODULE, load_error}).

%% The NIF library is optional: Port-only builds don't have it. Without it the
%% module still loads (a failing on_load would stop a release from booting) and
%% its functions raise {nif_not_loaded, Reason}.
-spec init() -> ok.
init() ->
    case erlang:load_nif(filename:join(resolve_priv_dir(), "gamebattle_nif"), 0) of
        ok -> ok;
        {error, Reason} -> persistent_term:put(?LOAD_ERROR, Reason)
    end.

-spec simulate(map()) -> {ok, map()} | {error, map()}.
simulate(_Request) ->
    erlang:nif_error({nif_not_loaded, persistent_term:get(?LOAD_ERROR, undefined)}).

-spec load_config(binary()) -> {ok, map()} | {error, map()}.
load_config(_Path) ->
    erlang:nif_error({nif_not_loaded, persistent_term:get(?LOAD_ERROR, undefined)}).

-spec resolve_priv_dir() -> file:filename_all().
resolve_priv_dir() ->
    case code:priv_dir(gamebattle) of
        {error, bad_name} ->
            Beam = code:which(?MODULE),
            filename:absname(filename:join([filename:dirname(Beam), "..", "priv"]));
        Directory -> Directory
    end.
