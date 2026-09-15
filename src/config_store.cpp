#include "gamebattle/config_store.hpp"

#include <algorithm>
#include <bit>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gamebattle {
namespace {

constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kMaxPackBytes = 64U * 1024U * 1024U;
constexpr std::uint32_t kMaxRecordsPerTable = 1'000'000;
constexpr std::uint32_t kMaxReferences = 4096;
constexpr std::uint32_t kMaxStringBytes = 1024U * 1024U;

class Reader final {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    std::uint8_t u8() {
        require(1);
        return bytes_[offset_++];
    }

    std::uint16_t u16() {
        require(2);
        const auto value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes_[offset_]) |
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(bytes_[offset_ + 1]) << 8U));
        offset_ += 2;
        return value;
    }

    std::uint32_t u32() {
        require(4);
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            value |= static_cast<std::uint32_t>(bytes_[offset_ + index])
                     << (index * 8U);
        }
        offset_ += 4;
        return value;
    }

    std::int32_t i32() {
        return std::bit_cast<std::int32_t>(u32());
    }

    std::int64_t i64() {
        require(8);
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < 8; ++index) {
            value |= static_cast<std::uint64_t>(bytes_[offset_ + index])
                     << (index * 8U);
        }
        offset_ += 8;
        return std::bit_cast<std::int64_t>(value);
    }

    std::string string() {
        const auto length = u32();
        if (length > kMaxStringBytes) {
            throw std::runtime_error("config string exceeds the 1 MiB limit");
        }
        require(length);
        const auto* begin = reinterpret_cast<const char*>(bytes_.data() + offset_);
        std::string result(begin, begin + length);
        offset_ += length;
        return result;
    }

    bool empty() const noexcept { return offset_ == bytes_.size(); }

private:
    void require(std::size_t count) const {
        if (count > bytes_.size() - offset_) {
            throw std::runtime_error("truncated gamebattle config pack");
        }
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{0};
};

struct RawEffect {
    std::uint32_t id{0};
    Effect value;
    std::uint32_t buff_id{0};
};

struct RawModifier {
    std::uint32_t buff_id{0};
    std::uint32_t sequence{0};
    AttributeModifier value;
};

struct RawReaction {
    std::uint32_t buff_id{0};
    std::uint32_t sequence{0};
    std::int32_t priority{0};
    Trigger trigger{Trigger::round_end};
    EffectSource source{EffectSource::owner};
    StackScaling stack_scaling{StackScaling::once};
    BasisPoints chance_bp{10000};
    std::int32_t max_triggers_per_round{0};
    std::vector<std::uint32_t> effect_ids;
};

struct RawSkill {
    std::uint32_t id{0};
    std::string name;
    BasisPoints chance_bp{10000};
    std::int32_t priority{0};
    std::vector<std::uint32_t> effect_ids;
};

struct RawPassive {
    std::uint32_t id{0};
    std::string name;
    Trigger trigger{Trigger::on_damaged};
    std::int32_t priority{0};
    BasisPoints chance_bp{10000};
    std::int32_t max_triggers_per_round{0};
    std::vector<std::uint32_t> effect_ids;
};

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t result = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        result ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(result & 1U));
            result = (result >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~result;
}

void check_count(std::uint32_t count, const char* table) {
    if (count > kMaxRecordsPerTable) {
        throw std::runtime_error(std::string(table) + " record count exceeds the limit");
    }
}

template <typename Enum>
Enum checked_enum(std::uint8_t value, std::uint8_t maximum, const char* field) {
    if (value > maximum) {
        throw std::runtime_error(std::string(field) + " contains an unknown enum value");
    }
    return static_cast<Enum>(value);
}

std::vector<std::uint32_t> read_ids(Reader& reader) {
    const auto count = reader.u32();
    if (count == 0 || count > kMaxReferences) {
        throw std::runtime_error("effect id list must contain between 1 and 4096 entries");
    }
    std::vector<std::uint32_t> result;
    result.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto id = reader.u32();
        if (id == 0) {
            throw std::runtime_error("effect id references must be non-zero");
        }
        result.push_back(id);
    }
    return result;
}

