#include "gamebattle/config_store.hpp"

#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace {

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

std::vector<std::uint8_t> make_cycle_pack() {
    constexpr std::uint32_t buff_id = 801;
    constexpr std::uint32_t effect_id = 9001;

    std::vector<std::uint8_t> payload;
    append_u32(payload, 1); // buffs
    append_u32(payload, 0); // modifiers
    append_u32(payload, 1); // reactions
    append_u32(payload, 1); // effects
    append_u32(payload, 0); // skills
    append_u32(payload, 0); // passives

    append_u32(payload, buff_id);
    append_string(payload, "cycle");
    append_u8(payload, 0);  // finite
    append_i32(payload, 1); // duration
    append_u8(payload, static_cast<std::uint8_t>(gamebattle::Trigger::round_end));
    append_i32(payload, 1); // max stacks
    append_u8(payload, static_cast<std::uint8_t>(gamebattle::StackKeyPolicy::by_buff));
    append_u8(payload, static_cast<std::uint8_t>(gamebattle::StackPolicy::refresh));
    append_u8(payload, static_cast<std::uint8_t>(gamebattle::RefreshPolicy::reset));

    append_u32(payload, buff_id);
    append_u32(payload, 1); // reaction sequence
    append_i32(payload, 0); // reaction priority
    append_u8(payload, static_cast<std::uint8_t>(gamebattle::Trigger::round_end));
    append_u8(payload, static_cast<std::uint8_t>(gamebattle::EffectSource::owner));
    append_u8(payload, static_cast<std::uint8_t>(gamebattle::StackScaling::once));
    append_i32(payload, 10000);
    append_i32(payload, 0);
    append_u32(payload, 1); // effect id count
    append_u32(payload, effect_id);

    append_u32(payload, effect_id);
    append_u8(payload, static_cast<std::uint8_t>(gamebattle::EffectKind::add_buff));
    append_u8(payload, static_cast<std::uint8_t>(gamebattle::TargetRule::self));
    append_i32(payload, 1);
    append_i32(payload, 0);
    append_i64(payload, 0);
    append_u32(payload, buff_id);
    append_u32(payload, 0);

    std::vector<std::uint8_t> pack;
    pack.insert(pack.end(), {'G', 'B', 'C', 'F'});
    append_u16(pack, gamebattle::ConfigStore::format_major);
    append_u16(pack, gamebattle::ConfigStore::format_minor);
    append_u32(pack, static_cast<std::uint32_t>(payload.size()));
    append_u32(pack, crc32(payload));
    pack.insert(pack.end(), payload.begin(), payload.end());
    return pack;
}

} // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      "gamebattle-invalid-buff-cycle-v3.gbcfg";
    const auto bytes = make_cycle_pack();
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::cerr << "cannot create temporary cycle config pack\n";
            return 2;
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            std::cerr << "cannot write temporary cycle config pack\n";
            return 2;
        }
    }

    try {
        static_cast<void>(gamebattle::ConfigStore::load_file(path));
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        const std::string message = error.what();
        if (message.find("ownership cycle") != std::string::npos) {
            std::cout << "loader rejected buff reaction ownership cycle\n";
            return 0;
        }
        std::cerr << "loader rejected the pack for the wrong reason: "
                  << message << '\n';
        return 3;
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::cerr << "loader accepted a buff reaction ownership cycle\n";
    return 1;
}
