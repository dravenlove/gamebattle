#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
struct UnitAoS { std::uint64_t id; std::int64_t hp, max_hp, attack, defense, speed; std::string phase, type; std::int64_t extra[4]; };   // 约 136 字节
struct UnitsSoA { std::vector<std::uint64_t> id; std::vector<std::int64_t> hp, max_hp, attack, defense, speed; };
int main() {
    constexpr std::size_t N = 2'000'000; constexpr int R = 20;
    std::vector<UnitAoS> aos(N); UnitsSoA soa; soa.hp.assign(N, 0); soa.id.resize(N);
    for (std::size_t i = 0; i < N; ++i) { aos[i].hp = static_cast<std::int64_t>(i); soa.hp[i] = static_cast<std::int64_t>(i); }
    auto t0 = std::chrono::steady_clock::now();
    std::int64_t a = 0; for (int r = 0; r < R; ++r) for (auto& u : aos) { u.hp -= 1; a += u.hp; }
    auto t1 = std::chrono::steady_clock::now();
    std::int64_t b = 0; for (int r = 0; r < R; ++r) for (auto& hp : soa.hp) { hp -= 1; b += hp; }
    auto t2 = std::chrono::steady_clock::now();
    std::cout << "sizeof(UnitAoS)=" << sizeof(UnitAoS) << "；200 万个单位，每轮所有 hp 减 1，共 20 轮\n";
    std::cout << "  AoS（结构体数组）: " << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms\n";
    std::cout << "  SoA（每个字段一个数组）: " << std::chrono::duration<double, std::milli>(t2 - t1).count() << " ms   结果一致: " << (a == b) << '\n';
}
