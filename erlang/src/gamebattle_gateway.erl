-module(gamebattle_gateway).
-behaviour(gen_server).

%% A TCP endpoint for the client protocol (proto/battle_client.proto): every
%% frame is a 4-byte length plus one message, a ClientMessage in and a
%% ServerMessage out. start_battle requests become battles through
%% gamebattle_demo and run in gamebattle_pool, so replies may come back in any
%% order; request_id pairs them up. The engine writes the BattleReport bytes
%% itself, with the detail the client asked for (the `report` request option),
%% and they go out as they are.
%%
%% It is for trying the protocol and load-testing with client/: a real game
%% builds battles from its own player data. Off unless gateway_port (or
%% GAMEBATTLE_GATEWAY_PORT) is set; see child_specs/0 for the settings.

-export([child_specs/0, start_link/2, port/0]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2, terminate/2]).

-define(MAX_FRAME, 65536).

-type engine() :: erlang | nif | port.

%% Settings, each from the application env or else the OS environment:
%%   gateway_port    GAMEBATTLE_GATEWAY_PORT    TCP port; the gateway is off without it
%%   gateway_engine  GAMEBATTLE_ENGINE          erlang (default), nif or port
%%   battle_workers  GAMEBATTLE_BATTLE_WORKERS  battles at a time (default: schedulers)
%%   battle_queue    GAMEBATTLE_BATTLE_QUEUE    battles waiting (default: 8 x workers)
-spec child_specs() -> [supervisor:child_spec()].
child_specs() ->
    case setting(gateway_port, "GAMEBATTLE_GATEWAY_PORT", undefined, fun list_to_integer/1) of
        undefined -> [];
        Port ->
            Engine = setting(gateway_engine, "GAMEBATTLE_ENGINE", erlang, fun engine/1),
            Workers = setting(battle_workers, "GAMEBATTLE_BATTLE_WORKERS",
                              erlang:system_info(schedulers_online), fun list_to_integer/1),
            Queue = setting(battle_queue, "GAMEBATTLE_BATTLE_QUEUE", Workers * 8,
                            fun list_to_integer/1),
            [#{id => gamebattle_pool, start => {gamebattle_pool, start_link, [Workers, Queue]}},
             #{id => ?MODULE, start => {?MODULE, start_link, [Port, Engine]}}]
    end.

-spec setting(atom(), string(), term(), fun((string()) -> term())) -> term().
setting(Key, Variable, Default, Parse) ->
    case application:get_env(gamebattle, Key) of
        {ok, Value} -> Value;
        undefined ->
            case os:getenv(Variable) of
                false -> Default;
                "" -> Default;
                Text -> Parse(Text)
            end
    end.

-spec engine(string()) -> engine().
engine("erlang") -> erlang;
engine("nif") -> nif;
engine("port") -> port.

-spec start_link(inet:port_number(), engine()) -> {ok, pid()} | {error, term()}.
start_link(Port, Engine) ->
    gen_server:start_link({local, ?MODULE}, ?MODULE, {Port, Engine}, []).

%% The port actually listened on (useful when configured as 0).
-spec port() -> inet:port_number().
port() ->
    gen_server:call(?MODULE, port).

-spec init({inet:port_number(), engine()}) -> {ok, map()} | {stop, term()}.
init({Port, Engine}) ->
    Options = [binary, {packet, 4}, {packet_size, ?MAX_FRAME}, {active, false},
               {reuseaddr, true}, {nodelay, true}, {backlog, 1024}],
    case gen_tcp:listen(Port, Options) of
        {ok, Listen} ->
            {ok, Bound} = inet:port(Listen),
            Acceptor = spawn_link(fun() -> accept(Listen, Engine) end),
            logger:notice("battle gateway listening on port ~b (engine ~s)", [Bound, Engine]),
            {ok, #{listen => Listen, port => Bound, acceptor => Acceptor}};
        {error, Reason} ->
            {stop, {gateway_listen_failed, Port, Reason}}
    end.

-spec handle_call(term(), gen_server:from(), map()) -> {reply, term(), map()}.
handle_call(port, _From, #{port := Port} = State) -> {reply, Port, State};
handle_call(_Request, _From, State) -> {reply, {error, unsupported_call}, State}.

-spec handle_cast(term(), map()) -> {noreply, map()}.
handle_cast(_Request, State) -> {noreply, State}.

-spec handle_info(term(), map()) -> {noreply, map()}.
handle_info(_Info, State) -> {noreply, State}.

-spec terminate(term(), map()) -> ok.
terminate(_Reason, #{listen := Listen}) ->
    gen_tcp:close(Listen).

%%% Connections ----------------------------------------------------------------

-spec accept(gen_tcp:socket(), engine()) -> ok.
accept(Listen, Engine) ->
    case gen_tcp:accept(Listen) of
        {ok, Socket} ->
            Connection = spawn(fun() ->
                                   receive {go, S} -> connection(S, Engine) end
                               end),
            _ = case gen_tcp:controlling_process(Socket, Connection) of
                    ok -> Connection ! {go, Socket};
                    {error, _} -> gen_tcp:close(Socket), exit(Connection, kill)
                end,
            accept(Listen, Engine);
        {error, closed} ->
            ok;
        {error, _} ->
            accept(Listen, Engine)
    end.

-spec connection(gen_tcp:socket(), engine()) -> ok.
connection(Socket, Engine) ->
    case inet:setopts(Socket, [{active, once}]) of
        ok -> receive_loop(Socket, Engine);
        {error, _} -> gen_tcp:close(Socket)
    end.

-spec receive_loop(gen_tcp:socket(), engine()) -> ok.
receive_loop(Socket, Engine) ->
    receive
        {tcp, Socket, Frame} ->
            request(Frame, Socket, Engine),
            connection(Socket, Engine);
        {reply, Bytes} ->
            case gen_tcp:send(Socket, Bytes) of
                ok -> receive_loop(Socket, Engine);
                {error, _} -> gen_tcp:close(Socket)
            end;
        {tcp_closed, Socket} ->
            ok;
        {tcp_error, Socket, _} ->
            gen_tcp:close(Socket)
    end.

-spec request(binary(), gen_tcp:socket(), engine()) -> ok.
request(Frame, Socket, Engine) ->
    Reply =
        case gamebattle_client:decode_client_message(Frame) of
            {ok, RequestId, {start_battle, Request}} ->
                Connection = self(),
                Job = fun() -> Connection ! {reply, battle(RequestId, Request, Engine)} end,
                case gamebattle_pool:run(Job) of
                    ok -> none;
                    busy -> gamebattle_client:encode_error(RequestId, retry_later,
                                                           <<"battle server busy">>)
                end;
            {error, RequestId, bad_message} ->
                gamebattle_client:encode_error(RequestId, bad_message, <<"bad request">>)
        end,
    case Reply of
        none -> ok;
        _ -> _ = gen_tcp:send(Socket, Reply), ok
    end.

%% Runs in a pool process; returns the encoded ServerMessage.
-spec battle(non_neg_integer(), map(), engine()) -> binary().
battle(RequestId, #{stage_id := StageId, lineup := Lineup, detail := Detail}, Engine) ->
    try gamebattle_demo:request(StageId, Lineup, rand:uniform(1 bsl 62) - 1) of
        {error, Message} ->
            gamebattle_client:encode_error(RequestId, invalid_request, Message);
        {ok, Battle} ->
            case gamebattle:simulate(Engine, Battle#{report => Detail}) of
                {ok, Result} ->
                    gamebattle_client:encode_battle_report(RequestId, Result);
                {error, Error} ->
                    logger:warning("battle failed: ~p", [Error]),
                    gamebattle_client:encode_error(RequestId, gamebattle_client:error_code(Error),
                                                   <<"battle failed">>)
            end
    catch
        Class:Reason:Stacktrace ->
            logger:error("battle crashed: ~p:~p ~p", [Class, Reason, Stacktrace]),
            gamebattle_client:encode_error(RequestId, internal, <<"battle failed">>)
    end.
