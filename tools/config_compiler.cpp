#include <algorithm>
#include <bit>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace {

namespace fs = std::filesystem;

constexpr std::uint16_t kFormatMajor = 3;
constexpr std::uint16_t kFormatMinor = 0;
constexpr std::size_t kMaxTableBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaxStringBytes = 1024U * 1024U;

class ConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct CsvRecord {
    std::size_t line{0};
    std::vector<std::string> fields;
};

struct Row {
    std::string source;
    std::size_t line{0};
    std::unordered_map<std::string, std::string> cells;
};

struct BuffRow {
    std::uint32_t id{0};
    std::string name;
    bool permanent{false};
    std::int32_t duration{1};
    std::uint8_t decrement_on{8};
    std::int32_t max_stacks{1};
    std::uint8_t stack_key{0};
    std::uint8_t stack_policy{0};
    std::uint8_t refresh_policy{0};
};

struct ModifierRow {
    std::uint32_t buff_id{0};
    std::uint32_t sequence{0};
    std::uint8_t attribute{0};
    std::uint8_t operation{0};
    std::int64_t value{0};
};

struct ReactionRow {
    std::uint32_t buff_id{0};
    std::uint32_t sequence{0};
    std::int32_t priority{0};
    std::uint8_t trigger{8};
    std::uint8_t source{0};
    std::uint8_t stack_scaling{0};
    std::int32_t chance_bp{10000};
    std::int32_t max_triggers_per_round{0};
    std::vector<std::uint32_t> effect_ids;
    std::string source_file;
    std::size_t source_line{0};
};

struct EffectRow {
    std::uint32_t id{0};
    std::uint8_t kind{0};
    std::uint8_t target{0};
    std::int32_t target_count{1};
    std::int32_t attack_bp{0};
    std::int64_t flat{0};
    std::uint32_t buff_id{0};
    std::uint32_t remove_buff_id{0};
};

struct SkillRow {
    std::uint32_t id{0};
    std::string name;
    std::int32_t chance_bp{10000};
    std::int32_t priority{0};
    std::vector<std::uint32_t> effect_ids;
};

struct PassiveRow {
    std::uint32_t id{0};
    std::string name;
    std::uint8_t trigger{0};
    std::int32_t priority{0};
    std::int32_t chance_bp{10000};
    std::int32_t max_triggers_per_round{0};
    std::vector<std::uint32_t> effect_ids;
};

struct Tables {
    std::map<std::uint32_t, BuffRow> buffs;
    std::map<std::pair<std::uint32_t, std::uint32_t>, ModifierRow> modifiers;
    std::map<std::pair<std::uint32_t, std::uint32_t>, ReactionRow> reactions;
    std::map<std::uint32_t, EffectRow> effects;
    std::map<std::uint32_t, SkillRow> skills;
    std::map<std::uint32_t, PassiveRow> passives;
};

const std::unordered_map<std::string, std::uint8_t> kEffectKinds{
    {"damage", std::uint8_t{0}}, {"heal", std::uint8_t{1}},
    {"add_buff", std::uint8_t{2}}, {"remove_buff", std::uint8_t{3}},
    {"direct_damage", std::uint8_t{4}}
};

const std::unordered_map<std::string, std::uint8_t> kTargetRules{
    {"self", std::uint8_t{0}}, {"trigger_unit", std::uint8_t{1}},
    {"enemy_front", std::uint8_t{2}},
    {"enemy_lowest_hp", std::uint8_t{3}},
    {"ally_lowest_hp", std::uint8_t{4}},
    {"all_enemies", std::uint8_t{5}}, {"all_allies", std::uint8_t{6}}
};

const std::unordered_map<std::string, std::uint8_t> kTriggers{
    {"battle_start", std::uint8_t{0}}, {"round_start", std::uint8_t{1}},
    {"before_action", std::uint8_t{2}}, {"on_attack", std::uint8_t{3}},
    {"on_hit", std::uint8_t{4}}, {"on_damaged", std::uint8_t{5}},
    {"unit_death", std::uint8_t{6}}, {"after_action", std::uint8_t{7}},
    {"round_end", std::uint8_t{8}}
};

const std::unordered_map<std::string, std::uint8_t> kLifetimes{
    {"finite", std::uint8_t{0}}, {"permanent", std::uint8_t{1}}
};

