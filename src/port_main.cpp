#include "gamebattle/wire.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr std::uint32_t kMaxPacketBytes = 64U * 1024U * 1024U;

bool read_exact(std::span<std::uint8_t> destination) {
    std::cin.read(reinterpret_cast<char*>(destination.data()),
                  static_cast<std::streamsize>(destination.size()));
    if (std::cin.gcount() == 0 && std::cin.eof()) {
        return false;
    }
    return std::cin.gcount() == static_cast<std::streamsize>(destination.size());
}

bool write_packet(std::span<const std::uint8_t> packet) {
    const auto length = static_cast<std::uint32_t>(packet.size());
    const std::array<std::uint8_t, 4> header{
        static_cast<std::uint8_t>(length >> 24U),
        static_cast<std::uint8_t>(length >> 16U),
        static_cast<std::uint8_t>(length >> 8U),
        static_cast<std::uint8_t>(length)
    };
    std::cout.write(reinterpret_cast<const char*>(header.data()), header.size());
    std::cout.write(reinterpret_cast<const char*>(packet.data()),
                    static_cast<std::streamsize>(packet.size()));
    std::cout.flush();
    return std::cout.good();
}

} // namespace

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    while (true) {
        std::array<std::uint8_t, 4> header{};
        if (!read_exact(header)) {
            return std::cin.eof() ? 0 : 2;
        }
        const auto length = (static_cast<std::uint32_t>(header[0]) << 24U) |
                            (static_cast<std::uint32_t>(header[1]) << 16U) |
                            (static_cast<std::uint32_t>(header[2]) << 8U) |
                            static_cast<std::uint32_t>(header[3]);
        if (length == 0 || length > kMaxPacketBytes) {
            std::cerr << "invalid port packet length: " << length << '\n';
            return 3;
        }
        std::vector<std::uint8_t> request(length);
        if (!read_exact(request)) {
            return 4;
        }
        const auto response = gamebattle::wire::handle_etf(request);
        if (!write_packet(response)) {
            return 5;
        }
    }
}
