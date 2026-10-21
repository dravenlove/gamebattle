#include "gamebattle/config_check.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace gamebattle::config_check {
namespace {

constexpr std::size_t kTriggerCount = static_cast<std::size_t>(Trigger::ally_activate) + 1;
// validate_request's limits for one unit.
constexpr std::size_t kMaxPerUnit = 128;

std::string_view trigger_name(Trigger trigger) {
    switch (trigger) {
    case Trigger::battle_start: return "battle_start";
    case Trigger::round_start: return "round_start";
    case Trigger::before_action: return "before_action";
    case Trigger::on_attack: return "on_attack";
    case Trigger::on_hit: return "on_hit";
    case Trigger::on_damaged: return "on_damaged";
    case Trigger::unit_death: return "unit_death";
    case Trigger::after_action: return "after_action";
    case Trigger::round_end: return "round_end";
    case Trigger::enemy_activate: return "enemy_activate";
    case Trigger::ally_activate: return "ally_activate";
    }
    return "unknown";
}

constexpr std::uint32_t bit(Trigger trigger) {
    return std::uint32_t{1} << static_cast<unsigned>(trigger);
}

std::string counted(std::size_t count, const std::string& noun) {
    return std::to_string(count) + " " + noun + (count == 1 ? "" : "s");
}

std::string with_commas(std::uint64_t value) {
    auto digits = std::to_string(value);
    for (auto index = digits.size(); index > 3; index -= 3) {
        digits.insert(index - 3, ",");
    }
    return digits;
}

// A passive or a buff reaction: something a trigger sets off.
struct Node {
    std::string name;
    Trigger trigger{Trigger::on_damaged};
    std::int32_t cap{0};
    // Triggers its effects set off (EffectSystem::apply_damage): damage sets
    // off on_hit for its source, on_damaged for the target and unit_death on
    // a kill; direct damage all but on_hit. Nothing else sets off a trigger.
    std::uint32_t emits{0};
    bool direct_only{false};

    // A unit dies once, so unit_death cannot repeat within a battle.
    bool limited() const { return cap > 0 || trigger == Trigger::unit_death; }
};

std::string describe(const Node& node) {
    std::string limit;
    if (node.cap > 0) {
        limit = std::to_string(node.cap) + "/round";
    } else if (node.trigger == Trigger::unit_death) {
        limit = "once per death";
    } else {
        limit = "no limit";
    }
    return node.name + " (" + std::string(trigger_name(node.trigger)) + ", " + limit + ")";
}

Node make_node(std::string name, Trigger trigger, std::int32_t cap,
               const std::vector<Effect>& effects) {
    Node node{.name = std::move(name), .trigger = trigger, .cap = cap};
    bool damage = false;
    for (const auto& effect : effects) {
        if (effect.kind == EffectKind::damage) {
            node.emits |= bit(Trigger::on_hit) | bit(Trigger::on_damaged) |
                          bit(Trigger::unit_death);
            damage = true;
        } else if (effect.kind == EffectKind::direct_damage) {
            node.emits |= bit(Trigger::on_damaged) | bit(Trigger::unit_death);
        }
    }
    node.direct_only = node.emits != 0 && !damage;
    return node;
}

void collect_buffs(const std::vector<Effect>& effects,
                   std::map<std::uint32_t, std::shared_ptr<const BuffSpec>>& buffs);

void add_buff(const std::shared_ptr<const BuffSpec>& buff,
              std::map<std::uint32_t, std::shared_ptr<const BuffSpec>>& buffs) {
    if (buff != nullptr && buffs.emplace(buff->id, buff).second) {
        for (const auto& reaction : buff->reactions) {
            collect_buffs(reaction.effects, buffs);
        }
    }
}

void collect_buffs(const std::vector<Effect>& effects,
                   std::map<std::uint32_t, std::shared_ptr<const BuffSpec>>& buffs) {
    for (const auto& effect : effects) {
        if (effect.kind == EffectKind::add_buff) {
            add_buff(effect.buff, buffs);
        }
    }
}

struct Edge {
    std::size_t to;
    Trigger via;
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<std::vector<Edge>> edges;
};

Graph build_graph(const Definitions& definitions) {
    std::map<std::uint32_t, std::shared_ptr<const BuffSpec>> buffs;
    for (const auto& buff : definitions.buffs) {
        add_buff(buff, buffs);
    }
    for (const auto& skill : definitions.skills) {
        collect_buffs(skill.effects, buffs);
    }
    for (const auto& passive : definitions.passives) {
        collect_buffs(passive.effects, buffs);
    }

    Graph graph;
    for (const auto& passive : definitions.passives) {
        if (passive.chance_bp > 0) {
            graph.nodes.push_back(make_node(
                "passive " + std::to_string(passive.id) + " \"" + passive.name + "\"",
                passive.trigger, passive.max_triggers_per_round, passive.effects));
        }
    }
    for (const auto& [id, buff] : buffs) {
        for (std::size_t index = 0; index < buff->reactions.size(); ++index) {
            const auto& reaction = buff->reactions[index];
            if (reaction.chance_bp > 0) {
                graph.nodes.push_back(make_node(
                    "buff " + std::to_string(id) + " \"" + buff->name + "\" reaction " +
                        std::to_string(index + 1),
                    reaction.trigger, reaction.max_triggers_per_round, reaction.effects));
            }
        }
    }

    std::array<std::vector<std::size_t>, kTriggerCount> listeners;
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        listeners[static_cast<std::size_t>(graph.nodes[index].trigger)].push_back(index);
    }
    graph.edges.resize(graph.nodes.size());
    for (std::size_t from = 0; from < graph.nodes.size(); ++from) {
        for (const auto via : {Trigger::on_hit, Trigger::on_damaged, Trigger::unit_death}) {
            if ((graph.nodes[from].emits & bit(via)) == 0) {
                continue;
            }
            for (const auto to : listeners[static_cast<std::size_t>(via)]) {
                graph.edges[from].push_back(Edge{to, via});
            }
        }
    }
    return graph;
}

