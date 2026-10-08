// Lesson 19: a TCP battle server on Linux epoll.
//
// One reactor thread owns every socket; battles run on a worker pool; finished
// replies come back to the reactor through an eventfd. Frames use the same
// {packet, 4} format as the Erlang Port, so an Erlang node can connect with
// gen_tcp:connect(Host, Port, [binary, {packet, 4}]).
//
//   battle_tcp_server [port] [worker_threads]

#include "thread_pool.hpp"

#include "gamebattle/wire.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kMaxFrameBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaxPendingFrames = 64;  // per connection, then stop reading
constexpr std::uint64_t kListenerId = 1;
constexpr std::uint64_t kWakeupId = 2;
constexpr std::uint64_t kSignalId = 3;
constexpr std::uint64_t kFirstConnectionId = 16;

[[noreturn]] void fail(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

// Owns one file descriptor: closes it exactly once, can be moved but not copied.
class FileDescriptor {
public:
    FileDescriptor() = default;
    explicit FileDescriptor(int fd) : fd_(fd) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~FileDescriptor() { reset(); }

    int get() const noexcept { return fd_; }
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_{-1};
};

struct Connection {
    FileDescriptor socket;
    std::vector<std::uint8_t> input;    // received, not yet framed
    std::vector<std::uint8_t> output;   // framed replies not yet sent
    std::size_t output_sent{0};
    std::deque<std::vector<std::uint8_t>> pending;  // complete requests, in arrival order
    bool busy{false};                   // one request of this connection is on the pool
    std::uint32_t interest{0};          // epoll events currently registered
};

struct Completion {
    std::uint64_t connection_id;
    std::vector<std::uint8_t> reply;
};

class Server {
public:
    Server(std::uint16_t port, std::size_t threads) : pool_(threads) {
        epoll_ = FileDescriptor(::epoll_create1(EPOLL_CLOEXEC));
        if (epoll_.get() < 0) fail("epoll_create1");

        listener_ = FileDescriptor(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (listener_.get() < 0) fail("socket");
        const int enable = 1;
        ::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) fail("bind");
        if (::listen(listener_.get(), SOMAXCONN) < 0) fail("listen");
        watch(listener_.get(), kListenerId, EPOLLIN);

        wakeup_ = FileDescriptor(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
        if (wakeup_.get() < 0) fail("eventfd");
        watch(wakeup_.get(), kWakeupId, EPOLLIN);

        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        signals_ = FileDescriptor(::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC));
        if (signals_.get() < 0) fail("signalfd");
        watch(signals_.get(), kSignalId, EPOLLIN);

        std::cout << "listening on 127.0.0.1:" << port << " with " << threads << " worker threads"
                  << std::endl;
    }

    void run() {
        std::array<epoll_event, 64> events{};
        while (running_) {
            const int ready = ::epoll_wait(epoll_.get(), events.data(), static_cast<int>(events.size()), -1);
            if (ready < 0) {
                if (errno == EINTR) continue;
                fail("epoll_wait");
            }
            for (int index = 0; index < ready; ++index) {
                const auto id = events[static_cast<std::size_t>(index)].data.u64;
                const auto flags = events[static_cast<std::size_t>(index)].events;
                if (id == kListenerId) {
                    accept_all();
                } else if (id == kWakeupId) {
                    deliver_completions();
                } else if (id == kSignalId) {
                    running_ = false;
                } else {
                    on_connection_event(id, flags);
                }
            }
        }
        std::cout << "shutting down: accepted " << accepted_ << " connections, served "
                  << served_ << " requests" << std::endl;
    }

private:
    void watch(int fd, std::uint64_t id, std::uint32_t events) {
        epoll_event event{};
        event.events = events;
        event.data.u64 = id;
        if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &event) < 0) fail("epoll_ctl add");
    }

    void accept_all() {
        while (true) {
            FileDescriptor client(::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (client.get() < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == EINTR || errno == ECONNABORTED) continue;
                fail("accept4");
            }
            const int enable = 1;  // battle replies are small: send them immediately
            ::setsockopt(client.get(), IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
            const auto id = next_connection_id_++;
            auto& connection = connections_[id];
            connection.socket = std::move(client);
            connection.interest = EPOLLIN | EPOLLRDHUP;
            watch(connection.socket.get(), id, connection.interest);
            ++accepted_;
        }
    }

    void on_connection_event(std::uint64_t id, std::uint32_t flags) {
        auto found = connections_.find(id);
        if (found == connections_.end()) return;  // closed earlier in this batch of events
        auto& connection = found->second;
        // RDHUP is level-triggered: ignoring it while EPOLLIN is paused would spin the loop.
        if (flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
            close_connection(id);
            return;
        }
        if (flags & EPOLLIN) {
            if (!read_frames(connection)) {
                close_connection(id);
                return;
            }
            dispatch(id, connection);
        }
        if (flags & EPOLLOUT) {
            if (!flush(connection)) {
                close_connection(id);
                return;
            }
        }
        update_interest(id, connection);
    }

    // Returns false when the peer closed the connection or sent an invalid frame.
    bool read_frames(Connection& connection) {
        std::array<std::uint8_t, 64 * 1024> buffer{};
        bool peer_closed = false;
        while (true) {
            const auto received = ::recv(connection.socket.get(), buffer.data(), buffer.size(), 0);
            if (received > 0) {
                connection.input.insert(connection.input.end(), buffer.begin(), buffer.begin() + received);
                continue;
            }
            if (received == 0) {
                peer_closed = true;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }

        std::size_t consumed = 0;
        auto& input = connection.input;
        while (input.size() - consumed >= 4) {
            const std::uint32_t length = (std::uint32_t{input[consumed]} << 24U) |
                                         (std::uint32_t{input[consumed + 1]} << 16U) |
                                         (std::uint32_t{input[consumed + 2]} << 8U) |
                                         std::uint32_t{input[consumed + 3]};
            if (length == 0 || length > kMaxFrameBytes) return false;
            if (input.size() - consumed - 4 < length) break;  // half a frame: wait for more bytes
            const auto begin = input.begin() + static_cast<std::ptrdiff_t>(consumed + 4);
            connection.pending.emplace_back(begin, begin + length);
            consumed += 4 + length;
        }
        input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(consumed));  // one erase per read
        return !peer_closed;
    }

    // At most one request per connection runs at a time, so replies keep request order.
    void dispatch(std::uint64_t id, Connection& connection) {
        if (connection.busy || connection.pending.empty()) return;
        connection.busy = true;
        auto request = std::move(connection.pending.front());
        connection.pending.pop_front();
        pool_.submit([this, id, request = std::move(request)] {
            auto reply = gamebattle::wire::handle_etf(request);  // thread-safe: stateless engine
            {
                std::lock_guard lock(completions_mutex_);
                completions_.push_back(Completion{id, std::move(reply)});
            }
            const std::uint64_t one = 1;
            [[maybe_unused]] const auto written = ::write(wakeup_.get(), &one, sizeof(one));
        });
    }

    void deliver_completions() {
        std::uint64_t counter = 0;
        [[maybe_unused]] const auto drained = ::read(wakeup_.get(), &counter, sizeof(counter));
        std::vector<Completion> ready;
        {
            std::lock_guard lock(completions_mutex_);
            ready.swap(completions_);  // hold the lock only for the swap
        }
        for (auto& completion : ready) {
            auto found = connections_.find(completion.connection_id);
            if (found == connections_.end()) continue;  // client left while its battle ran
            auto& connection = found->second;
            const auto length = static_cast<std::uint32_t>(completion.reply.size());
            const std::uint8_t header[4] = {
                static_cast<std::uint8_t>(length >> 24U), static_cast<std::uint8_t>(length >> 16U),
                static_cast<std::uint8_t>(length >> 8U), static_cast<std::uint8_t>(length)};
            connection.output.insert(connection.output.end(), header, header + 4);
            connection.output.insert(connection.output.end(), completion.reply.begin(), completion.reply.end());
            connection.busy = false;
            ++served_;
            if (!flush(connection)) {
                close_connection(completion.connection_id);
                continue;
            }
            dispatch(completion.connection_id, connection);
            update_interest(completion.connection_id, connection);
        }
    }

    bool flush(Connection& connection) {
        while (connection.output_sent < connection.output.size()) {
            const auto sent = ::send(connection.socket.get(), connection.output.data() + connection.output_sent,
                                     connection.output.size() - connection.output_sent, MSG_NOSIGNAL);
            if (sent > 0) {
                connection.output_sent += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;  // kernel buffer full
            if (sent < 0 && errno == EINTR) continue;
            return false;
        }
        connection.output.clear();
        connection.output_sent = 0;
        return true;
    }

    // Interest is derived from state: read while the queue has room, write while bytes wait.
    void update_interest(std::uint64_t id, Connection& connection) {
        std::uint32_t wanted = EPOLLRDHUP;
        if (connection.pending.size() < kMaxPendingFrames) wanted |= EPOLLIN;
        if (connection.output_sent < connection.output.size()) wanted |= EPOLLOUT;
        if (wanted == connection.interest) return;
        epoll_event event{};
        event.events = wanted;
        event.data.u64 = id;
        if (::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, connection.socket.get(), &event) < 0) fail("epoll_ctl mod");
        connection.interest = wanted;
    }

    void close_connection(std::uint64_t id) {
        connections_.erase(id);  // FileDescriptor closes the socket; epoll drops it automatically
    }

    FileDescriptor epoll_;
    FileDescriptor listener_;
    FileDescriptor wakeup_;
    FileDescriptor signals_;
    std::unordered_map<std::uint64_t, Connection> connections_;
    std::uint64_t next_connection_id_{kFirstConnectionId};
    bool running_{true};
    std::uint64_t accepted_{0};
    std::uint64_t served_{0};
    std::mutex completions_mutex_;
    std::vector<Completion> completions_;
    practice::ThreadPool pool_;  // last member: destroyed first, so no task outlives what it touches
};

} // namespace

int main(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::atoi(argv[1]) : 9000);
    const auto threads = static_cast<std::size_t>(argc > 2 ? std::atoi(argv[2]) : 4);

    // Block SIGINT/SIGTERM before any thread exists, so they are only seen through signalfd.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    try {
        Server server(port, threads);
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "battle_tcp_server: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
