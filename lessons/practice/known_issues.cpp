// Lesson 22: reproduces the engine issues found while building this course and
// reports whether each one is still present. Run it again after fixing them.
//
//   known_issues

#include "request_codec.hpp"
#include "sample_battle.hpp"

#include "gamebattle/engine.hpp"
#include "gamebattle/term.hpp"
#include "gamebattle/wire.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string reply_summary(const std::vector<std::uint8_t>& reply) {
    using gamebattle::term::Value;
    const auto value = gamebattle::term::decode(reply);
    const auto& tuple = std::get<Value::TupleValue>(value.data).value;
    const auto status = gamebattle::term::as_string(tuple[0], "status");
    if (status == "ok") return "ok";
    const auto* type = gamebattle::term::find(tuple[1], "type");
    const auto* message = gamebattle::term::find(tuple[1], "message");
    return "error " + (type ? gamebattle::term::as_string(*type, "type") : std::string("?")) + ": " +
           (message ? gamebattle::term::as_string(*message, "message") : std::string("?"));
}

long vm_peak_kb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmPeak:", 0) == 0) return std::stol(line.substr(7));
    }
    return -1;
}

// Issue 1: two units carrying the same inline buff are rejected over ETF,
// although the identical in-process request is accepted.
bool inline_buff_conflict() {
    auto request = practice::sample_battle(1, 42);
    request.attacker.units[1] = request.attacker.units[0];   // same passives, same buff id 801
    request.attacker.units[1].id = 1002;
    request.attacker.units[1].position = 2;

    std::string direct = "ok";
    try {
        static_cast<void>(gamebattle::Engine{}.simulate(request));
    } catch (const std::exception& error) {
        direct = std::string("error: ") + error.what();
    }
    const auto over_etf = reply_summary(gamebattle::wire::handle_etf(
        gamebattle::term::encode(practice::encode_request(request))));

    std::cout << "[1] same inline buff on two units\n"
              << "    Engine::simulate (in process): " << direct << "\n"
              << "    wire::handle_etf (over ETF)  : " << over_etf << "\n";
    return direct == "ok" && over_etf != "ok";
}

// Issue 2: a tiny input with nested containers claiming 1,000,000 elements each
// makes the decoder reserve gigabytes before noticing the input is truncated.
bool nested_reserve_amplification() {
    std::vector<std::uint8_t> input{131};
    for (int level = 0; level < 100; ++level) {
        input.push_back(108);                                // LIST_EXT
        input.insert(input.end(), {0x00, 0x0F, 0x42, 0x40}); // count = 1,000,000
    }
    const long before = vm_peak_kb();
    const auto summary = reply_summary(gamebattle::wire::handle_etf(input));
    const long after = vm_peak_kb();
    std::cout << "[2] " << input.size() << "-byte input of nested lists\n"
              << "    reply: " << summary << "\n"
              << "    VmPeak grew by " << (after - before) / 1024 << " MB\n";
    return after - before > 512 * 1024;
}

} // namespace

int main() {
    const bool issue1 = inline_buff_conflict();
    const bool issue2 = nested_reserve_amplification();
    std::cout << "\nissue 1 (inline buff conflict)        : " << (issue1 ? "PRESENT" : "fixed") << '\n'
              << "issue 2 (nested reserve amplification): " << (issue2 ? "PRESENT" : "fixed") << '\n'
              << "issue 3 (slow result encoding)        : run encode_bench to compare\n";
    return 0;
}
