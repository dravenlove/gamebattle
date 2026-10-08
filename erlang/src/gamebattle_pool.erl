-module(gamebattle_pool).
-behaviour(gen_server).

%% Runs battles in fresh processes, at most Workers at a time. Up to MaxQueue
%% more wait their turn; beyond that run/1 answers busy, so the caller can
%% tell the client to retry instead of letting every battle slow down. While
%% battles are coming in it logs the throughput every 10 seconds.

-export([start_link/2, run/1, stats/0]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2]).

-define(REPORT_MS, 10000).

-record(state, {
    workers :: pos_integer(),
    max_queue :: non_neg_integer(),
    running = 0 :: non_neg_integer(),
    queue = queue:new() :: queue:queue(fun(() -> term())),
    queued = 0 :: non_neg_integer(),
    done = 0 :: non_neg_integer(),
    rejected = 0 :: non_neg_integer(),
    reported = {0, 0} :: {non_neg_integer(), non_neg_integer()}
}).

-spec start_link(pos_integer(), non_neg_integer()) -> {ok, pid()} | {error, term()}.
start_link(Workers, MaxQueue) ->
    gen_server:start_link({local, ?MODULE}, ?MODULE, {Workers, MaxQueue}, []).

%% Job runs in a new process; it must send its own reply.
-spec run(fun(() -> term())) -> ok | busy.
run(Job) ->
    gen_server:call(?MODULE, {run, Job}).

-spec stats() -> #{atom() => non_neg_integer()}.
stats() ->
    gen_server:call(?MODULE, stats).

-spec init({pos_integer(), non_neg_integer()}) -> {ok, #state{}}.
init({Workers, MaxQueue}) ->
    _ = erlang:send_after(?REPORT_MS, self(), report),
    {ok, #state{workers = Workers, max_queue = MaxQueue}}.

-spec handle_call(term(), gen_server:from(), #state{}) -> {reply, term(), #state{}}.
handle_call({run, Job}, _From, #state{running = Running, workers = Workers} = State)
        when Running < Workers ->
    {reply, ok, start(Job, State)};
handle_call({run, Job}, _From, #state{queued = Queued, max_queue = MaxQueue} = State)
        when Queued < MaxQueue ->
    {reply, ok, State#state{queue = queue:in(Job, State#state.queue), queued = Queued + 1}};
handle_call({run, _Job}, _From, State) ->
    {reply, busy, State#state{rejected = State#state.rejected + 1}};
handle_call(stats, _From, State) ->
    {reply, #{workers => State#state.workers, running => State#state.running,
              queued => State#state.queued, done => State#state.done,
              rejected => State#state.rejected},
     State};
handle_call(_Request, _From, State) ->
    {reply, {error, unsupported_call}, State}.

-spec handle_cast(term(), #state{}) -> {noreply, #state{}}.
handle_cast(_Request, State) ->
    {noreply, State}.

-spec handle_info(term(), #state{}) -> {noreply, #state{}}.
handle_info({'DOWN', _Ref, process, _Pid, _Reason}, State0) ->
    State = State0#state{running = State0#state.running - 1, done = State0#state.done + 1},
    case queue:out(State#state.queue) of
        {{value, Job}, Rest} ->
            {noreply, start(Job, State#state{queue = Rest, queued = State#state.queued - 1})};
        {empty, _} ->
            {noreply, State}
    end;
handle_info(report, #state{done = Done, rejected = Rejected, reported = {Done0, Rejected0}} = State) ->
    _ = erlang:send_after(?REPORT_MS, self(), report),
    Seconds = ?REPORT_MS / 1000,
    case Done > Done0 orelse Rejected > Rejected0 of
        true ->
            logger:notice("battles: ~.1f/s done, ~.1f/s rejected as busy, ~b running, ~b queued",
                          [(Done - Done0) / Seconds, (Rejected - Rejected0) / Seconds,
                           State#state.running, State#state.queued]);
        false ->
            ok
    end,
    {noreply, State#state{reported = {Done, Rejected}}};
handle_info(_Info, State) ->
    {noreply, State}.

-spec start(fun(() -> term()), #state{}) -> #state{}.
start(Job, #state{running = Running} = State) ->
    _ = spawn_monitor(Job),
    State#state{running = Running + 1}.