const std::unordered_map<std::string, std::uint8_t> kStackPolicies{
    {"stack", std::uint8_t{0}}, {"refresh", std::uint8_t{1}}
};

const std::unordered_map<std::string, std::uint8_t> kStackKeyPolicies{
    {"by_buff", std::uint8_t{0}},
    {"by_buff_and_source", std::uint8_t{1}}
};

const std::unordered_map<std::string, std::uint8_t> kRefreshPolicies{
    {"reset", std::uint8_t{0}}, {"extend", std::uint8_t{1}},
    {"keep", std::uint8_t{2}}
};

const std::unordered_map<std::string, std::uint8_t> kAttributes{
    {"attack", std::uint8_t{0}}, {"defense", std::uint8_t{1}},
    {"speed", std::uint8_t{2}}, {"crit_rate_bp", std::uint8_t{3}},
    {"crit_damage_bp", std::uint8_t{4}},
    {"hit_rate_bp", std::uint8_t{5}},
    {"dodge_rate_bp", std::uint8_t{6}},
    {"damage_bonus_bp", std::uint8_t{7}},
    {"damage_reduction_bp", std::uint8_t{8}}
};

const std::unordered_map<std::string, std::uint8_t> kModifierOperations{
    {"add", std::uint8_t{0}}, {"scale_bp", std::uint8_t{1}}
};

const std::unordered_map<std::string, std::uint8_t> kEffectSources{
    {"owner", std::uint8_t{0}}, {"applier", std::uint8_t{1}}
};

const std::unordered_map<std::string, std::uint8_t> kStackScalings{
    {"once", std::uint8_t{0}}, {"per_stack", std::uint8_t{1}}
};

std::string path_text(const fs::path& path) {
    const auto utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

[[noreturn]] void row_error(const Row& row, std::string message) {
    throw ConfigError(row.source + ":" + std::to_string(row.line) + ": " +
                      std::move(message));
}

bool valid_utf8(std::string_view value) {
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        std::size_t continuation_count = 0;
        if (first >= 0xC2U && first <= 0xDFU) continuation_count = 1;
        else if (first >= 0xE0U && first <= 0xEFU) continuation_count = 2;
        else if (first >= 0xF0U && first <= 0xF4U) continuation_count = 3;
        else return false;
        if (index + continuation_count >= value.size()) return false;
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xC0U) != 0x80U) return false;
        }
        const auto second = static_cast<unsigned char>(value[index + 1]);
        if ((first == 0xE0U && second < 0xA0U) ||
            (first == 0xEDU && second > 0x9FU) ||
            (first == 0xF0U && second < 0x90U) ||
            (first == 0xF4U && second > 0x8FU)) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

std::string read_file(const fs::path& path) {
    std::error_code size_error;
    const auto size = fs::file_size(path, size_error);
    if (size_error) {
        throw ConfigError("cannot read required table " + path_text(path) +
                          ": " + size_error.message());
    }
    if (size > kMaxTableBytes) {
        throw ConfigError("table exceeds the 64 MiB limit: " + path_text(path));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw ConfigError("cannot open required table: " + path_text(path));
    }
    std::string result;
    result.assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
    if (!input.eof() && input.fail()) {
        throw ConfigError("failed while reading table: " + path_text(path));
    }
    if (result.size() >= 3 &&
        static_cast<unsigned char>(result[0]) == 0xEFU &&
        static_cast<unsigned char>(result[1]) == 0xBBU &&
        static_cast<unsigned char>(result[2]) == 0xBFU) {
        result.erase(0, 3);
    }
    if (!valid_utf8(result)) {
        throw ConfigError("table is not valid UTF-8: " + path_text(path));
    }
    return result;
}

