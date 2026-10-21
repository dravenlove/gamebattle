# Lesson 19: epoll and a TCP battle server

[中文](../19-epoll-battle-server.md) | **English**

> The first lesson of part 4, "Networking and game servers". Practice code: `practice/battle_tcp_server.cpp` (about 300 lines), `practice/tcp_client.py`.
>
> Game-server roles almost always test network programming. This lesson wraps the battle engine as a standalone TCP service: a Linux epoll event loop + a thread pool, with the same `{packet, 4}` framing as the Erlang Port, so **an Erlang node can connect directly with `gen_tcp`**.

## 1. Why make it a TCP service

A Port is "one Erlang node, one C++ process", a single pipe. As scale grows, a common deployment is to split battle computation into its own service:

```
 Erlang logic node A ──┐                           ┌── battle server 1 (4 cores, 4 worker threads)
 Erlang logic node B ──┼── TCP, {packet, 4} ───────┼── battle server 2
 Erlang logic node C ──┘   sharded by battle_id    └── battle server 3
```

- Battle servers can scale out independently and be deployed on machines with stronger CPUs;
- a battle server crashing doesn't affect the logic nodes, which can retry the request on another battle server;
- the protocol is exactly the same as the Port's (`{packet, 4}` + ETF), so **not a single line of engine code changes**; it reuses `wire::handle_etf`.

## 2. Socket basics

```cpp
int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);   // create a TCP socket
::bind(fd, address, ...);                                                    // bind the address and port
::listen(fd, SOMAXCONN);                                                     // start listening; SOMAXCONN is the backlog length
int client = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);  // accept a connection
::recv(client, buffer, size, 0);                                             // read
::send(client, data, size, MSG_NOSIGNAL);                                    // write
::close(client);
```

- **Blocking vs non-blocking**: calling `recv` on a blocking socket waits forever when there's no data. A non-blocking socket returns -1 immediately when there's no data, with `errno` set to `EAGAIN` (or `EWOULDBLOCK`). Event-driven servers must use non-blocking sockets.
- `SOCK_CLOEXEC`: closed automatically when `exec` starts a child process, so file descriptors don't leak into children.
- `MSG_NOSIGNAL`: writing to a connection the peer has closed raises a `SIGPIPE` signal by default, and **the default action is to kill the process outright**. With this flag it just returns an error. Not handling `SIGPIPE` is a classic reason beginners' servers "mysteriously exit".

These system calls all return an integer **file descriptor** (fd) that must be `close`d after use. The practice code manages them with lesson 13's `FileDescriptor` class (RAII, move-only), so they neither leak nor get closed twice.

## 3. I/O multiplexing: from one thread per connection to epoll

**One thread per connection**: the simplest, but ten thousand connections means ten thousand threads, and neither memory nor context switching can cope.

**I/O multiplexing**: one thread watches many fds at once and handles whichever becomes readable or writable.

| | select | poll | epoll (Linux) |
|---|---|---|---|
| fd limit | 1024 by default | None | None |
| Each call | Copies the whole fd set into the kernel | Same | Not needed: the interest list lives in the kernel |
| Result | You scan every fd to find the ready ones | Same | **Returns only the ready fds** |
| Complexity | O(total connections) | O(total connections) | O(ready connections) |

epoll's three calls:

```cpp
int epoll = ::epoll_create1(EPOLL_CLOEXEC);                      // create an epoll instance

epoll_event event{};
event.events = EPOLLIN | EPOLLRDHUP;                             // interested in: readable, peer closed
event.data.u64 = connection_id;                                  // data handed back unchanged when the event fires
::epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &event);                   // add to the interest list (MOD modifies, DEL removes)

epoll_event ready[64];
int n = ::epoll_wait(epoll, ready, 64, -1);                      // sleep until there are events; returns how many are ready
```

`data.u64` holds a **connection ID**, not a pointer. When an event comes back, the ID is looked up in `connections_`; if it's not found, the connection has already closed (lesson 2's "unique ID + lookup").

**What does this have to do with Erlang?** Inside, the BEAM VM uses epoll (on Linux) to watch every port and socket. The `gen_tcp` with `{active, once}` you write in Erlang sits on exactly this mechanism; the VM has just written the event loop for you and turns events into messages sent to processes.

### Level-triggered (LT) and edge-triggered (ET)

- **Level-triggered** (the default): as long as the socket still has unread data, every `epoll_wait` reports it.
- **Edge-triggered** (`EPOLLET`): reports only once, when the state **changes**. If you don't read all the data this time, you'll never be told again, so you **must loop until `EAGAIN`**.

The practice code is level-triggered but still loops reads until `EAGAIN`, which is correct in both modes. ET saves some repeated notifications, at the cost that a single missed read leaves the connection stuck forever.

## 4. The overall structure: one Reactor thread + a thread pool

