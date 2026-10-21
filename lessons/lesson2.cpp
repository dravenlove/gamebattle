// 第 2 课：值、引用、指针、const、std::move
// 编译：g++ -std=c++20 -Wall -Wextra -Wpedantic -g -fsanitize=address lesson2.cpp -o lesson2
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mini {

using UnitId = std::uint64_t;
enum class Side : std::uint8_t { attacker, defender };

struct Stats {
    std::int64_t hp{1};
    std::int64_t attack{0};
    std::int64_t defense{0};
};

struct UnitConfig {
    UnitId id{0};
    std::int32_t position{0};
    Stats final_stats;
};

struct BattleRequest {
    std::uint64_t battle_id{0};
    std::vector<UnitConfig> attacker;
    std::vector<UnitConfig> defender;
};

struct Event {
    std::string type;
    UnitId actor{0};
    UnitId target{0};
    std::int64_t value{0};
};

struct BattleResult {
    std::uint64_t battle_id{0};
    std::vector<Event> events;
};

// 本场可变状态：config 是从请求“复制”来的，本场可以随便排序、修改。
struct RuntimeUnit {
    UnitConfig config;
    Side side{Side::attacker};
    std::int64_t hp{0};

    bool alive() const { return hp > 0; }
};

class BattleState {
public:
    explicit BattleState(const BattleRequest& request_value)
        : request(request_value), result{.battle_id = request_value.battle_id, .events = {}} {
        add_side(request.attacker, Side::attacker);
        add_side(request.defender, Side::defender);
    }

    std::optional<std::size_t> find_unit(UnitId id) const {
        const auto found = unit_index.find(id);
        if (found == unit_index.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    void emit(std::string type, UnitId actor, UnitId target, std::int64_t value) {
        result.events.push_back(Event{
            .type = std::move(type), .actor = actor, .target = target, .value = value});
    }

    BattleResult finish() { return std::move(result); }

    const BattleRequest& request;
    BattleResult result;
    std::vector<RuntimeUnit> units;
    std::unordered_map<UnitId, std::size_t> unit_index;

private:
    void add_side(const std::vector<UnitConfig>& configs, Side side) {
        for (const auto& source : configs) {
            RuntimeUnit runtime;
            runtime.config = source;
            runtime.side = side;
            runtime.hp = source.final_stats.hp;
            unit_index.emplace(source.id, units.size());
            units.push_back(std::move(runtime));
        }
    }
};

class EffectSystem {
public:
    explicit EffectSystem(BattleState& state) : state_(state) {}

    void apply_damage(std::size_t actor_index, std::size_t target_index) {
        const auto& actor = state_.units[actor_index];
        auto& target = state_.units[target_index];
        if (!actor.alive() || !target.alive()) {
            return;
        }
        auto damage = actor.config.final_stats.attack - target.config.final_stats.defense;
        if (damage < 1) {
            damage = 1;
        }
        if (damage > target.hp) {
            damage = target.hp;
        }
        target.hp -= damage;
        state_.emit("damage", actor.config.id, target.config.id, damage);
    }

private:
    BattleState& state_;
};

class BattleRunner {
public:
    explicit BattleRunner(const BattleRequest& request) : state_(request), effects_(state_) {}

    BattleResult run() {
        const auto attacker = state_.find_unit(1001);
        const auto defender = state_.find_unit(2001);
        if (attacker.has_value() && defender.has_value()) {
            effects_.apply_damage(*attacker, *defender);
            effects_.apply_damage(*defender, *attacker);
        }
        return state_.finish();
    }

private:
    BattleState state_;
    EffectSystem effects_;
};

} // namespace mini

int main() {
    const mini::BattleRequest request{
        .battle_id = 3001,
        .attacker = {{.id = 1001, .position = 1,
                      .final_stats = {.hp = 1800, .attack = 260, .defense = 80}}},
        .defender = {{.id = 2001, .position = 1,
                      .final_stats = {.hp = 1500, .attack = 240, .defense = 90}}},
    };

    const auto result = mini::BattleRunner(request).run();

    std::cout << "battle " << result.battle_id << '\n';
    for (const auto& event : result.events) {
        std::cout << "  " << event.type << ' ' << event.actor << " -> " << event.target
                  << " value=" << event.value << '\n';
    }
    std::cout << "request attacker hp unchanged: " << request.attacker[0].final_stats.hp << '\n';
}
