# 第 19 课：epoll 与 TCP 战斗服务器

**中文** | [English](en/19-epoll-battle-server.md)

> 第四部分「网络与游戏服务器」的第一课。实战代码：`practice/battle_tcp_server.cpp`（约 300 行）、`practice/tcp_client.py`。
>
> 游戏服务端岗位几乎必考网络编程。这一课把战斗引擎包装成一个独立的 TCP 服务：Linux epoll 事件循环 + 线程池，帧格式和 Erlang Port 一样是 `{packet, 4}`，所以 **Erlang 节点可以直接用 `gen_tcp` 连上来**。

## 1. 为什么要做成 TCP 服务

Port 是"一个 Erlang 节点对应一个 C++ 进程"，一根管道。规模变大后，常见的部署是把战斗计算拆成独立的服务：

```
 Erlang 逻辑节点 A ──┐                        ┌── 战斗服务器 1（4 核，4 个工作线程）
 Erlang 逻辑节点 B ──┼── TCP, {packet, 4} ───┼── 战斗服务器 2
 Erlang 逻辑节点 C ──┘   按 battle_id 分片    └── 战斗服务器 3
```

- 战斗服务器可以单独扩容、单独部署到 CPU 更强的机器上；
- 战斗服务器崩溃不影响逻辑节点，逻辑节点可以把请求重试到别的战斗服务器；
- 协议和 Port 完全一样（`{packet, 4}` + ETF），**引擎代码一行都不用改**，复用 `wire::handle_etf`。

## 2. Socket 基础

```cpp
int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);   // 创建 TCP socket
::bind(fd, address, ...);                                                    // 绑定地址和端口
::listen(fd, SOMAXCONN);                                                     // 开始监听，SOMAXCONN 是等待队列长度
int client = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);  // 接受一个连接
::recv(client, buffer, size, 0);                                             // 读
::send(client, data, size, MSG_NOSIGNAL);                                    // 写
::close(client);
```

- **阻塞 vs 非阻塞**：阻塞 socket 上调用 `recv`，没有数据就一直等。非阻塞 socket 没有数据时立即返回 -1，`errno` 是 `EAGAIN`（或 `EWOULDBLOCK`）。事件驱动的服务器必须用非阻塞 socket。
- `SOCK_CLOEXEC`：执行 `exec` 启动子进程时自动关闭，避免文件描述符泄漏给子进程。
- `MSG_NOSIGNAL`：对方已经关闭连接时，往里写数据默认会收到 `SIGPIPE` 信号，**默认行为是直接杀死进程**。加上这个标志后只返回错误。不处理 `SIGPIPE` 是新手服务器"莫名其妙退出"的常见原因。

这些系统调用返回的都是一个整数**文件描述符**（fd），用完必须 `close`。实战代码用第 13 课的 `FileDescriptor` 类（RAII，只能移动）管理它们，不会泄漏也不会重复关闭。

## 3. I/O 多路复用：从一个线程一个连接，到 epoll

**一个线程一个连接**：最简单，但一万个连接就要一万个线程，内存和上下文切换都扛不住。

**I/O 多路复用**：一个线程同时监视很多 fd，哪个可读或可写了再去处理哪个。

| | select | poll | epoll（Linux） |
|---|---|---|---|
| fd 数量上限 | 默认 1024 | 无 | 无 |
| 每次调用 | 把整个 fd 集合复制进内核 | 同左 | 不需要：兴趣列表一直保存在内核里 |
| 返回结果 | 要自己遍历所有 fd 找出就绪的 | 同左 | **只返回就绪的 fd** |
| 复杂度 | O(总连接数) | O(总连接数) | O(就绪的连接数) |

epoll 的三个调用：

```cpp
int epoll = ::epoll_create1(EPOLL_CLOEXEC);                      // 创建一个 epoll 实例

epoll_event event{};
event.events = EPOLLIN | EPOLLRDHUP;                             // 关心：可读、对方关闭
event.data.u64 = connection_id;                                  // 事件发生时原样带回来的数据
::epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &event);                   // 加入兴趣列表（MOD 修改，DEL 删除）

epoll_event ready[64];
int n = ::epoll_wait(epoll, ready, 64, -1);                      // 睡到有事件为止，返回就绪的个数
```

