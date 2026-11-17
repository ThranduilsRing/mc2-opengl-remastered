#pragma once
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace mc2::order_menu {
enum class Item { Attack, Short, Medium, Long, Alpha, Priority, Cancel };
enum class Range { Optimal, Short, Medium, Long };
struct Order {
    std::vector<std::uint32_t> recipients;
    std::uint32_t target = 0;
    Range range = Range::Optimal;
};
struct Menu {
    std::optional<Order> captured;
    bool draining = false;
    float x = 0, y = 0;
    float w = 0, h = 0; // last measured window size, so the clamp runs before the rows hit-test

    // Consume the closing frame AND the release, even outside the window.
    bool consume(bool held) {
        if (captured) return true;
        if (!draining) return false;
        draining = held;
        return true;
    }
    // `held`: a mouse button or Esc is still down; only then is there a release left to swallow.
    std::optional<Order> choose(Item item, bool held = true) {
        if (!captured || item == Item::Alpha || item == Item::Priority) return std::nullopt;
        auto order = std::move(captured);
        captured.reset();
        draining = held;
        switch (item) {
        case Item::Attack: order->range = Range::Optimal; break;
        case Item::Short: order->range = Range::Short; break;
        case Item::Medium: order->range = Range::Medium; break;
        case Item::Long: order->range = Range::Long; break;
        default: return std::nullopt;
        }
        return order;
    }
};
}