std::vector<CsvRecord> parse_csv(const fs::path& path) {
    const auto data = read_file(path);
    std::vector<CsvRecord> records;
    std::vector<std::string> fields;
    std::string field;
    std::size_t line = 1;
    std::size_t record_line = 1;
    bool quoted = false;
    bool quote_closed = false;

    const auto finish_record = [&]() {
        fields.push_back(trim(field));
        field.clear();
        quote_closed = false;
        const bool nonempty = std::any_of(
            fields.begin(), fields.end(),
            [](const std::string& item) { return !item.empty(); });
        if (nonempty) {
            records.push_back(CsvRecord{record_line, std::move(fields)});
        }
        fields.clear();
        record_line = line;
    };

    for (std::size_t index = 0; index < data.size();) {
        const char current = data[index];
        if (quoted) {
            if (current == '"') {
                if (index + 1 < data.size() && data[index + 1] == '"') {
                    field.push_back('"');
                    index += 2;
                } else {
                    quoted = false;
                    quote_closed = true;
                    ++index;
                }
            } else if (current == '\r' || current == '\n') {
                field.push_back('\n');
                if (current == '\r' && index + 1 < data.size() &&
                    data[index + 1] == '\n') {
                    index += 2;
                } else {
                    ++index;
                }
                ++line;
            } else {
                field.push_back(current);
                ++index;
            }
            continue;
        }

        if (quote_closed) {
            if (current == ' ' || current == '\t') {
                ++index;
                continue;
            }
            if (current != ',' && current != '\r' && current != '\n') {
                throw ConfigError(path_text(path) + ":" + std::to_string(line) +
                                  ": unexpected character after closing quote");
            }
        }

        if (current == '"') {
            if (!field.empty()) {
                throw ConfigError(path_text(path) + ":" + std::to_string(line) +
                                  ": quote must begin at the start of a CSV field");
            }
            quoted = true;
            ++index;
        } else if (current == ',') {
            fields.push_back(trim(field));
            field.clear();
            quote_closed = false;
            ++index;
        } else if (current == '\r' || current == '\n') {
            if (current == '\r' && index + 1 < data.size() &&
                data[index + 1] == '\n') {
                index += 2;
            } else {
                ++index;
            }
            ++line;
            finish_record();
        } else {
            field.push_back(current);
            ++index;
        }
    }
    if (quoted) {
        throw ConfigError(path_text(path) + ":" + std::to_string(record_line) +
                          ": unterminated quoted CSV field");
    }
    if (!field.empty() || !fields.empty() || quote_closed) {
        finish_record();
    }
    return records;
}

std::vector<std::string> expected_columns(std::string_view filename) {
    if (filename == "buffs.csv") {
        return {"id", "name", "lifetime", "duration", "decrement_on",
                "max_stacks", "stack_key", "stack_policy", "refresh_policy",
                "notes"};
    }
    if (filename == "buff_modifiers.csv") {
        return {"buff_id", "sequence", "attribute", "operation", "value",
                "notes"};
    }
    if (filename == "buff_reactions.csv") {
        return {"buff_id", "sequence", "priority", "trigger", "source",
                "stack_scaling", "chance_bp", "max_triggers_per_round",
                "effect_ids", "notes"};
    }
    if (filename == "effects.csv") {
        return {"id", "type", "target", "target_count", "attack_bp", "flat",
                "buff_id", "remove_buff_id", "notes"};
    }
    if (filename == "skills.csv") {
        return {"id", "name", "chance_bp", "priority", "effect_ids", "notes"};
    }
    return {"id", "name", "trigger", "priority", "chance_bp",
            "max_triggers_per_round", "effect_ids", "notes"};
}