`data.u64` 里放的是**连接 ID**，不是指针。事件返回时用 ID 去 `connections_` 里查，查不到说明连接已经关闭（第 2 课"唯一 ID + 查找"）。

**这和 Erlang 有什么关系？** BEAM 虚拟机内部就是用 epoll（Linux 上）监视所有端口和 socket 的。你在 Erlang 里写的 `gen_tcp` 配合 `{active, once}`，底层正是这套机制，只是虚拟机替你写好了事件循环，再把事件变成消息发给进程。

### 水平触发（LT）和边缘触发（ET）

- **水平触发**（默认）：只要 socket 里还有没读完的数据，每次 `epoll_wait` 都会报告。
- **边缘触发**（`EPOLLET`）：只在状态**变化**时报告一次。如果这次没把数据读完，就再也不会通知你了，所以**必须循环读到 `EAGAIN` 为止**。

实战代码用的是水平触发，但读取时同样循环读到 `EAGAIN`，两种模式下都正确。ET 能减少一些重复通知，代价是一旦漏读就会让连接永远卡住。

## 4. 整体结构：一个 Reactor 线程 + 一个线程池

```
                         ┌─────────────── Reactor 线程（唯一拥有所有 socket）───────────────┐
 客户端 ── TCP ─────────▶│ epoll_wait                                                        │
                         │  ├─ 监听 socket 可读  → accept4，加入 epoll                        │
                         │  ├─ 连接可读          → recv → 切出完整帧 → 放进该连接的 pending    │
                         │  │                      → 该连接空闲？交给线程池 ─────────────┐     │
                         │  ├─ eventfd 可读      → 取出所有完成的结果 → 写回对应连接 ◀──┐ │     │
                         │  ├─ 连接可写          → 继续发送没发完的数据                  │ │     │
                         │  └─ signalfd 可读     → 收到 SIGTERM，退出循环               │ │     │
                         └───────────────────────────────────────────────────────────┼─┼─────┘
                                                                                     │ ▼
                         ┌──────────── 线程池（4 个工作线程）──────────────────────────┐
                         │  wire::handle_etf(请求) → 结果放进完成队列（加锁）→ 写 eventfd │
                         └─────────────────────────────────────────────────────────────┘
```

这叫 **Reactor 模式**：一个线程负责"等事件、分发事件"，耗时的计算交给别的线程。关键设计：

- **只有 Reactor 线程碰 socket 和连接状态**，所以 `connections_` 不需要加锁。工作线程只调用 `handle_etf`（第 17 课证明过它是线程安全的），然后把结果放进一个加锁的完成队列。
- **工作线程怎么通知 Reactor？** Reactor 正睡在 `epoll_wait` 里。`eventfd` 是一个内核计数器，它本身也是一个 fd：工作线程往里写一个数，它就变得"可读"，`epoll_wait` 随即返回。用一个 fd 把"跨线程通知"统一成了"又一个 I/O 事件"。
- **信号怎么处理？** `signalfd` 把信号也变成了一个 fd。`main` 在**创建任何线程之前**先屏蔽 SIGINT/SIGTERM（新线程会继承屏蔽状态），这样信号只会通过 `signalfd` 进入事件循环，不会在任意线程里打断执行。收到后退出循环，正常析构所有对象。

## 5. 帧协议：TCP 是字节流

**TCP 不保留消息边界。** 你发了两次 `send`，对方可能一次 `recv` 就收到了两条（俗称**粘包**）；你发了一条 8 KB 的消息，对方可能分几次才收全（**半包**）。所以必须自己定义"一条消息从哪里开始、到哪里结束"。

最常用的方式是**长度前缀**，正是 Erlang 的 `{packet, 4}`：4 字节大端长度 + 正文。