```
                         ┌─────────────── Reactor thread (the sole owner of every socket) ──────────────────┐
 client ── TCP ─────────▶│ epoll_wait                                                                       │
                         │  ├─ listening socket readable → accept4, add to epoll                            │
                         │  ├─ connection readable       → recv → cut out whole frames → connection pending │
                         │  │                              → connection idle? hand to the thread pool ──┐   │
                         │  ├─ eventfd readable          → take every finished result → write back ◀──┐ │   │
                         │  ├─ connection writable       → keep sending unsent data                   │ │   │
                         │  └─ signalfd readable         → got SIGTERM, leave the loop                │ │   │
                         └────────────────────────────────────────────────────────────────────────────┼─┼───┘
                                                                                                      │ ▼
                         ┌──────────── thread pool (4 worker threads) ──────────────────────────────────────┐
                         │  wire::handle_etf(request) → result to completion queue (locked) → write eventfd │
                         └──────────────────────────────────────────────────────────────────────────────────┘
```

This is the **Reactor pattern**: one thread "waits for events and dispatches them", and expensive computation goes to other threads. The key design points:

- **Only the Reactor thread touches sockets and connection state**, so `connections_` needs no lock. Worker threads only call `handle_etf` (shown thread-safe in lesson 17) and put the result into a locked completion queue.
- **How do workers notify the Reactor?** The Reactor is asleep in `epoll_wait`. An `eventfd` is a kernel counter that is itself an fd: when a worker writes a number to it, it becomes "readable" and `epoll_wait` returns. One fd turns "cross-thread notification" into "just another I/O event".
- **How are signals handled?** `signalfd` turns signals into an fd too. `main` blocks SIGINT/SIGTERM **before creating any threads** (new threads inherit the signal mask), so signals enter only through the `signalfd` in the event loop instead of interrupting some arbitrary thread. On receipt, the loop exits and every object is destroyed normally.

## 5. Framing: TCP is a byte stream

**TCP doesn't preserve message boundaries.** Two `send`s may arrive in a single `recv` on the other side (**coalesced packets**, often called "sticky packets"); one 8 KB message may take several reads to arrive in full (**partial packets**). So you must define for yourself "where a message starts and ends".

The most common approach is a **length prefix**, which is exactly Erlang's `{packet, 4}`: a 4-byte big-endian length + the body.

```cpp
bool read_frames(Connection& connection) {
    // 1. read everything in the kernel buffer and append it to connection.input
    while (true) {
        const auto received = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (received > 0) { connection.input.insert(connection.input.end(), ...); continue; }
        if (received == 0) { peer_closed = true; break; }                   // the peer closed
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;                 // all read
        if (errno == EINTR) continue;
        return false;                                                       // an error
    }
    // 2. cut every complete frame out of input
    std::size_t consumed = 0;
    while (input.size() - consumed >= 4) {
        const std::uint32_t length = big_endian(input[consumed..consumed+4]);
        if (length == 0 || length > kMaxFrameBytes) return false;            // invalid length: disconnect
        if (input.size() - consumed - 4 < length) break;                    // partial packet: wait for more data
        connection.pending.emplace_back(the frame body);
        consumed += 4 + length;
    }
    input.erase(input.begin(), input.begin() + consumed);                    // only one erase per read
    return !peer_closed;
}
```