// Strongly connected components of the nodes `included` marks that contain a
// cycle (more than one node, or one node with an edge to itself), in the
// order of their first node.
std::vector<std::vector<std::size_t>> loops(const Graph& graph,
                                            const std::vector<bool>& included) {
    const auto count = graph.nodes.size();
    std::vector<std::size_t> index(count, SIZE_MAX);
    std::vector<std::size_t> low(count, 0);
    std::vector<bool> on_stack(count, false);
    std::vector<std::size_t> stack;
    std::vector<std::vector<std::size_t>> result;
    std::size_t next = 0;

    // Iterative Tarjan: frames of (node, next edge to look at).
    std::vector<std::pair<std::size_t, std::size_t>> frames;
    for (std::size_t root = 0; root < count; ++root) {
        if (!included[root] || index[root] != SIZE_MAX) {
            continue;
        }
        frames.emplace_back(root, 0);
        index[root] = low[root] = next++;
        stack.push_back(root);
        on_stack[root] = true;
        while (!frames.empty()) {
            auto& [node, edge] = frames.back();
            if (edge < graph.edges[node].size()) {
                const auto to = graph.edges[node][edge++].to;
                if (!included[to]) {
                    continue;
                }
                if (index[to] == SIZE_MAX) {
                    index[to] = low[to] = next++;
                    stack.push_back(to);
                    on_stack[to] = true;
                    frames.emplace_back(to, 0);
                } else if (on_stack[to]) {
                    low[node] = std::min(low[node], index[to]);
                }
                continue;
            }
            const auto finished = node;
            frames.pop_back();
            if (!frames.empty()) {
                const auto parent = frames.back().first;
                low[parent] = std::min(low[parent], low[finished]);
            }
            if (low[finished] != index[finished]) {
                continue;
            }
            std::vector<std::size_t> component;
            std::size_t member = SIZE_MAX;
            while (member != finished) {
                member = stack.back();
                stack.pop_back();
                on_stack[member] = false;
                component.push_back(member);
            }
            const bool cycle =
                component.size() > 1 ||
                std::any_of(graph.edges[finished].begin(), graph.edges[finished].end(),
                            [&](const Edge& e) { return e.to == finished; });
            if (cycle) {
                std::sort(component.begin(), component.end());
                result.push_back(std::move(component));
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

// The shortest cycle through `start` within `members`, as the edges taken.
std::vector<std::pair<std::size_t, Edge>> shortest_cycle(
    const Graph& graph, const std::vector<std::size_t>& members, std::size_t start) {
    std::vector<bool> member(graph.nodes.size(), false);
    for (const auto node : members) {
        member[node] = true;
    }
    std::vector<std::optional<std::pair<std::size_t, Edge>>> came_from(graph.nodes.size());
    std::deque<std::size_t> queue{start};
    std::optional<std::pair<std::size_t, Edge>> closing;
    while (!queue.empty() && !closing) {
        const auto node = queue.front();
        queue.pop_front();
        for (const auto& edge : graph.edges[node]) {
            if (!member[edge.to]) {
                continue;
            }
            if (edge.to == start) {
                closing = std::make_pair(node, edge);
                break;
            }
            if (!came_from[edge.to]) {
                came_from[edge.to] = std::make_pair(node, edge);
                queue.push_back(edge.to);
            }
        }
    }
    std::vector<std::pair<std::size_t, Edge>> path;
    if (!closing) {
        return path;
    }
    path.push_back(*closing);
    for (auto node = closing->first; node != start; node = came_from[node]->first) {
        path.push_back(*came_from[node]);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

std::string effect_words(const Node& node, Trigger via) {
    if (via == Trigger::unit_death) {
        return "kills";
    }
    if (via == Trigger::on_damaged && node.direct_only) {
        return "deals direct damage";
    }
    return "deals damage";
}

std::string describe_cycle(const Graph& graph,
                           const std::vector<std::pair<std::size_t, Edge>>& path) {
    std::string text;
    for (const auto& [from, edge] : path) {
        text += "\n    " + describe(graph.nodes[from]) + " " +
                effect_words(graph.nodes[from], edge.via) + ", which sets off " +
                std::string(trigger_name(edge.via));
    }
    text += "\n    -> " + describe(graph.nodes[path.front().first]) + " again";
    return text;
}

std::string list_names(const Graph& graph, const std::vector<std::size_t>& nodes,
                       std::size_t shown) {
    std::string text;
    for (std::size_t index = 0; index < nodes.size() && index < shown; ++index) {
        text += (index == 0 ? "" : ", ") + graph.nodes[nodes[index]].name;
    }
    if (nodes.size() > shown) {
        text += " and " + std::to_string(nodes.size() - shown) + " more";
    }
    return text;
}

void check_loops(const Graph& graph, Report& report) {
    std::vector<bool> unlimited(graph.nodes.size());
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        unlimited[index] = !graph.nodes[index].limited();
    }
    std::vector<bool> in_error(graph.nodes.size(), false);
    for (const auto& component : loops(graph, unlimited)) {
        const auto path = shortest_cycle(graph, component, component.front());
        std::string message =
            "runaway loop: nothing in it has a max_triggers_per_round, so the "
            "triggers keep setting each other off until the engine cuts the "
            "cascade at its trigger depth limit, or ends the battle at max_events:" +
            describe_cycle(graph, path);
        if (component.size() > path.size()) {
            message += "\n  All of these can set one another off: " +
                       list_names(graph, component, 12) + ".";
        }
        message += "\n  Give at least one of them a max_triggers_per_round.";
        report.findings.push_back({Severity::error, std::move(message)});
        for (const auto node : component) {
            in_error[node] = true;
        }
    }

    const std::vector<bool> everything(graph.nodes.size(), true);
    for (const auto& component : loops(graph, everything)) {
        if (std::any_of(component.begin(), component.end(),
                        [&](std::size_t node) { return in_error[node]; })) {
            continue;
        }
        std::vector<std::size_t> limiting;
        for (const auto node : component) {
            if (graph.nodes[node].limited()) {
                limiting.push_back(node);
            }
        }
        std::string message;
        if (component.size() == 1) {
            message = describe(graph.nodes[component.front()]) +
                      " can set itself off again; its limit ends the loop.";
        } else {
            message = std::to_string(component.size()) +
                      " passives and buff reactions can set each other off: " +
                      list_names(graph, component, 8) + ".";
            if (limiting.size() == component.size()) {
                std::int64_t total = 0;
                bool deaths = false;
                for (const auto node : component) {
                    total += graph.nodes[node].cap;
                    deaths = deaths || graph.nodes[node].cap == 0;
                }
                message += " All of them are limited, so each unit fires them at most " +
                           with_commas(static_cast<std::uint64_t>(total)) +
                           " times a round in total" + (deaths ? ", plus once per death." : ".");
            } else {
                message += " The loop ends when " + list_names(graph, limiting, 8) +
                           " reach their limits.";
            }
        }
        report.findings.push_back({Severity::note, std::move(message)});
    }
}

// The biggest step of a battle, steps as in report::encode: one unit's
// action, or a run of triggers with the same round and phase.
struct Step {
    std::uint32_t events{0};
    std::int32_t round{0};
    std::string phase;
    UnitId actor{0};
    std::vector<std::pair<std::string, std::uint32_t>> top;
};

Step largest_step(const std::vector<Event>& events) {
    Step best;
    Step current;
    bool open = false;
    bool action = false;
    std::unordered_map<std::uint64_t, std::uint32_t> sources;
    const auto close = [&] {
        if (open && current.events > best.events) {
            std::vector<std::pair<std::uint64_t, std::uint32_t>> counts(sources.begin(),
                                                                        sources.end());
            std::sort(counts.begin(), counts.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second > b.second : a.first < b.first;
            });
            current.top.clear();
            for (std::size_t index = 0; index < counts.size() && index < 5; ++index) {
                const auto id = static_cast<std::uint32_t>(counts[index].first);
                current.top.emplace_back(
                    (counts[index].first >> 32) == 1
                        ? "passive " + std::to_string(id)
                        : "buff " + std::to_string(id) + " reactions",
                    counts[index].second);
            }
            best = current;
        }
        open = false;
    };
    for (const auto& event : events) {
        const bool starts = event.type == "action_start";
        if (starts || !open ||
            (!action && (event.round != current.round || event.phase != current.phase))) {
            close();
            open = true;
            action = starts;
            current = Step{.round = event.round, .phase = event.phase,
                           .actor = starts ? event.actor : 0, .top = {}};
            sources.clear();
        }
        ++current.events;
        if (event.type == "passive") {
            ++sources[(std::uint64_t{1} << 32) | event.source_id];
        } else if (event.type == "buff_reaction") {
            ++sources[(std::uint64_t{2} << 32) | event.source_id];
        }
        if (action && event.type == "action_end") {
            close();
        }
    }
    close();
    return best;
}

std::string describe_step(const Step& step) {
    std::string text = with_commas(step.events) + " events in round " +
                       std::to_string(step.round) + ", ";
    text += step.actor != 0 ? "during unit " + std::to_string(step.actor) + "'s action"
                            : "in the " + step.phase + " triggers";
    if (!step.top.empty()) {
        text += "; most set off:";
        for (std::size_t index = 0; index < step.top.size(); ++index) {
            text += (index == 0 ? " " : ", ") + step.top[index].first + " x" +
                    with_commas(step.top[index].second);
        }
    }
    return text;
}

// Every unit carries every passive (in groups of 128, the most one unit may
// have) and the first 128 skills, and nobody dies within the rounds played.
BattleRequest stress_request(const Definitions& definitions, const Options& options,
                             std::uint64_t seed) {
    BattleRequest request;
    request.battle_id = seed;
    request.seed = seed;
    request.max_rounds = std::clamp(options.rounds, 1, 10000);
    request.max_events = std::clamp(options.max_events, 100, 1000000);
    const auto per_side = static_cast<std::size_t>(std::clamp(options.units_per_side, 1, 256));
    const auto groups = std::max<std::size_t>(
        1, (definitions.passives.size() + kMaxPerUnit - 1) / kMaxPerUnit);
    std::size_t made = 0;
    for (auto* formation : {&request.attacker, &request.defender}) {
        formation->name = "stress";
        for (std::size_t position = 1; position <= per_side; ++position, ++made) {
            UnitConfig unit;
            unit.id = (formation == &request.attacker ? 1000 : 2000) + position;
            unit.position = static_cast<std::int32_t>(position);
            unit.final_stats.hp = 1'000'000'000'000LL;
            unit.final_stats.attack = 1000;
            unit.final_stats.defense = 100;
            unit.final_stats.speed = static_cast<std::int64_t>(100 + made * 3);
            unit.final_stats.crit_rate_bp = 2000;
            const auto group = made % groups;
            for (std::size_t index = group * kMaxPerUnit;
                 index < definitions.passives.size() && index < (group + 1) * kMaxPerUnit;
                 ++index) {
                unit.passives.push_back(definitions.passives[index]);
            }
            for (std::size_t index = 0;
                 index < definitions.skills.size() && index < kMaxPerUnit; ++index) {
                unit.skills.push_back(definitions.skills[index]);
            }
            formation->units.push_back(std::move(unit));
        }
    }
    return request;
}

void stress(const Definitions& definitions, const Options& options, bool loops_found,
            Report& report) {
    const auto battles = std::max(1, options.battles);
    const auto per_side = std::clamp(options.units_per_side, 1, 256);
    std::string setup = std::to_string(per_side) + "v" + std::to_string(per_side) + ", ";
    if (definitions.passives.size() <= kMaxPerUnit) {
        setup += "every unit with all " + counted(definitions.passives.size(), "passive");
    } else {
        setup += "the " + counted(definitions.passives.size(), "passive") +
                 " spread over the units in groups of 128";
    }
    setup += " and " + counted(std::min(definitions.skills.size(), kMaxPerUnit), "skill") +
             ", nobody dies, " + counted(static_cast<std::size_t>(options.rounds), "round");

    Step largest;
    std::uint64_t most_events = 0;
    std::optional<std::int32_t> limit_round;
    double milliseconds = 0;
    for (int battle = 0; battle < battles; ++battle) {
        const auto request =
            stress_request(definitions, options, 1 + static_cast<std::uint64_t>(battle) * 7919);
        BattleResult result;
        const auto started = std::chrono::steady_clock::now();
        try {
            result = Engine{}.simulate(request);
        } catch (const std::exception& error) {
            report.findings.push_back(
                {Severity::warning, "the stress battle could not run: " + std::string(error.what())});
            return;
        }
        milliseconds += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started)
                            .count();
        most_events = std::max<std::uint64_t>(most_events, result.events.size());
        auto step = largest_step(result.events);
        if (step.events > largest.events) {
            largest = std::move(step);
        }
        if (result.reason == "event_limit" && !limit_round) {
            limit_round = result.rounds;
        }
    }

    if (limit_round) {
        report.findings.push_back(
            {loops_found ? Severity::error : Severity::warning,
             "stress battle (" + setup + ") ran out of events: it reached max_events (" +
                 with_commas(static_cast<std::uint64_t>(options.max_events)) +
                 ") in round " + std::to_string(*limit_round) + ". Largest step: " +
                 describe_step(largest) + "."});
    } else if (largest.events > options.step_warning) {
        report.findings.push_back(
            {Severity::warning,
             "a single step of the stress battle (" + setup + ") set off " +
                 describe_step(largest) +
                 ". Check the max_triggers_per_round of these passives."});
    } else {
        const auto per_battle = milliseconds / battles;
        auto time = std::to_string(per_battle < 10 ? std::lround(per_battle * 10)
                                                   : std::lround(per_battle));
        if (per_battle < 10) {
            time.insert(time.size() - 1, time.size() == 1 ? "0." : ".");
        }
        report.findings.push_back(
            {Severity::note,
             "stress battle (" + setup + "): up to " + with_commas(most_events) +
                 " events a battle, largest step " + describe_step(largest) + "; " + time +
                 " ms a battle."});
    }
}

} // namespace

Definitions definitions(const ConfigStore& store) {
    Definitions result;
    for (const auto id : store.skill_ids()) {
        result.skills.push_back(store.require_skill(id));
    }
    for (const auto id : store.passive_ids()) {
        result.passives.push_back(store.require_passive(id));
    }
    for (const auto id : store.buff_ids()) {
        // Buffs are shared, immutable definitions; alias the store's copy.
        result.buffs.push_back(std::shared_ptr<const BuffSpec>(
            std::shared_ptr<const BuffSpec>{}, &store.require_buff(id)));
    }
    return result;
}

bool Report::has_errors() const {
    return std::any_of(findings.begin(), findings.end(), [](const Finding& finding) {
        return finding.severity == Severity::error;
    });
}

Report check(const Definitions& definitions, const Options& options) {
    Report report;
    const auto graph = build_graph(definitions);
    check_loops(graph, report);
    if (options.stress && !definitions.passives.empty()) {
        stress(definitions, options, report.has_errors(), report);
    }
    std::stable_sort(report.findings.begin(), report.findings.end(),
                     [](const Finding& a, const Finding& b) { return a.severity < b.severity; });
    return report;
}

std::string format(const Report& report) {
    std::string text;
    for (const auto& finding : report.findings) {
        switch (finding.severity) {
        case Severity::error: text += "error: "; break;
        case Severity::warning: text += "warning: "; break;
        case Severity::note: text += "note: "; break;
        }
        text += finding.message + "\n";
    }
    return text;
}

} // namespace gamebattle::config_check