```cpp
bool read_frames(Connection& connection) {
    // 1. 把内核缓冲区里所有数据读出来，追加到 connection.input
    while (true) {
        const auto received = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (received > 0) { connection.input.insert(connection.input.end(), ...); continue; }
        if (received == 0) { peer_closed = true; break; }                   // 对方关闭
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;                 // 读完了
        if (errno == EINTR) continue;
        return false;                                                       // 出错
    }
    // 2. 从 input 里切出所有完整的帧
    std::size_t consumed = 0;
    while (input.size() - consumed >= 4) {
        const std::uint32_t length = 大端拼装(input[consumed..consumed+4]);
        if (length == 0 || length > kMaxFrameBytes) return false;            // 非法长度：直接断开
        if (input.size() - consumed - 4 < length) break;                    // 半包：等更多数据
        connection.pending.emplace_back(帧的正文);
        consumed += 4 + length;
    }
    input.erase(input.begin(), input.begin() + consumed);                    // 每次读取只 erase 一次
    return !peer_closed;
}
```

几个细节：
- **先读完、再切帧，最后一次性 `erase`**。如果每切一帧就 `erase` 一次，每次都要把后面的数据往前挪，N 帧就是 O(N²)。
- 长度上限检查放在**分配内存之前**（第 9 课的原则）：对方声称发 4 GB，直接断开连接。
- `input.size() - consumed >= 4` 而不是 `consumed + 4 <= input.size()`：第 7 课"判断条件本身不能溢出"的写法习惯。

实测这三种情况都能正确处理：

```
8 个连接，每个连接一次 sendall 25 帧（粘包）  → 200 个回复全部正确
一个 ping 帧逐字节发送，每字节间隔 20 ms（半包）→ 回复: 83 68 02 77 02 6f 6b 77 04 70 6f 6e 67  ({ok, pong})
发送长度头 0xFFFFFFFF                           → 服务器立即断开连接
```

## 6. 写：发送缓冲区与 `EPOLLOUT`

`send` 也可能只发出一部分：内核的发送缓冲区满了（客户端读得慢），就只能先发一部分，剩下的返回 `EAGAIN`。

```cpp
bool flush(Connection& connection) {
    while (connection.output_sent < connection.output.size()) {
        const auto sent = ::send(fd, output.data() + output_sent, output.size() - output_sent, MSG_NOSIGNAL);
        if (sent > 0) { connection.output_sent += sent; continue; }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;   // 缓冲区满了，等 EPOLLOUT
        if (sent < 0 && errno == EINTR) continue;
        return false;
    }
    connection.output.clear();
    connection.output_sent = 0;
    return true;
}
```

没发完的数据留在 `output` 里，等 socket 再次可写（`EPOLLOUT`）时继续发。

**什么时候关注 `EPOLLOUT`？** 只在确实有数据没发完时。socket 几乎总是可写的，如果一直关注 `EPOLLOUT`，水平触发模式下 `epoll_wait` 会不停返回，CPU 空转 100%。实战代码让关注的事件**从连接的状态推导出来**：

```cpp
void update_interest(std::uint64_t id, Connection& connection) {
    std::uint32_t wanted = EPOLLRDHUP;
    if (connection.pending.size() < kMaxPendingFrames) wanted |= EPOLLIN;      // 待处理请求没满才继续读
    if (connection.output_sent < connection.output.size()) wanted |= EPOLLOUT; // 有数据没发完才关注可写
    if (wanted == connection.interest) return;                                 // 没变化就不调用 epoll_ctl
    ...epoll_ctl(EPOLL_CTL_MOD, ...);
}
```

每次状态变化后调用一次，就不会忘记打开或关闭某个事件。

## 7. 背压与保序

**背压**：如果客户端发请求的速度比服务器处理得快，请求会无限堆积，最后内存耗尽。实战代码的做法是：某个连接积压的请求达到 64 个时，**暂停读取这个连接**（从兴趣列表里去掉 `EPOLLIN`）。数据就留在内核的接收缓冲区里；缓冲区满了以后，TCP 的流量控制会让客户端的 `send` 阻塞或返回 `EAGAIN`。压力就这样一路传回了发送方。这正是 Erlang 里 `{active, once}` 的思路：处理完一条才要下一条。

**保序**：`{packet, 4}` 协议里请求和回复没有编号，客户端靠顺序把回复和请求对应起来。如果同一个连接的多个请求同时在线程池里跑，后发的可能先算完，顺序就乱了。实战代码规定**每个连接同一时间只有一个请求在执行**（`busy` 标志），算完一个才派发下一个。和 `gamebattle_port` 的 gen_server 一样：每个 worker 一次只处理一场战斗。不同连接之间仍然是并行的。