void validate_buff(const BuffSpec& buff) {
    const bool valid_lifetime =
        (buff.lifetime.permanent && buff.lifetime.duration == 0) ||
        (!buff.lifetime.permanent && buff.lifetime.duration >= 1 &&
         buff.lifetime.duration <= 10000);
    if (buff.id == 0 || buff.name.empty() || !valid_lifetime ||
        buff.stacking.max_stacks < 1 || buff.stacking.max_stacks > 1000 ||
        (buff.stacking.mode == StackPolicy::refresh &&
         buff.stacking.max_stacks != 1)) {
        throw std::runtime_error("buff fields are outside supported bounds");
    }
}

void validate_modifier(const RawModifier& modifier) {
    if (modifier.buff_id == 0 || modifier.sequence == 0 ||
        modifier.sequence > kMaxReferences ||
        modifier.value.value < -1'000'000'000'000LL ||
        modifier.value.value > 1'000'000'000'000LL ||
        (modifier.value.operation == ModifierOperation::scale_bp &&
         (modifier.value.value < -1'000'000 ||
          modifier.value.value > 1'000'000))) {
        throw std::runtime_error("buff modifier fields are outside supported bounds");
    }
}

void validate_reaction(const RawReaction& reaction) {
    if (reaction.buff_id == 0 || reaction.sequence == 0 ||
        reaction.sequence > kMaxReferences || reaction.priority < -1'000'000 ||
        reaction.priority > 1'000'000 || reaction.chance_bp < 0 ||
        reaction.chance_bp > 10000 || reaction.max_triggers_per_round < 0 ||
        reaction.max_triggers_per_round > 10000 || reaction.effect_ids.empty()) {
        throw std::runtime_error("buff reaction fields are outside supported bounds");
    }
}

void validate_effect(const RawEffect& effect) {
    if (effect.id == 0 || effect.value.target_count < 1 ||
        effect.value.target_count > 256 || effect.value.attack_bp < 0 ||
        effect.value.attack_bp > 1'000'000 ||
        effect.value.flat < -1'000'000'000'000LL ||
        effect.value.flat > 1'000'000'000'000LL) {
        throw std::runtime_error("effect fields are outside supported bounds");
    }
    if (effect.value.kind == EffectKind::add_buff && effect.buff_id == 0) {
        throw std::runtime_error("add_buff effect has no buff reference");
    }
    if (effect.value.kind != EffectKind::add_buff && effect.buff_id != 0) {
        throw std::runtime_error("only add_buff effects may contain a buff reference");
    }
    if (effect.value.kind == EffectKind::remove_buff &&
        effect.value.remove_buff_id == 0) {
        throw std::runtime_error("remove_buff effect has no buff reference");
    }
    if (effect.value.kind != EffectKind::remove_buff &&
        effect.value.remove_buff_id != 0) {
        throw std::runtime_error("only remove_buff effects may contain remove_buff_id");
    }
}

template <typename T>
void insert_unique(std::unordered_map<std::uint32_t, T>& destination,
                   std::uint32_t id, T value, const char* table) {
    if (!destination.emplace(id, std::move(value)).second) {
        throw std::runtime_error(std::string("duplicate id in ") + table);
    }
}

template <typename T>
const T& require_item(const std::unordered_map<std::uint32_t, T>& values,
                      std::uint32_t id, const char* kind) {
    const auto found = values.find(id);
    if (found == values.end()) {
        throw std::out_of_range(std::string("unknown ") + kind + " id " +
                                std::to_string(id));
    }
    return found->second;
}

} // namespace

