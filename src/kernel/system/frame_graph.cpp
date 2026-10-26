#include "corona/kernel/system/frame_graph.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Corona::Kernel {
namespace {

double elapsed_ms(std::chrono::steady_clock::time_point from,
                  std::chrono::steady_clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

}  // namespace

FrameNodeId FrameGraph::add_node(FrameNodeDesc desc) {
    if (desc.name.empty()) {
        throw std::invalid_argument("FrameGraph::add_node: node name must not be empty");
    }
    if (find_node(desc.name) != kInvalidFrameNodeId) {
        throw std::invalid_argument("FrameGraph::add_node: duplicate node name '" + desc.name + "'");
    }
    if (desc.period_ticks == 0) {
        desc.period_ticks = 1;
    }
    if (desc.phase >= desc.period_ticks) {
        throw std::invalid_argument("FrameGraph::add_node: node '" + desc.name +
                                    "' has phase >= period_ticks");
    }

    const auto id = static_cast<FrameNodeId>(nodes_.size());
    nodes_.push_back(Node{std::move(desc), FrameNodeStats{}});
    // Adding a node can introduce new edges into an already compiled order.
    compiled_ = false;
    order_.clear();
    return id;
}

FrameNodeId FrameGraph::find_node(std::string_view name) const {
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        if (nodes_[index].desc.name == name) {
            return static_cast<FrameNodeId>(index);
        }
    }
    return kInvalidFrameNodeId;
}

std::size_t FrameGraph::node_count() const {
    return nodes_.size();
}

std::string_view FrameGraph::node_name(FrameNodeId id) const {
    if (!valid(id)) return {};
    return nodes_[id].desc.name;
}

bool FrameGraph::compile(std::string* error) {
    const auto fail = [error](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };

    // Node names are only valid for diagnostics here, so keep the helper local.
    const auto describe = [this](const std::vector<FrameNodeId>& ids) {
        std::ostringstream out;
        bool first = true;
        for (const auto id : ids) {
            if (!first) out << ", ";
            first = false;
            out << (valid(id) ? nodes_[id].desc.name : std::string("<invalid>"));
        }
        return out.str();
    };

    order_.clear();
    compiled_ = false;

    if (nodes_.empty()) {
        compiled_ = true;
        return true;
    }

    std::vector<std::size_t> indegree(nodes_.size(), 0);
    std::vector<std::vector<FrameNodeId>> outgoing(nodes_.size());

    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        const auto id = static_cast<FrameNodeId>(index);
        for (const auto dep : nodes_[index].desc.dependencies) {
            if (!valid(dep)) {
                std::ostringstream out;
                out << "node '" << nodes_[index].desc.name << "' depends on unknown node id "
                    << dep;
                return fail(out.str());
            }
            if (dep == id) {
                return fail("node '" + nodes_[index].desc.name + "' depends on itself");
            }
            outgoing[dep].push_back(id);
            ++indegree[index];
        }
    }

    // Kahn's algorithm. Ties are broken by insertion index so the produced order is
    // stable: the same graph always compiles to the same execution sequence.
    std::priority_queue<FrameNodeId, std::vector<FrameNodeId>, std::greater<>> ready;
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        if (indegree[index] == 0) ready.push(static_cast<FrameNodeId>(index));
    }

    order_.reserve(nodes_.size());
    while (!ready.empty()) {
        const auto id = ready.top();
        ready.pop();
        order_.push_back(id);
        for (const auto next : outgoing[id]) {
            if (--indegree[next] == 0) ready.push(next);
        }
    }

    if (order_.size() != nodes_.size()) {
        std::vector<FrameNodeId> stuck;
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            if (indegree[index] != 0) stuck.push_back(static_cast<FrameNodeId>(index));
        }
        // Report one concrete cycle by walking dependencies until a node repeats.
        std::vector<FrameNodeId> cycle;
        if (!stuck.empty()) {
            std::vector<std::uint8_t> seen(nodes_.size(), 0);
            FrameNodeId cursor = stuck.front();
            while (cursor != kInvalidFrameNodeId && !seen[cursor]) {
                seen[cursor] = 1;
                cycle.push_back(cursor);
                FrameNodeId next = kInvalidFrameNodeId;
                for (const auto dep : nodes_[cursor].desc.dependencies) {
                    if (indegree[dep] != 0) {
                        next = dep;
                        break;
                    }
                }
                cursor = next;
            }
            if (cursor != kInvalidFrameNodeId && seen[cursor]) {
                cycle.push_back(cursor);
            }
        }
        std::ostringstream out;
        out << "dependency cycle detected among " << stuck.size() << " node(s): "
            << describe(stuck);
        if (!cycle.empty()) {
            out << " (candidate cycle: " << describe(cycle) << ")";
        }
        order_.clear();
        return fail(out.str());
    }

    compiled_ = true;
    return true;
}

const std::vector<FrameNodeId>& FrameGraph::execution_order() const {
    return order_;
}

