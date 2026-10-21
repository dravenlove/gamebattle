#pragma once
// Redis ZSET 同款跳表：按分数从高到低（同分按 id 从小到大）排序，
// 每个前进指针额外记录 span（跳过了几个节点），从而 O(log n) 求排名。
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

class RankList {
public:
    RankList() : head_(new Node{0, 0, std::vector<Level>(kMaxLevel)}) {}
    RankList(const RankList&) = delete;
    RankList& operator=(const RankList&) = delete;
    ~RankList() {
        Node* node = head_;
        while (node != nullptr) { Node* next = node->levels[0].next; delete node; node = next; }
    }

    void insert(std::int64_t score, std::uint64_t id) {
        std::array<Node*, kMaxLevel> update{};
        std::array<std::size_t, kMaxLevel> rank{};
        Node* x = head_;
        for (int i = level_ - 1; i >= 0; --i) {
            rank[i] = i == level_ - 1 ? 0 : rank[i + 1];
            while (x->levels[i].next != nullptr && precedes(*x->levels[i].next, score, id)) {
                rank[i] += x->levels[i].span;
                x = x->levels[i].next;
            }
            update[i] = x;
        }
        const int new_level = random_level();
        if (new_level > level_) {
            for (int i = level_; i < new_level; ++i) {
                rank[i] = 0;
                update[i] = head_;
                head_->levels[i].span = size_;
            }
            level_ = new_level;
        }
        Node* node = new Node{score, id, std::vector<Level>(static_cast<std::size_t>(new_level))};
        for (int i = 0; i < new_level; ++i) {
            node->levels[i].next = update[i]->levels[i].next;
            update[i]->levels[i].next = node;
            node->levels[i].span = update[i]->levels[i].span - (rank[0] - rank[i]);
            update[i]->levels[i].span = (rank[0] - rank[i]) + 1;
        }
        for (int i = new_level; i < level_; ++i) ++update[i]->levels[i].span;
        ++size_;
    }

    bool erase(std::int64_t score, std::uint64_t id) {
        std::array<Node*, kMaxLevel> update{};
        Node* x = head_;
        for (int i = level_ - 1; i >= 0; --i) {
            while (x->levels[i].next != nullptr && precedes(*x->levels[i].next, score, id)) x = x->levels[i].next;
            update[i] = x;
        }
        x = x->levels[0].next;
        if (x == nullptr || x->score != score || x->id != id) return false;
        for (int i = 0; i < level_; ++i) {
            if (update[i]->levels[i].next == x) {
                update[i]->levels[i].span += x->levels[i].span - 1;
                update[i]->levels[i].next = x->levels[i].next;
            } else {
                --update[i]->levels[i].span;
            }
        }
        while (level_ > 1 && head_->levels[level_ - 1].next == nullptr) --level_;
        --size_;
        delete x;
        return true;
    }

    // 1 表示第一名；不存在返回 0
    std::size_t rank_of(std::int64_t score, std::uint64_t id) const {
        std::size_t rank = 0;
        const Node* x = head_;
        for (int i = level_ - 1; i >= 0; --i) {
            while (x->levels[i].next != nullptr &&
                   (precedes(*x->levels[i].next, score, id) ||
                    (x->levels[i].next->score == score && x->levels[i].next->id == id))) {
                rank += x->levels[i].span;
                x = x->levels[i].next;
            }
            if (x != head_ && x->score == score && x->id == id) return rank;
        }
        return 0;
    }

    // 第 rank 名的 id（1 起）；越界返回 0
    std::uint64_t id_at(std::size_t rank) const {
        std::size_t traversed = 0;
        const Node* x = head_;
        for (int i = level_ - 1; i >= 0; --i) {
            while (x->levels[i].next != nullptr && traversed + x->levels[i].span <= rank) {
                traversed += x->levels[i].span;
                x = x->levels[i].next;
            }
            if (traversed == rank && x != head_) return x->id;
        }
        return 0;
    }

    std::size_t size() const noexcept { return size_; }

private:
    static constexpr int kMaxLevel = 32;
    struct Node;
    struct Level { Node* next = nullptr; std::size_t span = 0; };
    struct Node { std::int64_t score; std::uint64_t id; std::vector<Level> levels; };

    static bool precedes(const Node& node, std::int64_t score, std::uint64_t id) {
        return node.score > score || (node.score == score && node.id < id);
    }
    int random_level() {
        int level = 1;
        while (level < kMaxLevel && (rng_() & 3U) == 0) ++level;   // 每升一层概率 1/4，和 Redis 一样
        return level;
    }

    Node* head_;
    int level_{1};
    std::size_t size_{0};
    std::mt19937_64 rng_{2026};
};
