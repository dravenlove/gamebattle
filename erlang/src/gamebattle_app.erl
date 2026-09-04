-module(gamebattle_app).
-behaviour(application).

-export([start/2, stop/1]).

-spec start(application:start_type(), term()) ->
    {ok, pid()} | {ok, pid(), term()} | {error, term()}.
start(_StartType, _StartArgs) ->
    gamebattle_sup:start_link().

-spec stop(term()) -> ok.
stop(_State) ->
    ok.
