#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace mini {

using UnitId = std::uint64_t;
using BasisPoints = std::int32_t;

enum class Side : std::uint8_t { attacker, defender };
enum class UnitKind : std::uint8_t { hero, beauty, pet, artifact };

struct Stats {
    std::int64_t hp{1};
    std::int64_t attack{0};
    std::int64_t defense{0};
    std::int64_t speed{0};
    BasisPoints crit_rate_bp{0};
};

struct UnitConfig {
    UnitId id{0};
    UnitKind kind{UnitKind::hero};
    std::int32_t position{0};
    Stats final_stats;
};

struct Formation {
    std::string name;
    std::vector<UnitConfig> units;
};

struct BattleRequest {
    std::uint64_t battle_id{0};
    std::uint64_t seed{1};
    std::int32_t max_rounds{50};
    Formation attacker;
    Formation defender;
    std::optional<Side> forced_first_side;
};

} // namespace mini

int main() {
    using namespace mini;

    BattleRequest request{
        .battle_id = 3001,
        .seed = 42,
        .attacker = Formation{
            .name = "crane_wing",
            .units = {
                UnitConfig{.id = 1001, .position = 1,
                           .final_stats = {.hp = 1800, .attack = 260, .defense = 80, .speed = 120}},
            }},
        .defender = Formation{
            .name = "square",
            .units = {
                UnitConfig{.id = 2001, .position = 1,
                           .final_stats = {.hp = 1500, .attack = 240, .defense = 90, .speed = 110}},
            }},
        .forced_first_side = std::nullopt,
    };

    std::cout << "battle " << request.battle_id
              << " max_rounds=" << request.max_rounds << '\n';
    for (const auto& unit : request.attacker.units) {
        std::cout << "attacker " << unit.id << " hp=" << unit.final_stats.hp << '\n';
    }
    if (!request.forced_first_side.has_value()) {
        std::cout << "first side: decided by initiative\n";
    }
}
