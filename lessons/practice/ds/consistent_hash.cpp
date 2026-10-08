#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>
std::uint64_t mix(std::uint64_t x) { x += 0x9e3779b97f4a7c15ULL; x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL; x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL; return x ^ (x >> 31); }
class HashRing {
public:
    explicit HashRing(int virtual_nodes) : virtual_nodes_(virtual_nodes) {}
    void add(int server) { for (int v = 0; v < virtual_nodes_; ++v) ring_[mix((std::uint64_t(server) << 32) | std::uint64_t(v))] = server; }
    int pick(std::uint64_t battle_id) const {                    // 顺时针找到第一个节点
        auto it = ring_.lower_bound(mix(battle_id));
        return it == ring_.end() ? ring_.begin()->second : it->second;
    }
private:
    int virtual_nodes_; std::map<std::uint64_t, int> ring_;
};
int main() {
    constexpr std::uint64_t kBattles = 300000;
    for (int vnodes : {1, 160}) {
        HashRing ring(vnodes); for (int s = 0; s < 3; ++s) ring.add(s);
        std::vector<int> before(kBattles), count(4, 0);
        for (std::uint64_t id = 0; id < kBattles; ++id) { before[id] = ring.pick(id); ++count[before[id]]; }
        ring.add(3);
        std::uint64_t moved = 0; for (std::uint64_t id = 0; id < kBattles; ++id) moved += ring.pick(id) != before[id];
        std::cout << "一致性哈希(每台 " << vnodes << " 个虚拟节点): 3 台时分布 " << count[0] << '/' << count[1] << '/' << count[2]
                  << "，加第 4 台后迁移 " << 100.0 * moved / kBattles << "% 的战斗\n";
    }
    std::uint64_t moved = 0; for (std::uint64_t id = 0; id < kBattles; ++id) moved += (mix(id) % 3) != (mix(id) % 4);
    std::cout << "取模 hash % N: 从 3 台变 4 台迁移 " << 100.0 * moved / kBattles << "% 的战斗\n";
}
