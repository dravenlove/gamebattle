#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>
struct Entity { std::uint32_t id; float x, y; };
// 九宫格 AOI：地图切成 cell×cell 的格子，每个格子记录里面有哪些实体；查询只看周围 3×3 个格子
class GridAoi {
public:
    GridAoi(float map_size, float cell) : cell_(cell), columns_(static_cast<int>(std::ceil(map_size / cell))), cells_(columns_ * columns_) {}
    void insert(const Entity& e) { cells_[index(e.x, e.y)].push_back(e); }
    std::size_t count_in_range(float x, float y, float radius) const {
        const int cx = static_cast<int>(x / cell_), cy = static_cast<int>(y / cell_);
        std::size_t count = 0;
        for (int gy = cy - 1; gy <= cy + 1; ++gy)
            for (int gx = cx - 1; gx <= cx + 1; ++gx) {
                if (gx < 0 || gy < 0 || gx >= columns_ || gy >= columns_) continue;
                for (const auto& e : cells_[gy * columns_ + gx]) { const float dx = e.x - x, dy = e.y - y; if (dx * dx + dy * dy <= radius * radius) ++count; }
            }
        return count;
    }
private:
    std::size_t index(float x, float y) const { return static_cast<std::size_t>(static_cast<int>(y / cell_) * columns_ + static_cast<int>(x / cell_)); }
    float cell_; int columns_; std::vector<std::vector<Entity>> cells_;
};
int main() {
    constexpr float kMap = 10000.f, kRadius = 100.f; constexpr int kEntities = 20000, kQueries = 20000;
    std::mt19937 rng(3); std::uniform_real_distribution<float> pos(0.f, kMap - 1.f);
    std::vector<Entity> all(kEntities); for (std::uint32_t i = 0; i < kEntities; ++i) all[i] = {i, pos(rng), pos(rng)};
    GridAoi grid(kMap, kRadius); for (const auto& e : all) grid.insert(e);
    std::vector<std::pair<float, float>> queries(kQueries); for (auto& q : queries) q = {pos(rng), pos(rng)};
    std::size_t brute_total = 0, grid_total = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (auto [x, y] : queries) for (const auto& e : all) { const float dx = e.x - x, dy = e.y - y; if (dx * dx + dy * dy <= kRadius * kRadius) ++brute_total; }
    auto t1 = std::chrono::steady_clock::now();
    for (auto [x, y] : queries) grid_total += grid.count_in_range(x, y, kRadius);
    auto t2 = std::chrono::steady_clock::now();
    std::cout << "2 万个实体，2 万次视野查询（半径 100，地图 10000×10000）\n";
    std::cout << "  暴力遍历: " << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms\n";
    std::cout << "  九宫格  : " << std::chrono::duration<double, std::milli>(t2 - t1).count() << " ms\n";
    std::cout << "  两种方法找到的实体总数相同: " << (brute_total == grid_total) << " (" << grid_total << ")\n";
}
