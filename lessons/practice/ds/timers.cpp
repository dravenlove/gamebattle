#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <queue>
#include <random>
#include <vector>

struct Timer { std::uint64_t deadline; std::uint32_t id; };

// 最小堆：O(log n) 插入，O(log n) 弹出
struct HeapTimers {
    struct Later { bool operator()(const Timer& a, const Timer& b) const { return a.deadline != b.deadline ? a.deadline > b.deadline : a.id > b.id; } };
    std::priority_queue<Timer, std::vector<Timer>, Later> heap;
    std::uint64_t now = 0;
    void add(std::uint32_t id, std::uint64_t delay) { heap.push({now + delay, id}); }
    template <typename F> void tick(F&& fire) {
        ++now;
        while (!heap.empty() && heap.top().deadline <= now) { fire(heap.top().id, now); heap.pop(); }
    }
};

// 单层时间轮：Slots 个槽，每个 tick 前进一格；延迟超过一圈的定时器记录还要转几圈
template <std::size_t Slots>
struct TimerWheel {
    struct Entry { std::uint32_t id; std::uint64_t rounds; };
    std::vector<std::vector<Entry>> slots = std::vector<std::vector<Entry>>(Slots);
    std::uint64_t now = 0;
    void add(std::uint32_t id, std::uint64_t delay) {                 // O(1)
        const auto deadline = now + delay;
        slots[deadline % Slots].push_back({id, (delay - 1) / Slots});
    }
    template <typename F> void tick(F&& fire) {                         // 只看当前这一个槽
        ++now;
        auto& slot = slots[now % Slots];
        std::size_t keep = 0;
        for (auto& entry : slot) {
            if (entry.rounds == 0) fire(entry.id, now);
            else { --entry.rounds; slot[keep++] = entry; }
        }
        slot.resize(keep);
    }
};

int main() {
    constexpr std::uint32_t N = 1'000'000;
    std::mt19937 rng(42); std::vector<std::uint64_t> delays(N);
    for (auto& d : delays) d = 1 + rng() % 30'000;        // 1~30000 个 tick（tick=10ms 时最长 5 分钟）
    auto run = [&](auto& timers, const char* name) {
        std::uint64_t fired = 0, checksum = 0;
        const auto start = std::chrono::steady_clock::now();
        for (std::uint32_t id = 0; id < N; ++id) timers.add(id, delays[id]);
        const auto added = std::chrono::steady_clock::now();
        while (fired < N) timers.tick([&](std::uint32_t id, std::uint64_t at) { ++fired; checksum += (id + 1) * at; });
        const auto done = std::chrono::steady_clock::now();
        std::cout << name << ": 插入 " << std::chrono::duration<double, std::milli>(added - start).count() << " ms, 推进并触发 "
                  << std::chrono::duration<double, std::milli>(done - added).count() << " ms, 校验和 " << checksum << '\n';
        return checksum;
    };
    HeapTimers heap; TimerWheel<1024> wheel;
    const auto a = run(heap, "最小堆  ");
    const auto b = run(wheel, "时间轮  ");
    std::cout << "两者在完全相同的 tick 触发了完全相同的定时器: " << (a == b) << '\n';
}