ConfigStore ConfigStore::load_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open gamebattle config pack: " + path.string());
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > kMaxPackBytes) {
        throw std::runtime_error("gamebattle config pack exceeds the 64 MiB limit");
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes;
    bytes.assign(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
    if (!input.eof() && input.fail()) {
        throw std::runtime_error("failed while reading gamebattle config pack");
    }
    if (bytes.size() < kHeaderBytes) {
        throw std::runtime_error("gamebattle config pack is smaller than its header");
    }

    Reader header(std::span<const std::uint8_t>(bytes).first(kHeaderBytes));
    if (header.u8() != 'G' || header.u8() != 'B' || header.u8() != 'C' ||
        header.u8() != 'F') {
        throw std::runtime_error("invalid gamebattle config magic");
    }
    const auto major = header.u16();
    const auto minor = header.u16();
    const auto payload_size = header.u32();
    const auto expected_crc = header.u32();
    if (major != format_major || minor != format_minor) {
        throw std::runtime_error("unsupported gamebattle config format " +
                                 std::to_string(major) + "." +
                                 std::to_string(minor));
    }
    if (payload_size != bytes.size() - kHeaderBytes) {
        throw std::runtime_error("gamebattle config payload length does not match its header");
    }
    const auto payload = std::span<const std::uint8_t>(bytes).subspan(kHeaderBytes);
    if (crc32(payload) != expected_crc) {
        throw std::runtime_error("gamebattle config CRC32 check failed");
    }

    Reader reader(payload);
    const auto buff_count = reader.u32();
    const auto modifier_count = reader.u32();
    const auto reaction_count = reader.u32();
    const auto effect_count = reader.u32();
    const auto skill_count = reader.u32();
    const auto passive_count = reader.u32();
    check_count(buff_count, "buff");
    check_count(modifier_count, "buff modifier");
    check_count(reaction_count, "buff reaction");
    check_count(effect_count, "effect");
    check_count(skill_count, "skill");
    check_count(passive_count, "passive");

    // Phase one creates mutable definition shells. No shell escapes this
    // function until every reference and ownership edge has been validated.
    std::unordered_map<std::uint32_t, std::shared_ptr<BuffSpec>> mutable_buffs;
    mutable_buffs.reserve(buff_count);
    for (std::uint32_t index = 0; index < buff_count; ++index) {
        auto buff = std::make_shared<BuffSpec>();
        buff->id = reader.u32();
        buff->name = reader.string();
        const auto permanent = reader.u8();
        if (permanent > 1) {
            throw std::runtime_error("buff.lifetime.permanent must be 0 or 1");
        }
        buff->lifetime.permanent = permanent != 0;
        buff->lifetime.duration = reader.i32();
        buff->lifetime.decrement_on =
            checked_enum<Trigger>(reader.u8(), 8, "buff.lifetime.decrement_on");
        buff->stacking.max_stacks = reader.i32();
        buff->stacking.key = checked_enum<StackKeyPolicy>(
            reader.u8(), 1, "buff.stacking.key");
        buff->stacking.mode =
            checked_enum<StackPolicy>(reader.u8(), 1, "buff.stacking.mode");
        buff->stacking.refresh = checked_enum<RefreshPolicy>(
            reader.u8(), 2, "buff.stacking.refresh");
        validate_buff(*buff);
        const auto buff_id = buff->id;
        insert_unique(mutable_buffs, buff_id, std::move(buff), "buffs");
    }

    std::vector<RawModifier> raw_modifiers;
    raw_modifiers.reserve(modifier_count);
    std::set<std::pair<std::uint32_t, std::uint32_t>> modifier_keys;
    for (std::uint32_t index = 0; index < modifier_count; ++index) {
        RawModifier raw;
        raw.buff_id = reader.u32();
        raw.sequence = reader.u32();
        raw.value.attribute =
            checked_enum<Attribute>(reader.u8(), 8, "buff_modifier.attribute");
        raw.value.operation = checked_enum<ModifierOperation>(
            reader.u8(), 1, "buff_modifier.operation");
        raw.value.value = reader.i64();
        validate_modifier(raw);
        if (!mutable_buffs.contains(raw.buff_id)) {
            throw std::runtime_error("buff modifier references an unknown buff");
        }
        if (!modifier_keys.emplace(raw.buff_id, raw.sequence).second) {
            throw std::runtime_error("duplicate buff modifier sequence");
        }
        raw_modifiers.push_back(std::move(raw));
    }

    std::vector<RawReaction> raw_reactions;
    raw_reactions.reserve(reaction_count);
    std::set<std::pair<std::uint32_t, std::uint32_t>> reaction_keys;
    for (std::uint32_t index = 0; index < reaction_count; ++index) {
        RawReaction raw;
        raw.buff_id = reader.u32();
        raw.sequence = reader.u32();
        raw.priority = reader.i32();
        raw.trigger =
            checked_enum<Trigger>(reader.u8(), 8, "buff_reaction.trigger");
        raw.source = checked_enum<EffectSource>(
            reader.u8(), 1, "buff_reaction.source");
        raw.stack_scaling = checked_enum<StackScaling>(
            reader.u8(), 1, "buff_reaction.stack_scaling");
        raw.chance_bp = reader.i32();
        raw.max_triggers_per_round = reader.i32();
        raw.effect_ids = read_ids(reader);
        validate_reaction(raw);
        if (!mutable_buffs.contains(raw.buff_id)) {
            throw std::runtime_error("buff reaction references an unknown buff");
        }
        if (!reaction_keys.emplace(raw.buff_id, raw.sequence).second) {
            throw std::runtime_error("duplicate buff reaction sequence");
        }
        raw_reactions.push_back(std::move(raw));
    }

    std::vector<RawEffect> raw_effects;
    raw_effects.reserve(effect_count);
    std::unordered_map<std::uint32_t, std::size_t> raw_effect_indexes;
    raw_effect_indexes.reserve(effect_count);
    for (std::uint32_t index = 0; index < effect_count; ++index) {
        RawEffect raw;
        raw.id = reader.u32();
        raw.value.kind = checked_enum<EffectKind>(reader.u8(), 4, "effect.kind");
        raw.value.target = checked_enum<TargetRule>(reader.u8(), 6, "effect.target");
        raw.value.target_count = reader.i32();
        raw.value.attack_bp = reader.i32();
        raw.value.flat = reader.i64();
        raw.buff_id = reader.u32();
        raw.value.remove_buff_id = reader.u32();
        validate_effect(raw);
        if (!raw_effect_indexes.emplace(raw.id, raw_effects.size()).second) {
            throw std::runtime_error("duplicate id in effects");
        }
        raw_effects.push_back(std::move(raw));
    }

    std::vector<RawSkill> raw_skills;
    raw_skills.reserve(skill_count);
    for (std::uint32_t index = 0; index < skill_count; ++index) {
        RawSkill raw;
        raw.id = reader.u32();
        raw.name = reader.string();
        raw.chance_bp = reader.i32();
        raw.priority = reader.i32();
        raw.effect_ids = read_ids(reader);
        if (raw.id == 0 || raw.name.empty() || raw.chance_bp < 0 ||
            raw.chance_bp > 10000) {
            throw std::runtime_error("skill fields are outside supported bounds");
        }
        raw_skills.push_back(std::move(raw));
    }

    std::vector<RawPassive> raw_passives;
    raw_passives.reserve(passive_count);
    for (std::uint32_t index = 0; index < passive_count; ++index) {
        RawPassive raw;
        raw.id = reader.u32();
        raw.name = reader.string();
        raw.trigger = checked_enum<Trigger>(reader.u8(), 8, "passive.trigger");
        raw.priority = reader.i32();
        raw.chance_bp = reader.i32();
        raw.max_triggers_per_round = reader.i32();
        raw.effect_ids = read_ids(reader);
        if (raw.id == 0 || raw.name.empty() || raw.chance_bp < 0 ||
            raw.chance_bp > 10000 || raw.priority < -1'000'000 ||
            raw.priority > 1'000'000 || raw.max_triggers_per_round < 0 ||
            raw.max_triggers_per_round > 10000) {
            throw std::runtime_error("passive fields are outside supported bounds");
        }
        raw_passives.push_back(std::move(raw));
    }
    if (!reader.empty()) {
        throw std::runtime_error("gamebattle config pack contains trailing bytes");
    }

    for (const auto& raw : raw_effects) {
        if (raw.value.kind == EffectKind::add_buff &&
            !mutable_buffs.contains(raw.buff_id)) {
            throw std::runtime_error("add_buff effect references an unknown buff");
        }
        if (raw.value.kind == EffectKind::remove_buff &&
            !mutable_buffs.contains(raw.value.remove_buff_id)) {
            throw std::runtime_error("remove_buff effect references an unknown buff");
        }
    }
    for (const auto& raw : raw_reactions) {
        for (const auto effect_id : raw.effect_ids) {
            if (!raw_effect_indexes.contains(effect_id)) {
                throw std::runtime_error("buff reaction references an unknown effect");
            }
        }
    }

    // Reactions own Effect values and add_buff Effects own shared BuffSpec
    // references. A cycle here would therefore be a shared_ptr ownership cycle.
    std::map<std::uint32_t, std::uint32_t> indegree;
    std::map<std::uint32_t, std::vector<std::uint32_t>> edges;
    std::set<std::pair<std::uint32_t, std::uint32_t>> unique_edges;
    for (const auto& [buff_id, buff] : mutable_buffs) {
        static_cast<void>(buff);
        indegree.emplace(buff_id, 0);
    }
    for (const auto& reaction : raw_reactions) {
        for (const auto effect_id : reaction.effect_ids) {
            const auto& effect = raw_effects[raw_effect_indexes.at(effect_id)];
            if (effect.value.kind != EffectKind::add_buff) continue;
            const auto edge = std::pair{reaction.buff_id, effect.buff_id};
            if (unique_edges.insert(edge).second) {
                edges[edge.first].push_back(edge.second);
                ++indegree.at(edge.second);
            }
        }
    }
    std::vector<std::uint32_t> ready;
    ready.reserve(indegree.size());
    for (const auto& [buff_id, degree] : indegree) {
        if (degree == 0) ready.push_back(buff_id);
    }
    std::size_t visited = 0;
    for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
        const auto buff_id = ready[cursor];
        ++visited;
        for (const auto target : edges[buff_id]) {
            auto& degree = indegree.at(target);
            if (--degree == 0) ready.push_back(target);
        }
    }
    if (visited != mutable_buffs.size()) {
        throw std::runtime_error(
            "buff reaction add_buff graph contains an ownership cycle");
    }

    std::sort(raw_modifiers.begin(), raw_modifiers.end(),
              [](const RawModifier& left, const RawModifier& right) {
                  return std::pair{left.buff_id, left.sequence} <
                         std::pair{right.buff_id, right.sequence};
              });
    for (const auto& raw : raw_modifiers) {
        mutable_buffs.at(raw.buff_id)->modifiers.push_back(raw.value);
    }

    ConfigStore store;
    store.effects_.reserve(effect_count);
    for (auto& raw : raw_effects) {
        if (raw.value.kind == EffectKind::add_buff) {
            raw.value.buff = mutable_buffs.at(raw.buff_id);
        }
        insert_unique(store.effects_, raw.id, std::move(raw.value), "effects");
    }

    std::sort(raw_reactions.begin(), raw_reactions.end(),
              [](const RawReaction& left, const RawReaction& right) {
                  return std::pair{left.buff_id, left.sequence} <
                         std::pair{right.buff_id, right.sequence};
              });
    for (const auto& raw : raw_reactions) {
        BuffReaction reaction;
        reaction.trigger = raw.trigger;
        reaction.priority = raw.priority;
        reaction.source = raw.source;
        reaction.stack_scaling = raw.stack_scaling;
        reaction.chance_bp = raw.chance_bp;
        reaction.max_triggers_per_round = raw.max_triggers_per_round;
        reaction.effects.reserve(raw.effect_ids.size());
        for (const auto effect_id : raw.effect_ids) {
            reaction.effects.push_back(store.require_effect(effect_id));
        }
        mutable_buffs.at(raw.buff_id)->reactions.push_back(std::move(reaction));
    }

    store.buffs_.reserve(buff_count);
    for (auto& [id, buff] : mutable_buffs) {
        std::shared_ptr<const BuffSpec> immutable = std::move(buff);
        insert_unique(store.buffs_, id, std::move(immutable), "buffs");
    }

    store.skills_.reserve(skill_count);
    for (auto& raw : raw_skills) {
        Skill skill;
        skill.id = raw.id;
        skill.name = std::move(raw.name);
        skill.chance_bp = raw.chance_bp;
        skill.priority = raw.priority;
        skill.effects.reserve(raw.effect_ids.size());
        for (const auto effect_id : raw.effect_ids) {
            skill.effects.push_back(store.require_effect(effect_id));
        }
        insert_unique(store.skills_, skill.id, std::move(skill), "skills");
    }

    store.passives_.reserve(passive_count);
    for (auto& raw : raw_passives) {
        Passive passive;
        passive.id = raw.id;
        passive.name = std::move(raw.name);
        passive.trigger = raw.trigger;
        passive.priority = raw.priority;
        passive.chance_bp = raw.chance_bp;
        passive.max_triggers_per_round = raw.max_triggers_per_round;
        passive.effects.reserve(raw.effect_ids.size());
        for (const auto effect_id : raw.effect_ids) {
            passive.effects.push_back(store.require_effect(effect_id));
        }
        insert_unique(store.passives_, passive.id, std::move(passive), "passives");
    }
    return store;
}