FrameTickReport FrameGraph::run_tick(const FrameTick& tick) {
    FrameTickReport report;
    if (!compiled_) {
        report.completed = false;
        report.error = "frame graph is not compiled";
        return report;
    }

    using Clock = std::chrono::steady_clock;
    const auto tick_start = Clock::now();

    // active_[i] == 0 means the node (or one of its ancestors) is disabled this tick.
    dependents_mark_.assign(nodes_.size(), 0);

    for (const auto id : order_) {
        Node& node = nodes_[id];
        bool active = node.desc.enabled;
        if (active) {
            for (const auto dep : node.desc.dependencies) {
                if (dependents_mark_[dep] == 0) {
                    active = false;
                    break;
                }
            }
        }
        dependents_mark_[id] = active ? 1 : 0;
        if (!active) {
            ++node.stats.disabled_out;
            ++report.disabled;
            continue;
        }

        const std::uint32_t period = node.desc.period_ticks;
        if (period > 1 && (tick.tick % period) != (node.desc.phase % period)) {
            ++node.stats.gated_out;
            ++report.gated;
            continue;
        }

        const auto node_start = Clock::now();
        FrameNodeStatus status = FrameNodeStatus::idle;
        try {
            if (node.desc.body) {
                status = node.desc.body(tick);
            }
        } catch (const std::exception& error) {
            const double ms = elapsed_ms(node_start, Clock::now());
            ++node.stats.invocations;
            node.stats.last_ms = ms;
            node.stats.average_ms =
                (node.stats.average_ms * static_cast<double>(node.stats.invocations - 1) + ms) /
                static_cast<double>(node.stats.invocations);
            node.stats.max_ms = std::max(node.stats.max_ms, ms);
            ++node.stats.failures;
            ++node.stats.consecutive_failures;
            report.completed = false;
            report.failed_node = id;
            report.stopped_on = id;
            report.error = error.what();
            report.total_ms = elapsed_ms(tick_start, Clock::now());
            return report;
        } catch (...) {
            const double ms = elapsed_ms(node_start, Clock::now());
            ++node.stats.invocations;
            node.stats.last_ms = ms;
            node.stats.average_ms =
                (node.stats.average_ms * static_cast<double>(node.stats.invocations - 1) + ms) /
                static_cast<double>(node.stats.invocations);
            node.stats.max_ms = std::max(node.stats.max_ms, ms);
            ++node.stats.failures;
            ++node.stats.consecutive_failures;
            report.completed = false;
            report.failed_node = id;
            report.stopped_on = id;
            report.error = "unknown exception";
            report.total_ms = elapsed_ms(tick_start, Clock::now());
            return report;
        }

        const double ms = elapsed_ms(node_start, Clock::now());
        ++node.stats.invocations;
        node.stats.consecutive_failures = 0;
        if (status == FrameNodeStatus::idle) {
            ++node.stats.idle_invocations;
            ++report.idle;
        }
        node.stats.last_ms = ms;
        node.stats.average_ms =
            (node.stats.average_ms * static_cast<double>(node.stats.invocations - 1) + ms) /
            static_cast<double>(node.stats.invocations);
        node.stats.max_ms = std::max(node.stats.max_ms, ms);
        ++report.executed;

        if (boundary_hook_) {
            boundary_hook_(id, tick);
        }
    }

    report.total_ms = elapsed_ms(tick_start, Clock::now());
    return report;
}

void FrameGraph::set_boundary_hook(std::function<void(FrameNodeId, const FrameTick&)> hook) {
    boundary_hook_ = std::move(hook);
}

bool FrameGraph::set_enabled(FrameNodeId id, bool enabled) {
    if (!valid(id)) return false;
    nodes_[id].desc.enabled = enabled;
    return true;
}

bool FrameGraph::set_dependencies(FrameNodeId id, std::vector<FrameNodeId> dependencies) {
    if (!valid(id)) return false;
    nodes_[id].desc.dependencies = std::move(dependencies);
    compiled_ = false;
    order_.clear();
    return true;
}

bool FrameGraph::set_rate(FrameNodeId id, std::uint32_t period_ticks, std::uint32_t phase) {
    if (!valid(id)) return false;
    if (period_ticks == 0) period_ticks = 1;
    if (phase >= period_ticks) return false;
    nodes_[id].desc.period_ticks = period_ticks;
    nodes_[id].desc.phase = phase;
    return true;
}

FrameNodeStats FrameGraph::stats(FrameNodeId id) const {
    if (!valid(id)) return FrameNodeStats{};
    return nodes_[id].stats;
}

FrameGraphStats FrameGraph::snapshot_stats() const {
    FrameGraphStats snapshot;
    snapshot.nodes.reserve(nodes_.size());
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        snapshot.nodes.push_back(FrameNodeSnapshot{
            static_cast<FrameNodeId>(i), nodes_[i].desc.name, nodes_[i].stats});
    }
    return snapshot;
}

void FrameGraph::reset_stats() {
    for (auto& node : nodes_) {
        node.stats = FrameNodeStats{};
    }
}

std::string FrameGraph::dump() const {
    std::ostringstream out;
    out << "frame graph: " << nodes_.size() << " node(s), "
        << (compiled_ ? "compiled" : "not compiled");
    if (compiled_ && !order_.empty()) {
        out << ", order: ";
        bool first = true;
        for (const auto id : order_) {
            if (!first) out << ", ";
            first = false;
            out << node_name(id);
        }
    }
    out << '\n';
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        const auto& node = nodes_[index];
        out << "  [" << index << "] " << node.desc.name
            << " (period=" << node.desc.period_ticks << ", phase=" << node.desc.phase
            << ", enabled=" << (node.desc.enabled ? "true" : "false") << ')';
        if (!node.desc.dependencies.empty()) {
            out << " after=";
            bool first = true;
            for (const auto dep : node.desc.dependencies) {
                if (!first) out << ',';
                first = false;
                out << (valid(dep) ? nodes_[dep].desc.name : std::string("<invalid>"));
            }
        }
        out << " runs=" << node.stats.invocations
            << " idle=" << node.stats.idle_invocations
            << " gated=" << node.stats.gated_out
            << " disabled=" << node.stats.disabled_out;
        out.setf(std::ios::fixed);
        out.precision(3);
        out << " avg=" << node.stats.average_ms << "ms"
            << " max=" << node.stats.max_ms << "ms"
            << " last=" << node.stats.last_ms << "ms\n";
        out.unsetf(std::ios::fixed);
    }
    return out.str();
}

}  // namespace Corona::Kernel
