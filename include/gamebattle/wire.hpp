#pragma once

#include "gamebattle/config_store.hpp"
#include "gamebattle/engine.hpp"
#include "gamebattle/term.hpp"

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <span>
#include <vector>

namespace gamebattle::wire {

BattleRequest parse_request(const term::Value& value,
                            const ConfigStore* configs = nullptr);
term::Value encode_result(const BattleResult& result);

// Thread-safe process-local endpoint. A successful {load_config, Path} swaps an
// immutable store atomically from the perspective of new requests; simulations
// already parsing a request keep their previous shared snapshot.
class Handler final {
public:
    std::vector<std::uint8_t> handle_etf(
        std::span<const std::uint8_t> request);

private:
    mutable std::shared_mutex config_mutex_;
    std::shared_ptr<const ConfigStore> configs_;
};

// Uses one process-global Handler. Accepts a complete ETF value (including
// version byte 131) and returns ETF {ok, Value} or {error, ErrorMap}.
std::vector<std::uint8_t> handle_etf(std::span<const std::uint8_t> request);

} // namespace gamebattle::wire