**连接在任务执行期间断开了怎么办？** 结果回来时按连接 ID 查找，找不到就丢弃：

```cpp
auto found = connections_.find(completion.connection_id);
if (found == connections_.end()) continue;   // 客户端在战斗计算期间离开了
```

如果工作线程拿的是 `Connection&` 或指针，连接被删除后它就成了悬空引用。

**成员声明顺序**：线程池 `pool_` 是 `Server` 的**最后一个成员**。析构时它最先被销毁：等所有工作线程退出之后，它们用到的 `completions_mutex_`、`completions_`、`wakeup_` 才被销毁（第 2、17 课）。

## 8. 几个必须知道的 socket 选项

| 选项 | 作用 | 不设置会怎样 |
|---|---|---|
| `TCP_NODELAY` | 关闭 Nagle 算法，小数据立即发送 | Nagle 会把小包攒起来再发，碰上对方的延迟确认（delayed ACK），可能多等约 40 ms。游戏服务器几乎都要开 |
| `SO_REUSEADDR` | 允许绑定处于 TIME_WAIT 状态的端口 | 服务器重启后几十秒内 `bind` 失败："Address already in use" |
| `MSG_NOSIGNAL` | 写入已关闭的连接时不产生 SIGPIPE | 进程被 SIGPIPE 直接杀死 |
| `SO_KEEPALIVE` | 内核定期探测空闲连接是否还活着 | 对方断电时连接永远不会被发现已失效（默认探测间隔 2 小时，游戏里通常用应用层心跳） |

## 9. 实测结果

在 4 核机器上，4 个工作线程：

```
8 个连接 × 每个流水线发送 25 场 = 200 场战斗，用时 0.15 s，0 错误
所有回复逐字节相同（同一个 seed，在不同线程上计算）
单连接串行 300 场: p50=2.10 ms  p99=4.23 ms  max=6.86 ms
收到 SIGTERM 后干净退出：accepted 10 connections, served 203 requests
ThreadSanitizer 构建跑同样的测试：0 条警告
```

用 `strace -f -c` 统计服务器在这次测试中发出的系统调用（节选）：

```
calls  syscall
  753  futex          ← 线程池的锁和条件变量
 1566  mprotect       ← 多线程 malloc 扩展堆内存（第 21 课）
   62  epoll_wait     ← 203 个请求只用了 62 次：一次返回多个就绪事件
  203  sendto         ← 每个回复一次
  205  write          ← 203 次是工作线程写 eventfd 通知 Reactor，另外 2 次是日志输出
   58  read           ← 绝大部分是 Reactor 读 eventfd：203 次写入合并成了几十次读取
   44  recvfrom       ← 流水线的 25 帧往往一次就读完了
   13  accept4
   13  epoll_ctl      ← 兴趣列表只在真正变化时才修改
```

这张表很适合面试时讲：
- **epoll 的批量效应**：203 个请求只唤醒了 62 次；
- **eventfd 的合并**：工作线程写了 203 次，但 Reactor 只读了几十次，因为 eventfd 是一个计数器，多次写入会累加起来，一次读取就能全部取走；
- **最大的系统调用开销不在网络上**，而在 `futex`（锁）和 `mprotect`（内存分配）。这说明下一步该优化的是线程池的锁竞争和每场战斗的内存分配（第 16、21 课），而不是网络部分。

## 10. 从 Erlang 连接这个服务器

`gen_tcp` 的 `{packet, 4}` 和服务器的帧格式完全一致（下面的代码本机没有 Erlang 环境，没有实际运行过）：

```erlang
{ok, Socket} = gen_tcp:connect("127.0.0.1", 9000, [binary, {packet, 4}, {active, false}]),
ok = gen_tcp:send(Socket, term_to_binary(gamebattle:example_request())),
{ok, Reply} = gen_tcp:recv(Socket, 0, 30000),
{ok, Result} = binary_to_term(Reply),
#{winner := Winner, rounds := Rounds} = Result.
```

`gen_tcp` 自动处理长度头的添加和拆分，你拿到的就是完整的 ETF 二进制。生产环境里可以用一个连接池（比如每个战斗服务器保持 N 个连接，每个连接由一个 gen_server 管理），按 `battle_id` 哈希选择服务器（第 20 课的一致性哈希）。

