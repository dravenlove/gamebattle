#include "ranklist.hpp"
#include <chrono>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <utility>
#include <vector>
using Key = std::pair<std::int64_t, std::uint64_t>;          // (-score, id)：std::set 按这个升序就是排行榜顺序
int main() {
    // 1) 正确性：随机插入、改分、删除、查排名，与 std::set 逐一比对
    RankList list; std::set<Key> reference; std::map<std::uint64_t, std::int64_t> score_of; std::mt19937_64 rng(1);
    long checks = 0;
    for (int op = 0; op < 60000; ++op) {
        const std::uint64_t id = rng() % 3000; const std::int64_t score = static_cast<std::int64_t>(rng() % 500);
        if (auto it = score_of.find(id); it != score_of.end()) { list.erase(it->second, id); reference.erase({-it->second, id}); }
        if (rng() % 10 != 0) { list.insert(score, id); reference.insert({-score, id}); score_of[id] = score; } else { score_of.erase(id); }
        if (op % 7 == 0 && !reference.empty()) {
            auto pick = std::next(reference.begin(), static_cast<long>(rng() % reference.size()));
            const auto expected_rank = static_cast<std::size_t>(std::distance(reference.begin(), pick)) + 1;
            if (list.rank_of(-pick->first, pick->second) != expected_rank || list.id_at(expected_rank) != pick->second || list.size() != reference.size()) {
                std::cout << "MISMATCH at op " << op << '\n'; return 1;
            }
            ++checks;
        }
    }
    std::cout << "6 万次随机增删改，" << checks << " 次排名查询与 std::set 结果完全一致\n";

    // 2) 性能：20 万玩家，查 2 万次排名
    RankList big; std::set<Key> big_set; std::vector<Key> players;
    for (std::uint64_t id = 0; id < 200000; ++id) { const auto s = static_cast<std::int64_t>(rng() % 1'000'000); big.insert(s, id); big_set.insert({-s, id}); players.push_back({s, id}); }
    auto t0 = std::chrono::steady_clock::now(); std::size_t sum1 = 0;
    for (int q = 0; q < 20000; ++q) { const auto& p = players[rng() % players.size()]; sum1 += big.rank_of(p.first, p.second); }
    auto t1 = std::chrono::steady_clock::now(); std::size_t sum2 = 0;
    for (int q = 0; q < 2000; ++q) { const auto& p = players[rng() % players.size()]; sum2 += static_cast<std::size_t>(std::distance(big_set.begin(), big_set.find({-p.first, p.second}))) + 1; }
    auto t2 = std::chrono::steady_clock::now();
    std::cout << "20 万玩家：跳表查一次排名 " << std::chrono::duration<double, std::micro>(t1 - t0).count() / 20000 << " us，"
              << "std::set + distance 查一次 " << std::chrono::duration<double, std::micro>(t2 - t1).count() / 2000 << " us\n";
    std::cout << "（校验用，防止优化器删掉循环）sum1=" << sum1 << " sum2=" << sum2 << '\n';
}
