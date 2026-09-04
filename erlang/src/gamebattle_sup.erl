-module(gamebattle_sup).
-behaviour(supervisor).

-export([start_link/0, init/1]).

-spec start_link() -> {ok, pid()} | {error, term()}.
start_link() ->
    supervisor:start_link({local, ?MODULE}, ?MODULE, []).

-spec init(term()) -> {ok, {supervisor:sup_flags(), [supervisor:child_spec()]}}.
init([]) ->
    PortWorker = #{
        id => gamebattle_port,
        start => {gamebattle_port, start_link, []},
        restart => permanent,
        shutdown => 5000,
        type => worker,
        modules => [gamebattle_port]
    },
    {ok, {#{strategy => one_for_one, intensity => 5, period => 10}, [PortWorker]}}.
