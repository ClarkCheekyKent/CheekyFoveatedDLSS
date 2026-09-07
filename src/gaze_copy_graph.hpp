#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

namespace cheeky::foveated_dlss {
struct GazeCopyRegion {
    std::uint64_t resource{};
    std::uint32_t subresource{}, x{}, y{}, width{}, height{};
    bool operator==(const GazeCopyRegion&) const = default;
};
struct GazeCopyEdge {
    GazeCopyRegion source{}, destination{};
    std::uint64_t time_ms{}, sequence{};
};

// Only observed, submitted, equal-size copies establish provenance. No matching
// by resolution or call order. Expire history and invalidate destroyed resources.
class GazeCopyGraph {
public:
    void record(GazeCopyEdge edge, std::uint64_t now) {
        expire(now);
        if (!edge.source.resource || !edge.destination.resource ||
            edge.source.resource == edge.destination.resource ||
            edge.source.width == 0 || edge.source.height == 0 ||
            edge.source.width != edge.destination.width ||
            edge.source.height != edge.destination.height) return;
        edge.time_ms = now;
        edge.sequence = ++sequence_;
        std::erase_if(edges_, [&](const auto& e) {
            return e.source == edge.source && e.destination == edge.destination;
        });
        edges_.push_back(edge);
        if (edges_.size() > 512) edges_.pop_front();
    }
    void forget(std::uint64_t resource) {
        std::erase_if(edges_, [=](const auto& e) {
            return e.source.resource == resource || e.destination.resource == resource;
        });
    }
    void clear() { edges_.clear(); }
    const std::deque<GazeCopyEdge>& recent_edges(std::uint64_t now) {
        expire(now);
        return edges_;
    }
    bool reaches(GazeCopyRegion source, const GazeCopyRegion& target, std::uint64_t now) {
        expire(now);
        struct Node { GazeCopyRegion region; unsigned depth; std::uint64_t sequence; };
        std::vector<Node> pending{{source, 0, 0}};
        for (std::size_t i = 0; i < pending.size() && i < 128; ++i) {
            const auto node = pending[i];
            if (node.region == target && node.depth != 0) return true;
            if (node.depth >= 4) continue;
            for (const auto& edge : edges_) {
                const auto& r = node.region;
                const auto& s = edge.source;
                if (edge.sequence <= node.sequence || r.resource != s.resource ||
                    r.subresource != s.subresource || r.x < s.x || r.y < s.y ||
                    std::uint64_t(r.x) + r.width > std::uint64_t(s.x) + s.width ||
                    std::uint64_t(r.y) + r.height > std::uint64_t(s.y) + s.height) continue;
                auto next = r;
                next.resource = edge.destination.resource;
                next.subresource = edge.destination.subresource;
                next.x = edge.destination.x + (r.x - s.x);
                next.y = edge.destination.y + (r.y - s.y);
                if (pending.size() < 128) pending.push_back({next, node.depth + 1, edge.sequence});
            }
        }
        return false;
    }
private:
    void expire(std::uint64_t now) {
        while (!edges_.empty() && now - edges_.front().time_ms > 500) edges_.pop_front();
    }
    std::uint64_t sequence_{};
    std::deque<GazeCopyEdge> edges_;
};
} // namespace cheeky::foveated_dlss