## 11. 还可以怎么扩展

| 方向 | 做法 |
|---|---|
| 多核处理网络 | 多个 Reactor 线程，每个一个 epoll；用 `SO_REUSEPORT` 让内核把新连接分摊到多个监听 socket |
| 空闲连接超时、心跳 | 每个连接记录最后活跃时间，用时间轮定时检查（第 20 课） |
| 更少的系统调用 | Linux 的 `io_uring`：批量提交 I/O 请求、批量收取结果 |
| 跨平台 | 用 Boost.Asio / standalone Asio，Windows 上底层是 IOCP（Proactor 模式） |
| 安全 | TLS（OpenSSL）、连接认证、限制每个 IP 的连接数 |

## 面试题

**Q1：select、poll、epoll 的区别？**
select 有 1024 个 fd 的上限，poll 没有；两者每次调用都要把整个 fd 集合传给内核，返回后还要遍历所有 fd 找出就绪的，复杂度 O(总连接数)。epoll 在内核里维护兴趣列表，只返回就绪的 fd，复杂度 O(就绪数)，适合大量连接、少量活跃的场景。

**Q2：epoll 的 LT 和 ET 有什么区别？**
LT（水平触发，默认）：只要还有数据没读完，每次 `epoll_wait` 都会报告。ET（边缘触发）：只在状态变化时报告一次，必须循环读到 `EAGAIN`，否则剩余数据不会再触发通知。ET 减少重复通知，但更容易写出漏读的 Bug。

**Q3：什么是粘包和半包？怎么解决？**
TCP 是字节流，不保留应用层消息边界：多条消息可能一次读到（粘包），一条消息可能分多次读到（半包）。解决方法是定义帧格式：长度前缀（最常用，如 `{packet, 4}`）、固定长度、特殊分隔符。接收方把数据追加到缓冲区，攒够一个完整帧再处理。实测逐字节发送的帧能被正确重组。

**Q4：Reactor 和 Proactor 的区别？**
Reactor：通知你"可以读写了"，由你自己调用 `recv`/`send`（epoll）。Proactor：你提交"帮我读到这个缓冲区"，完成后通知你"已经读好了"（Windows IOCP、io_uring）。

**Q5：工作线程算完了，怎么通知正在 `epoll_wait` 里睡眠的 I/O 线程？**
用 `eventfd`（或管道）：把它加入 epoll 的兴趣列表，工作线程写入即可唤醒 I/O 线程。结果本身放在一个加锁的队列里。eventfd 是计数器，多次写入会合并（实测 203 次写入只对应几十次读取）。

**Q6：为什么要设置 `TCP_NODELAY`？**
Nagle 算法会把小数据攒起来再发，与接收方的延迟确认叠加时可能多出几十毫秒延迟。游戏服务器的消息小、对延迟敏感，通常关闭 Nagle。

**Q7：服务器怎么做背压？**
限制每个连接待处理请求的数量，超过时暂停读取该连接（去掉 EPOLLIN）。数据留在内核接收缓冲区，缓冲区满后 TCP 流量控制会让发送方变慢。效果类似 Erlang 的 `{active, once}`。

**Q8：TCP 三次握手、四次挥手、TIME_WAIT 和 CLOSE_WAIT？**
三次握手：SYN → SYN+ACK → ACK，双方确认初始序列号。四次挥手：双方各自发送 FIN 并确认，因为 TCP 是全双工的，两个方向分别关闭。TIME_WAIT 出现在**主动关闭**的一方，持续 2MSL，确保最后一个 ACK 能重传、旧连接的延迟包不会干扰新连接；服务器重启时用 `SO_REUSEADDR` 绕开。CLOSE_WAIT 出现在**被动关闭**的一方：对方已经关闭，但本方还没调用 `close`。大量 CLOSE_WAIT 通常说明代码在 `recv` 返回 0 后忘了关闭连接。

**Q9：为什么要处理 SIGPIPE？**
向已被对方关闭的连接写数据会触发 SIGPIPE，默认行为是终止进程。可以用 `MSG_NOSIGNAL` 标志，或者忽略这个信号。

下一课：[第 20 课：游戏服务器架构与常用数据结构](20-game-server-architecture.md)
