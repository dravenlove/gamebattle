#pragma once

#include "gamebattle/engine.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <unordered_map>

namespace gamebattle {

// Immutable after construction. A ConfigStore can therefore be shared by all
// concurrent battles; UnitConfig receives value snapshots through assign_loadout.
class ConfigStore final {
public:
    static constexpr std::uint16_t format_major = 2;
    static constexpr std::uint16_t format_minor = 0;

    static ConfigStore load_file(const std::filesystem::path& path);

    const BuffSpec* find_buff(std::uint32_t id) const noexcept;
    const Effect* find_effect(std::uint32_t id) const noexcept;
    const Skill* find_skill(std::uint32_t id) const noexcept;
    const Passive* find_passive(std::uint32_t id) const noexcept;

    const BuffSpec& require_buff(std::uint32_t id) const;
    const Effect& require_effect(std::uint32_t id) const;
    const Skill& require_skill(std::uint32_t id) const;
    const Passive& require_passive(std::uint32_t id) const;

    // Resolves ids first and only updates the unit if every id exists. Existing
    // embedded loadout data is replaced, which makes request semantics explicit.
    void assign_loadout(UnitConfig& unit,
                        std::span<const std::uint32_t> skill_ids,
                        std::span<const std::uint32_t> passive_ids) const;

    std::size_t buff_count() const noexcept { return buffs_.size(); }
    std::size_t effect_count() const noexcept { return effects_.size(); }
    std::size_t skill_count() const noexcept { return skills_.size(); }
    std::size_t passive_count() const noexcept { return passives_.size(); }

private:
    std::unordered_map<std::uint32_t, std::shared_ptr<const BuffSpec>> buffs_;
    std::unordered_map<std::uint32_t, Effect> effects_;
    std::unordered_map<std::uint32_t, Skill> skills_;
    std::unordered_map<std::uint32_t, Passive> passives_;
};

} // namespace gamebattle