std::vector<Row> read_rows(const fs::path& directory, std::string filename) {
    const auto path = directory / fs::path(filename);
    const auto records = parse_csv(path);
    if (records.empty()) {
        throw ConfigError(path_text(path) + ": missing header row");
    }
    const auto expected = expected_columns(filename);
    const auto& header = records.front().fields;
    std::map<std::string, bool> expected_set;
    for (const auto& column : expected) {
        expected_set.emplace(column, false);
    }
    std::vector<std::string> unexpected;
    for (const auto& column : header) {
        const auto found = expected_set.find(column);
        if (found == expected_set.end()) {
            unexpected.push_back(column);
        } else if (found->second) {
            throw ConfigError(path_text(path) + ": duplicate column " + column);
        } else {
            found->second = true;
        }
    }
    std::vector<std::string> missing;
    for (const auto& [column, present] : expected_set) {
        if (!present) {
            missing.push_back(column);
        }
    }
    if (!missing.empty() || !unexpected.empty()) {
        std::string message = path_text(path) + ": invalid columns";
        if (!missing.empty()) {
            message += "; missing: ";
            for (const auto& column : missing) message += column + " ";
        }
        if (!unexpected.empty()) {
            message += "; unexpected: ";
            for (const auto& column : unexpected) message += column + " ";
        }
        throw ConfigError(message);
    }

    std::vector<Row> rows;
    rows.reserve(records.size() - 1);
    for (std::size_t index = 1; index < records.size(); ++index) {
        const auto& record = records[index];
        if (record.fields.size() != header.size()) {
            throw ConfigError(filename + ":" + std::to_string(record.line) +
                              ": expected " + std::to_string(header.size()) +
                              " columns but found " +
                              std::to_string(record.fields.size()));
        }
        Row row{filename, record.line, {}};
        for (std::size_t column = 0; column < header.size(); ++column) {
            row.cells.emplace(header[column], record.fields[column]);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

const std::string& cell(const Row& row, std::string_view key) {
    return row.cells.at(std::string(key));
}

std::string required_text(const Row& row, std::string_view key) {
    const auto& value = cell(row, key);
    if (value.empty()) {
        row_error(row, std::string(key) + " is required");
    }
    return value;
}

std::int64_t integer(const Row& row, std::string_view key,
                     std::int64_t default_value, bool has_default = true) {
    const auto& source = cell(row, key);
    if (source.empty()) {
        if (has_default) return default_value;
        row_error(row, std::string(key) + " is required");
    }
    std::int64_t value = 0;
    const auto result = std::from_chars(source.data(), source.data() + source.size(), value);
    if (result.ec != std::errc{} || result.ptr != source.data() + source.size()) {
        row_error(row, std::string(key) + " must be an integer");
    }
    return value;
}

std::int64_t bounded(const Row& row, std::string_view key, std::int64_t value,
                     std::int64_t minimum, std::int64_t maximum) {
    if (value < minimum || value > maximum) {
        row_error(row, std::string(key) + " must be between " +
                       std::to_string(minimum) + " and " +
                       std::to_string(maximum));
    }
    return value;
}

std::uint8_t enum_value(
    const Row& row, std::string_view key,
    const std::unordered_map<std::string, std::uint8_t>& values) {
    const auto source = required_text(row, key);
    const auto found = values.find(source);
    if (found == values.end()) {
        row_error(row, std::string(key) + " has unsupported value " + source);
    }
    return found->second;
}

std::vector<std::uint32_t> id_list(const Row& row, std::string_view key) {
    const auto source = required_text(row, key);
    std::vector<std::uint32_t> result;
    std::size_t begin = 0;
    while (begin <= source.size()) {
        const auto separator = source.find('|', begin);
        const auto part = trim(std::string_view(source).substr(
            begin, separator == std::string::npos ? std::string::npos
                                                   : separator - begin));
        if (part.empty()) {
            row_error(row, std::string(key) +
                           " must contain integer ids separated by |");
        }
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() ||
            value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
            row_error(row, std::string(key) + " contains an invalid id");
        }
        result.push_back(static_cast<std::uint32_t>(value));
        if (separator == std::string::npos) break;
        begin = separator + 1;
    }
    return result;
}

template <typename T>
void insert_unique(std::map<std::uint32_t, T>& table, T value,
                   const Row& row, std::string_view table_name) {
    const auto id = value.id;
    if (!table.emplace(id, std::move(value)).second) {
        row_error(row, "duplicate id " + std::to_string(id) + " in " +
                       std::string(table_name));
    }
}

Tables parse_tables(const fs::path& directory) {
    Tables tables;
    for (const auto& row : read_rows(directory, "buffs.csv")) {
        BuffRow buff;
        buff.id = static_cast<std::uint32_t>(bounded(
            row, "id", integer(row, "id", 0, false), 1,
            std::numeric_limits<std::uint32_t>::max()));
        buff.name = required_text(row, "name");
        buff.permanent = enum_value(row, "lifetime", kLifetimes) == 1;
        buff.duration = static_cast<std::int32_t>(bounded(
            row, "duration", integer(row, "duration", buff.permanent ? 0 : 1),
            0, 10000));
        buff.decrement_on = enum_value(row, "decrement_on", kTriggers);
        buff.max_stacks = static_cast<std::int32_t>(bounded(
            row, "max_stacks", integer(row, "max_stacks", 1), 1, 1000));
        buff.stack_key = enum_value(row, "stack_key", kStackKeyPolicies);
        buff.stack_policy = enum_value(row, "stack_policy", kStackPolicies);
        buff.refresh_policy = enum_value(row, "refresh_policy", kRefreshPolicies);
        if (buff.permanent && buff.duration != 0) {
            row_error(row, "permanent buffs must use duration 0");
        }
        if (!buff.permanent && buff.duration == 0) {
            row_error(row, "finite buffs must use duration between 1 and 10000");
        }
        if (buff.stack_policy == 1 && buff.max_stacks != 1) {
            row_error(row, "refresh stack_policy requires max_stacks 1");
        }
        if (buff.name.size() > kMaxStringBytes) {
            row_error(row, "name exceeds the 1 MiB limit");
        }
        insert_unique(tables.buffs, std::move(buff), row, "buffs.csv");
    }

    for (const auto& row : read_rows(directory, "buff_modifiers.csv")) {
        ModifierRow modifier;
        modifier.buff_id = static_cast<std::uint32_t>(bounded(
            row, "buff_id", integer(row, "buff_id", 0, false), 1,
            std::numeric_limits<std::uint32_t>::max()));
        modifier.sequence = static_cast<std::uint32_t>(bounded(
            row, "sequence", integer(row, "sequence", 0, false), 1, 4096));
        modifier.attribute = enum_value(row, "attribute", kAttributes);
        modifier.operation = enum_value(row, "operation", kModifierOperations);
        modifier.value = bounded(row, "value", integer(row, "value", 0, false),
                                 -1'000'000'000'000LL, 1'000'000'000'000LL);
        if (modifier.operation == 1 &&
            (modifier.value < -1'000'000 || modifier.value > 1'000'000)) {
            row_error(row, "scale_bp modifier value must be between -1000000 and 1000000");
        }
        if (!tables.buffs.contains(modifier.buff_id)) {
            row_error(row, "buff_id references missing buff " +
                           std::to_string(modifier.buff_id));
        }
        const auto key = std::pair{modifier.buff_id, modifier.sequence};
        if (!tables.modifiers.emplace(key, std::move(modifier)).second) {
            row_error(row, "duplicate sequence for buff " +
                           std::to_string(key.first));
        }
    }

    for (const auto& row : read_rows(directory, "buff_reactions.csv")) {
        ReactionRow reaction;
        reaction.buff_id = static_cast<std::uint32_t>(bounded(
            row, "buff_id", integer(row, "buff_id", 0, false), 1,
            std::numeric_limits<std::uint32_t>::max()));
        reaction.sequence = static_cast<std::uint32_t>(bounded(
            row, "sequence", integer(row, "sequence", 0, false), 1, 4096));
        reaction.priority = static_cast<std::int32_t>(bounded(
            row, "priority", integer(row, "priority", 0), -1'000'000, 1'000'000));
        reaction.trigger = enum_value(row, "trigger", kTriggers);
        reaction.source = enum_value(row, "source", kEffectSources);
        reaction.stack_scaling =
            enum_value(row, "stack_scaling", kStackScalings);
        reaction.chance_bp = static_cast<std::int32_t>(bounded(
            row, "chance_bp", integer(row, "chance_bp", 10000), 0, 10000));
        reaction.max_triggers_per_round = static_cast<std::int32_t>(bounded(
            row, "max_triggers_per_round",
            integer(row, "max_triggers_per_round", 0), 0, 10000));
        reaction.effect_ids = id_list(row, "effect_ids");
        reaction.source_file = row.source;
        reaction.source_line = row.line;
        if (!tables.buffs.contains(reaction.buff_id)) {
            row_error(row, "buff_id references missing buff " +
                           std::to_string(reaction.buff_id));
        }
        const auto key = std::pair{reaction.buff_id, reaction.sequence};
        if (!tables.reactions.emplace(key, std::move(reaction)).second) {
            row_error(row, "duplicate sequence for buff " +
                           std::to_string(key.first));
        }
    }

    for (const auto& row : read_rows(directory, "effects.csv")) {
        EffectRow effect;
        effect.id = static_cast<std::uint32_t>(bounded(
            row, "id", integer(row, "id", 0, false), 1,
            std::numeric_limits<std::uint32_t>::max()));
        effect.kind = enum_value(row, "type", kEffectKinds);
        effect.target = enum_value(row, "target", kTargetRules);
        effect.target_count = static_cast<std::int32_t>(bounded(
            row, "target_count", integer(row, "target_count", 1), 1, 256));
        effect.attack_bp = static_cast<std::int32_t>(bounded(
            row, "attack_bp", integer(row, "attack_bp", effect.kind == 0 ? 10000 : 0),
            0, 1'000'000));
        effect.flat = bounded(row, "flat", integer(row, "flat", 0),
                              -1'000'000'000'000LL, 1'000'000'000'000LL);
        effect.buff_id = static_cast<std::uint32_t>(bounded(
            row, "buff_id", integer(row, "buff_id", 0), 0,
            std::numeric_limits<std::uint32_t>::max()));
        effect.remove_buff_id = static_cast<std::uint32_t>(bounded(
            row, "remove_buff_id", integer(row, "remove_buff_id", 0), 0,
            std::numeric_limits<std::uint32_t>::max()));
        if (effect.kind == 2) {
            if (effect.buff_id == 0 || !tables.buffs.contains(effect.buff_id)) {
                row_error(row, "add_buff requires buff_id referencing buffs.csv");
            }
        } else if (effect.buff_id != 0) {
            row_error(row, "buff_id is only valid for add_buff effects");
        }
        if (effect.kind == 3) {
            if (effect.remove_buff_id == 0 ||
                !tables.buffs.contains(effect.remove_buff_id)) {
                row_error(row,
                          "remove_buff requires remove_buff_id referencing buffs.csv");
            }
        } else if (effect.remove_buff_id != 0) {
            row_error(row,
                      "remove_buff_id is only valid for remove_buff effects");
        }
        insert_unique(tables.effects, std::move(effect), row, "effects.csv");
    }

    for (const auto& [key, reaction] : tables.reactions) {
        static_cast<void>(key);
        for (const auto effect_id : reaction.effect_ids) {
            if (!tables.effects.contains(effect_id)) {
                throw ConfigError(reaction.source_file + ":" +
                                  std::to_string(reaction.source_line) +
                                  ": effect_ids references missing effect " +
                                  std::to_string(effect_id));
            }
        }
    }

    // A reaction owns Effect values and add_buff Effects own shared BuffSpec
    // references. Reject cycles now so the immutable runtime graph cannot form
    // a shared_ptr ownership cycle.
    std::map<std::uint32_t, std::uint32_t> indegree;
    std::map<std::uint32_t, std::vector<std::uint32_t>> edges;
    std::set<std::pair<std::uint32_t, std::uint32_t>> unique_edges;
    for (const auto& [buff_id, buff] : tables.buffs) {
        static_cast<void>(buff);
        indegree.emplace(buff_id, 0);
    }
    for (const auto& [key, reaction] : tables.reactions) {
        static_cast<void>(key);
        for (const auto effect_id : reaction.effect_ids) {
            const auto& effect = tables.effects.at(effect_id);
            if (effect.kind != 2) continue;
            const auto edge = std::pair{reaction.buff_id, effect.buff_id};
            if (unique_edges.insert(edge).second) {
                edges[edge.first].push_back(edge.second);
                ++indegree.at(edge.second);
            }
        }
    }
    std::vector<std::uint32_t> ready;
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
    if (visited != tables.buffs.size()) {
        throw ConfigError(
            "buff_reactions.csv: add_buff reaction graph contains a cycle");
    }

    for (const auto& row : read_rows(directory, "skills.csv")) {
        SkillRow skill;
        skill.id = static_cast<std::uint32_t>(bounded(
            row, "id", integer(row, "id", 0, false), 1,
            std::numeric_limits<std::uint32_t>::max()));
        skill.name = required_text(row, "name");
        skill.chance_bp = static_cast<std::int32_t>(bounded(
            row, "chance_bp", integer(row, "chance_bp", 10000), 0, 10000));
        skill.priority = static_cast<std::int32_t>(bounded(
            row, "priority", integer(row, "priority", 0),
            std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()));
        skill.effect_ids = id_list(row, "effect_ids");
        for (const auto id : skill.effect_ids) {
            if (!tables.effects.contains(id)) {
                row_error(row, "effect_ids references missing effect " +
                               std::to_string(id));
            }
        }
        if (skill.name.size() > kMaxStringBytes) {
            row_error(row, "name exceeds the 1 MiB limit");
        }
        insert_unique(tables.skills, std::move(skill), row, "skills.csv");
    }

    for (const auto& row : read_rows(directory, "passives.csv")) {
        PassiveRow passive;
        passive.id = static_cast<std::uint32_t>(bounded(
            row, "id", integer(row, "id", 0, false), 1,
            std::numeric_limits<std::uint32_t>::max()));
        passive.name = required_text(row, "name");
        passive.trigger = enum_value(row, "trigger", kTriggers);
        passive.priority = static_cast<std::int32_t>(bounded(
            row, "priority", integer(row, "priority", 0), -1'000'000, 1'000'000));
        passive.chance_bp = static_cast<std::int32_t>(bounded(
            row, "chance_bp", integer(row, "chance_bp", 10000), 0, 10000));
        passive.max_triggers_per_round = static_cast<std::int32_t>(bounded(
            row, "max_triggers_per_round",
            integer(row, "max_triggers_per_round", 0), 0, 10000));
        passive.effect_ids = id_list(row, "effect_ids");
        for (const auto id : passive.effect_ids) {
            if (!tables.effects.contains(id)) {
                row_error(row, "effect_ids references missing effect " +
                               std::to_string(id));
            }
        }
        if (passive.name.size() > kMaxStringBytes) {
            row_error(row, "name exceeds the 1 MiB limit");
        }
        insert_unique(tables.passives, std::move(passive), row, "passives.csv");
    }
    return tables;
}

void append_u8(std::vector<std::uint8_t>& output, std::uint8_t value) {
    output.push_back(value);
}

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    for (std::size_t index = 0; index < 2; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        output.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void append_i32(std::vector<std::uint8_t>& output, std::int32_t value) {
    append_u32(output, std::bit_cast<std::uint32_t>(value));
}

void append_i64(std::vector<std::uint8_t>& output, std::int64_t value) {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    for (std::size_t index = 0; index < 8; ++index) {
        output.push_back(static_cast<std::uint8_t>(bits >> (index * 8U)));
    }
}

void append_string(std::vector<std::uint8_t>& output, const std::string& value) {
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

void append_ids(std::vector<std::uint8_t>& output,
                std::span<const std::uint32_t> values) {
    append_u32(output, static_cast<std::uint32_t>(values.size()));
    for (const auto value : values) append_u32(output, value);
}

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

std::vector<std::uint8_t> build_pack(const Tables& tables) {
    std::vector<std::uint8_t> payload;
    append_u32(payload, static_cast<std::uint32_t>(tables.buffs.size()));
    append_u32(payload, static_cast<std::uint32_t>(tables.modifiers.size()));
    append_u32(payload, static_cast<std::uint32_t>(tables.reactions.size()));
    append_u32(payload, static_cast<std::uint32_t>(tables.effects.size()));
    append_u32(payload, static_cast<std::uint32_t>(tables.skills.size()));
    append_u32(payload, static_cast<std::uint32_t>(tables.passives.size()));

    for (const auto& [id, buff] : tables.buffs) {
        append_u32(payload, id);
        append_string(payload, buff.name);
        append_u8(payload, buff.permanent ? std::uint8_t{1} : std::uint8_t{0});
        append_i32(payload, buff.duration);
        append_u8(payload, buff.decrement_on);
        append_i32(payload, buff.max_stacks);
        append_u8(payload, buff.stack_key);
        append_u8(payload, buff.stack_policy);
        append_u8(payload, buff.refresh_policy);
    }
    for (const auto& [key, modifier] : tables.modifiers) {
        static_cast<void>(key);
        append_u32(payload, modifier.buff_id);
        append_u32(payload, modifier.sequence);
        append_u8(payload, modifier.attribute);
        append_u8(payload, modifier.operation);
        append_i64(payload, modifier.value);
    }
    for (const auto& [key, reaction] : tables.reactions) {
        static_cast<void>(key);
        append_u32(payload, reaction.buff_id);
        append_u32(payload, reaction.sequence);
        append_i32(payload, reaction.priority);
        append_u8(payload, reaction.trigger);
        append_u8(payload, reaction.source);
        append_u8(payload, reaction.stack_scaling);
        append_i32(payload, reaction.chance_bp);
        append_i32(payload, reaction.max_triggers_per_round);
        append_ids(payload, reaction.effect_ids);
    }
    for (const auto& [id, effect] : tables.effects) {
        append_u32(payload, id);
        append_u8(payload, effect.kind);
        append_u8(payload, effect.target);
        append_i32(payload, effect.target_count);
        append_i32(payload, effect.attack_bp);
        append_i64(payload, effect.flat);
        append_u32(payload, effect.buff_id);
        append_u32(payload, effect.remove_buff_id);
    }
    for (const auto& [id, skill] : tables.skills) {
        append_u32(payload, id);
        append_string(payload, skill.name);
        append_i32(payload, skill.chance_bp);
        append_i32(payload, skill.priority);
        append_ids(payload, skill.effect_ids);
    }
    for (const auto& [id, passive] : tables.passives) {
        append_u32(payload, id);
        append_string(payload, passive.name);
        append_u8(payload, passive.trigger);
        append_i32(payload, passive.priority);
        append_i32(payload, passive.chance_bp);
        append_i32(payload, passive.max_triggers_per_round);
        append_ids(payload, passive.effect_ids);
    }

    std::vector<std::uint8_t> pack;
    pack.insert(pack.end(), {'G', 'B', 'C', 'F'});
    append_u16(pack, kFormatMajor);
    append_u16(pack, kFormatMinor);
    append_u32(pack, static_cast<std::uint32_t>(payload.size()));
    append_u32(pack, crc32(payload));
    pack.insert(pack.end(), payload.begin(), payload.end());
    return pack;
}

void replace_file(const fs::path& temporary, const fs::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw ConfigError("cannot replace output file; Windows error " +
                          std::to_string(GetLastError()));
    }
#else
    std::error_code error;
    fs::rename(temporary, destination, error);
    if (error) {
        throw ConfigError("cannot replace output file: " + error.message());
    }
#endif
}

void write_pack(const fs::path& output, std::span<const std::uint8_t> bytes) {
    if (!output.parent_path().empty()) {
        std::error_code directory_error;
        fs::create_directories(output.parent_path(), directory_error);
        if (directory_error) {
            throw ConfigError("cannot create output directory: " +
                              directory_error.message());
        }
    }
    auto temporary = output;
    temporary += ".tmp";
    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            throw ConfigError("cannot open temporary output: " + path_text(temporary));
        }
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!stream) {
            throw ConfigError("failed while writing output: " + path_text(temporary));
        }
        replace_file(temporary, output);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

struct Arguments {
    fs::path input_directory;
    fs::path output;
    bool check_only{false};
    bool help{false};
};

Arguments parse_arguments(std::span<const fs::path> args) {
    Arguments result;
    for (std::size_t index = 1; index < args.size(); ++index) {
        const auto option = args[index].generic_string();
        if (option == "--help" || option == "-h") {
            result.help = true;
        } else if (option == "--check-only") {
            result.check_only = true;
        } else if (option == "--input-dir" || option == "--output") {
            if (index + 1 >= args.size()) {
                throw ConfigError(option + " requires a value");
            }
            if (option == "--input-dir") result.input_directory = args[++index];
            else result.output = args[++index];
        } else {
            throw ConfigError("unknown argument: " + option);
        }
    }
    if (!result.help && result.input_directory.empty()) {
        throw ConfigError("--input-dir is required");
    }
    if (!result.help && !result.check_only && result.output.empty()) {
        throw ConfigError("--output is required unless --check-only is used");
    }
    return result;
}

int run(std::span<const fs::path> args) {
    try {
        const auto options = parse_arguments(args);
        if (options.help) {
            std::cout
                << "Usage: gamebattle_config_compiler --input-dir DIR "
                   "[--output FILE | --check-only]\n";
            return 0;
        }
        const auto tables = parse_tables(options.input_directory);
        if (options.check_only) {
            std::cout << "configuration is valid\n";
            return 0;
        }
        const auto pack = build_pack(tables);
        const auto output = fs::absolute(options.output);
        write_pack(output, pack);
        std::cout << "wrote " << path_text(output) << " (" << pack.size()
                  << " bytes): " << tables.buffs.size() << " buffs, "
                  << tables.modifiers.size() << " modifiers, "
                  << tables.reactions.size() << " reactions, "
                  << tables.effects.size() << " effects, "
                  << tables.skills.size() << " skills, "
                  << tables.passives.size() << " passives\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "config error: " << error.what() << '\n';
        return 2;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    std::vector<fs::path> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) arguments.emplace_back(argv[index]);
    return run(arguments);
}
#else
int main(int argc, char* argv[]) {
    std::vector<fs::path> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) arguments.emplace_back(argv[index]);
    return run(arguments);
}
#endif