A few details:
- **Read everything first, then cut frames, then `erase` once at the end.** Erasing after each frame would shift the remaining data forward every time, making N frames O(N²).
- The length limit is checked **before allocating memory** (lesson 9's principle): if the peer claims to send 4 GB, disconnect immediately.
- `input.size() - consumed >= 4` rather than `consumed + 4 <= input.size()`: the habit from lesson 7, "the check itself must not overflow".

Measured, all three cases are handled correctly:

```
8 connections, each sending 25 frames in one sendall (coalesced)  → all 200 replies correct
one ping frame sent byte by byte, 20 ms between bytes (partial)   → reply: 83 68 02 77 02 6f 6b 77 04 70 6f 6e 67  ({ok, pong})
a length header of 0xFFFFFFFF                                     → the server disconnects immediately
```

## 6. Writing: the send buffer and `EPOLLOUT`

`send` may also send only part of the data: when the kernel's send buffer is full (the client reads slowly), only part goes out, and the rest returns `EAGAIN`.

```cpp
bool flush(Connection& connection) {
    while (connection.output_sent < connection.output.size()) {
        const auto sent = ::send(fd, output.data() + output_sent, output.size() - output_sent, MSG_NOSIGNAL);
        if (sent > 0) { connection.output_sent += sent; continue; }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;   // buffer full; wait for EPOLLOUT
        if (sent < 0 && errno == EINTR) continue;
        return false;
    }
    connection.output.clear();
    connection.output_sent = 0;
    return true;
}
```

Unsent data stays in `output`, and sending resumes when the socket becomes writable again (`EPOLLOUT`).

**When should you watch for `EPOLLOUT`?** Only when there really is unsent data. A socket is almost always writable; if you always watch `EPOLLOUT`, `epoll_wait` keeps returning in level-triggered mode and the CPU spins at 100%. The practice code **derives** the events of interest from the connection's state:

```cpp
void update_interest(std::uint64_t id, Connection& connection) {
    std::uint32_t wanted = EPOLLRDHUP;
    if (connection.pending.size() < kMaxPendingFrames) wanted |= EPOLLIN;      // keep reading only while pending requests aren't full
    if (connection.output_sent < connection.output.size()) wanted |= EPOLLOUT; // watch writability only with unsent data
    if (wanted == connection.interest) return;                                 // unchanged: skip epoll_ctl
    ...epoll_ctl(EPOLL_CTL_MOD, ...);
}
```

Calling it once after every state change means you never forget to turn an event on or off.

## 7. Backpressure and ordering

**Backpressure**: if a client sends requests faster than the server processes them, requests pile up without limit until memory runs out. The practice code's approach: when a connection has 64 pending requests, **stop reading from that connection** (remove `EPOLLIN` from the interest list). The data stays in the kernel's receive buffer; once that buffer fills, TCP flow control makes the client's `send` block or return `EAGAIN`. The pressure travels all the way back to the sender. It's exactly the idea behind Erlang's `{active, once}`: ask for the next message only after handling one.

**Ordering**: in the `{packet, 4}` protocol, requests and replies carry no IDs, and the client matches replies to requests by order. If several requests from one connection ran in the thread pool at once, a later one could finish first and scramble the order. The practice code allows **only one request per connection in flight at a time** (the `busy` flag), dispatching the next only when one finishes. Just like `gamebattle_port`'s gen_server: each worker handles one battle at a time. Different connections still run in parallel.

**What if a connection drops while its task is running?** When the result comes back it's looked up by connection ID, and dropped if not found:

```cpp
auto found = connections_.find(completion.connection_id);
if (found == connections_.end()) continue;   // the client left while the battle was being computed
```

Had the worker held a `Connection&` or a pointer, it would become a dangling reference once the connection was erased.

**Member declaration order**: the thread pool `pool_` is `Server`'s **last member**. On destruction it's destroyed first: only after every worker thread has exited are the `completions_mutex_`, `completions_` and `wakeup_` they use destroyed (lessons 2, 17).

## 8. Socket options you must know

| Option | Effect | What happens without it |
|---|---|---|
| `TCP_NODELAY` | Disables Nagle's algorithm so small data is sent immediately | Nagle batches small packets, and combined with the peer's delayed ACK you may wait an extra ~40 ms. Almost every game server turns it on |
| `SO_REUSEADDR` | Allows binding a port in TIME_WAIT | For tens of seconds after a restart, `bind` fails with "Address already in use" |
| `MSG_NOSIGNAL` | No SIGPIPE when writing to a closed connection | The process is killed by SIGPIPE |
| `SO_KEEPALIVE` | The kernel periodically probes idle connections to check they're alive | If the peer loses power, the dead connection is never noticed (the default probe interval is 2 hours; games usually use application-level heartbeats) |

## 9. Measured results

On a 4-core machine with 4 worker threads:

```
8 connections × 25 battles pipelined each = 200 battles in 0.15 s, 0 errors
every reply byte-identical (same seed, computed on different threads)
300 battles serially on one connection: p50=2.10 ms  p99=4.23 ms  max=6.86 ms
clean exit after SIGTERM: accepted 10 connections, served 203 requests
the same test on a ThreadSanitizer build: 0 warnings
```

The system calls the server made during this test, counted with `strace -f -c` (excerpt):

```
calls  syscall
  753  futex          ← the thread pool's lock and condition variable
 1566  mprotect       ← multithreaded malloc growing heap memory (lesson 21)
   62  epoll_wait     ← only 62 calls for 203 requests: one call returns several ready events
  203  sendto         ← one per reply
  205  write          ← 203 are workers writing the eventfd to notify the Reactor, the other 2 are log output
   58  read           ← mostly the Reactor reading the eventfd: 203 writes merged into a few dozen reads
   44  recvfrom       ← the 25 pipelined frames were often read in one go
   13  accept4
   13  epoll_ctl      ← the interest list is modified only when it really changes
```

This table is great for an interview:
- **epoll's batching effect**: 203 requests needed only 62 wakeups;
- **eventfd's merging**: the workers wrote 203 times, but the Reactor read only a few dozen times, because an eventfd is a counter: multiple writes add up, and one read takes them all;
- **the biggest system-call cost isn't in networking** but in `futex` (locks) and `mprotect` (memory allocation). That says the next thing to optimize is the thread pool's lock contention and each battle's memory allocations (lessons 16, 21), not the networking.

## 10. Connecting to this server from Erlang

`gen_tcp`'s `{packet, 4}` matches the server's framing exactly (the code below wasn't actually run, as this machine has no Erlang installed):

```erlang
{ok, Socket} = gen_tcp:connect("127.0.0.1", 9000, [binary, {packet, 4}, {active, false}]),
ok = gen_tcp:send(Socket, term_to_binary(gamebattle:example_request())),
{ok, Reply} = gen_tcp:recv(Socket, 0, 30000),
{ok, Result} = binary_to_term(Reply),
#{winner := Winner, rounds := Rounds} = Result.
```

`gen_tcp` adds and strips the length header automatically, so what you get is a complete ETF binary. In production you might use a connection pool (say N connections per battle server, each managed by a gen_server) and pick the server by hashing `battle_id` (lesson 20's consistent hashing).

## 11. Where to go from here

| Direction | How |
|---|---|
| Networking on multiple cores | Several Reactor threads, one epoll each; use `SO_REUSEPORT` so the kernel spreads new connections over several listening sockets |
| Idle-connection timeouts, heartbeats | Record each connection's last-active time and check it periodically with a timing wheel (lesson 20) |
| Fewer system calls | Linux `io_uring`: submit I/O requests in batches and collect results in batches |
| Cross-platform | Boost.Asio / standalone Asio, which uses IOCP on Windows (the Proactor pattern) |
| Security | TLS (OpenSSL), connection authentication, per-IP connection limits |

## Interview questions

**Q1: What's the difference between select, poll and epoll?**
select is limited to 1024 fds, poll isn't; both pass the whole fd set to the kernel on every call and then scan every fd to find the ready ones, O(total connections). epoll keeps the interest list in the kernel and returns only ready fds, O(ready), which suits many connections with few active.

**Q2: What's the difference between epoll's LT and ET?**
LT (level-triggered, the default): as long as data remains unread, every `epoll_wait` reports it. ET (edge-triggered): reports once per state change, so you must loop until `EAGAIN` or the remaining data never triggers another notification. ET saves repeated notifications but makes missed-read bugs easier to write.

**Q3: What are coalesced and partial packets? How do you handle them?**
TCP is a byte stream and doesn't preserve application message boundaries: several messages may be read at once (coalesced), and one message may arrive over several reads (partial). The fix is to define a frame format: a length prefix (the most common, like `{packet, 4}`), fixed lengths, or special delimiters. The receiver appends data to a buffer and processes a frame once it's complete. Measured, a frame sent byte by byte was reassembled correctly.

**Q4: What's the difference between Reactor and Proactor?**
Reactor: you're told "you can read/write now" and call `recv`/`send` yourself (epoll). Proactor: you submit "read into this buffer for me" and are told "it's been read" once done (Windows IOCP, io_uring).

**Q5: When a worker thread finishes, how does it notify the I/O thread sleeping in `epoll_wait`?**
With an `eventfd` (or a pipe): add it to epoll's interest list, and a worker writing to it wakes the I/O thread. The result itself goes into a locked queue. An eventfd is a counter, so multiple writes merge (measured: 203 writes corresponded to a few dozen reads).

**Q6: Why set `TCP_NODELAY`?**
Nagle's algorithm batches small data before sending, and combined with the receiver's delayed ACK it can add tens of milliseconds of latency. Game server messages are small and latency-sensitive, so Nagle is usually turned off.

**Q7: How does a server apply backpressure?**
Limit the number of pending requests per connection, and stop reading that connection when it's exceeded (remove EPOLLIN). Data stays in the kernel receive buffer, and once it fills, TCP flow control slows the sender down. The effect is like Erlang's `{active, once}`.

**Q8: TCP's three-way handshake, four-way teardown, TIME_WAIT and CLOSE_WAIT?**
Three-way handshake: SYN → SYN+ACK → ACK, with both sides confirming initial sequence numbers. Four-way teardown: each side sends a FIN and gets it acknowledged, because TCP is full duplex and each direction closes separately. TIME_WAIT appears on the side that **closes actively** and lasts 2MSL, ensuring the final ACK can be retransmitted and delayed packets from the old connection don't interfere with a new one; servers sidestep it on restart with `SO_REUSEADDR`. CLOSE_WAIT appears on the side that **closes passively**: the peer has closed, but this side hasn't called `close` yet. Lots of CLOSE_WAIT usually means the code forgot to close the connection after `recv` returned 0.

**Q9: Why handle SIGPIPE?**
Writing to a connection the peer has closed raises SIGPIPE, whose default action terminates the process. Use the `MSG_NOSIGNAL` flag, or ignore the signal.

Next: [Lesson 20: Game server architecture and common data structures](20-game-server-architecture.md)
