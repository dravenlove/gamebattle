-module(gamebattle_port).
-behaviour(gen_server).

-export([start_link/0, start_link/1, simulate/1, load_config/1, ping/0]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2, terminate/2, code_change/3]).

-record(state, {
    port :: port(),
    executable :: file:filename_all(),
    timeout = 30000 :: pos_integer()
}).

-type response() :: {ok, map()} | {error, map()}.

-spec start_link() -> {ok, pid()} | {error, term()}.
start_link() ->
    start_link(#{}).

-spec start_link(map()) -> {ok, pid()} | {error, term()}.
start_link(Options) ->
    gen_server:start_link({local, ?MODULE}, ?MODULE, Options, []).

-spec simulate(map()) -> response().
simulate(Request) when is_map(Request) ->
    gen_server:call(?MODULE, {request, Request}, infinity).

-spec load_config(file:filename_all()) -> {ok, map()} | {error, map()}.
load_config(Path) ->
    gen_server:call(
        ?MODULE,
        {request, {load_config, unicode:characters_to_binary(Path)}},
        infinity
    ).

-spec ping() -> {ok, pong} | {error, term()}.
ping() ->
    gen_server:call(?MODULE, {request, ping}, infinity).

-spec init(map()) -> {ok, #state{}} | {stop, term()}.
init(Options) ->
    process_flag(trap_exit, true),
    Executable = maps:get(executable, Options, resolve_executable()),
    Timeout = maps:get(timeout, Options, application:get_env(gamebattle, port_timeout, 30000)),
    case filelib:is_regular(Executable) of
        true ->
            Port = open_port(
                {spawn_executable, filename:absname(Executable)},
                port_options()
            ),
            {ok, #state{port = Port, executable = Executable, timeout = Timeout}};
        false ->
            {stop, {port_executable_not_found, Executable}}
    end.

-spec handle_call(term(), {pid(), term()}, #state{}) ->
    {reply, term(), #state{}} | {stop, term(), term(), #state{}}.
handle_call({request, Request}, _From, State = #state{port = Port, timeout = Timeout}) ->
    true = port_command(Port, term_to_binary(Request)),
    receive
        {Port, {data, ResponseBinary}} ->
            {reply, decode_response(ResponseBinary), State};
        {Port, {exit_status, Status}} ->
            {stop, {port_exit, Status}, {error, #{type => port_exit, status => Status}}, State};
        {'EXIT', Port, Reason} ->
            {stop, {port_exit, Reason}, {error, #{type => port_exit, reason => Reason}}, State}
    after Timeout ->
        close_port_safely(Port),
        {stop, port_timeout, {error, #{type => timeout, timeout_ms => Timeout}}, State}
    end;
handle_call(Request, _From, State) ->
    {reply, {error, #{type => unsupported_call, request => Request}}, State}.

-spec handle_cast(term(), #state{}) -> {noreply, #state{}}.
handle_cast(_Request, State) ->
    {noreply, State}.

-spec handle_info(term(), #state{}) -> {noreply, #state{}} | {stop, term(), #state{}}.
handle_info({Port, {exit_status, Status}}, State = #state{port = Port}) ->
    {stop, {port_exit, Status}, State};
handle_info({'EXIT', Port, Reason}, State = #state{port = Port}) ->
    {stop, {port_exit, Reason}, State};
handle_info(_Info, State) ->
    {noreply, State}.

-spec terminate(term(), #state{}) -> ok.
terminate(_Reason, #state{port = Port}) ->
    close_port_safely(Port),
    ok.

-spec code_change(term(), #state{}, term()) -> {ok, #state{}}.
code_change(_OldVersion, State, _Extra) ->
    {ok, State}.

-spec decode_response(binary()) -> term().
decode_response(Binary) ->
    try binary_to_term(Binary) of
        Response -> Response
    catch
        error:badarg -> {error, #{type => invalid_port_response}}
    end.

-spec resolve_executable() -> file:filename_all().
resolve_executable() ->
    case application:get_env(gamebattle, port_executable) of
        {ok, Path} -> Path;
        undefined ->
            case os:getenv("GAMEBATTLE_PORT") of
                false -> filename:join(resolve_priv_dir(), executable_name());
                Path -> Path
            end
    end.

-spec resolve_priv_dir() -> file:filename_all().
resolve_priv_dir() ->
    case code:priv_dir(gamebattle) of
        {error, bad_name} ->
            Beam = code:which(?MODULE),
            filename:absname(filename:join([filename:dirname(Beam), "..", "priv"]));
        Directory -> Directory
    end.

-spec executable_name() -> string().
executable_name() ->
    case os:type() of
        {win32, _} -> "gamebattle_port.exe";
        _ -> "gamebattle_port"
    end.

-spec port_options() -> list().
port_options() ->
    Common = [binary, {packet, 4}, use_stdio, exit_status],
    case os:type() of
        {win32, _} -> [hide | Common];
        _ -> Common
    end.

-spec close_port_safely(port()) -> ok.
close_port_safely(Port) ->
    try port_close(Port) of
        true -> ok
    catch
        error:badarg -> ok
    end.