const BuffSpec* ConfigStore::find_buff(std::uint32_t id) const noexcept {
    const auto found = buffs_.find(id);
    return found == buffs_.end() ? nullptr : found->second.get();
}

const Effect* ConfigStore::find_effect(std::uint32_t id) const noexcept {
    const auto found = effects_.find(id);
    return found == effects_.end() ? nullptr : &found->second;
}

const Skill* ConfigStore::find_skill(std::uint32_t id) const noexcept {
    const auto found = skills_.find(id);
    return found == skills_.end() ? nullptr : &found->second;
}

const Passive* ConfigStore::find_passive(std::uint32_t id) const noexcept {
    const auto found = passives_.find(id);
    return found == passives_.end() ? nullptr : &found->second;
}

const BuffSpec& ConfigStore::require_buff(std::uint32_t id) const {
    return *require_item(buffs_, id, "buff");
}

const Effect& ConfigStore::require_effect(std::uint32_t id) const {
    return require_item(effects_, id, "effect");
}

const Skill& ConfigStore::require_skill(std::uint32_t id) const {
    return require_item(skills_, id, "skill");
}

const Passive& ConfigStore::require_passive(std::uint32_t id) const {
    return require_item(passives_, id, "passive");
}

void ConfigStore::assign_loadout(UnitConfig& unit,
                                 std::span<const std::uint32_t> skill_ids,
                                 std::span<const std::uint32_t> passive_ids) const {
    std::vector<Skill> skills;
    std::vector<Passive> passives;
    skills.reserve(skill_ids.size());
    passives.reserve(passive_ids.size());
    for (const auto id : skill_ids) {
        skills.push_back(require_skill(id));
    }
    for (const auto id : passive_ids) {
        passives.push_back(require_passive(id));
    }
    unit.skills = std::move(skills);
    unit.passives = std::move(passives);
}

} // namespace gamebattle
