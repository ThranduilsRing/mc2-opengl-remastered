#pragma once
// Order menu state: pure, no engine types. The menu owns the pointer (through an ImGui popup and the engine's
// PointerCapture layer) and the Esc key, nothing else: it never skips game code and never filters a release.
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>
#include "../GameOS/gameos/input_capture_policy.h"

namespace mc2::order_menu {
enum class Item { Attack, Short, Medium, Long, Alpha, Priority, Cancel };
enum class Range { Optimal, Short, Medium, Long };
enum class CancelReason { Esc, Dismissed, Context, Watchdog, CancelRow };

struct Order {
    std::vector<std::uint32_t> recipients;
    std::uint32_t target = 0;
    Range range = Range::Optimal;
};

// What a classic left-click attack accepts: alive, enabled, not a capture target, not friendly.
struct TargetFacts { bool destroyed = false, disabled = false, capturable = false, friendly = false; };
enum class TargetClass { Rejected, Attackable };
constexpr TargetClass classify(const TargetFacts& t) {
    return (t.destroyed || t.disabled || t.capturable || t.friendly) ? TargetClass::Rejected : TargetClass::Attackable;
}

// One predicate, evaluated every frame (open gate, per-frame validity, row enable).
// `classic`, `targetAttackable`, `selectionMatches`, `recipientAlive`: true is good. The rest: true blocks the menu.
struct ContextFacts {
    bool classic = true;
    bool movie = false, tutorialAnimation = false, guiFrozen = false, hotkeyReference = false;
    bool formationLine = false, legacyPauseMenu = false, mpPaused = false, placementMode = false;
    bool targetAttackable = true, selectionMatches = true, recipientAlive = true;
};
constexpr bool contextValid(const ContextFacts& f) {
    return f.classic && !f.movie && !f.tutorialAnimation && !f.guiFrozen && !f.hotkeyReference &&
           !f.formationLine && !f.legacyPauseMenu && !f.mpPaused && !f.placementMode &&
           f.targetAttackable && f.selectionMatches && f.recipientAlive;
}

enum class KeyAction { None, Press, Release, Skip };
// Mirrors the command loop. A release is never filtered, whatever the menu state.
constexpr KeyAction keyAction(input::capture::Edge edge, bool pressBlocked) {
    switch (edge) {
    case input::capture::Edge::Released: return KeyAction::Release;
    case input::capture::Edge::Pressed:
    case input::capture::Edge::Held: return pressBlocked ? KeyAction::Skip : KeyAction::Press;
    default: return KeyAction::None;
    }
}

struct Menu {
    std::optional<Order> captured; // open iff set
    std::uint64_t lastServiced = 0;
    bool escClaimed = false;
    std::optional<CancelReason> lastClose; // for the log line only

    bool isOpen() const { return captured.has_value(); }
    void open(Order order, std::uint64_t frame) {
        captured = std::move(order);
        lastServiced = frame;
        lastClose.reset();
    }
    // Once per frame, before any game code. Returns the reason when this frame closed the menu.
    std::optional<CancelReason> beginFrame(std::uint64_t frame, bool contextOk, bool escDown) {
        if (!escDown) escClaimed = false;
        if (!captured) return std::nullopt;
        std::optional<CancelReason> why;
        if (escDown) { why = CancelReason::Esc; escClaimed = true; }
        else if (!contextOk) why = CancelReason::Context;
        else if (lastServiced + 1 < frame) why = CancelReason::Watchdog; // previous frame never reached the service point
        if (why) close(*why);
        return why;
    }
    // At the service point, in a frame whose context was just validated.
    std::optional<Order> service(std::uint64_t frame, bool popupOpen, std::optional<Item> item) {
        if (!captured) return std::nullopt;
        lastServiced = frame;
        if (!popupOpen) { close(CancelReason::Dismissed); return std::nullopt; }
        if (!item) return std::nullopt;
        if (*item == Item::Cancel) { close(CancelReason::CancelRow); return std::nullopt; }
        if (*item == Item::Alpha || *item == Item::Priority) return std::nullopt;
        auto order = std::move(captured);
        captured.reset();
        switch (*item) {
        case Item::Short: order->range = Range::Short; break;
        case Item::Medium: order->range = Range::Medium; break;
        case Item::Long: order->range = Range::Long; break;
        default: order->range = Range::Optimal; break;
        }
        return order;
    }
    bool pressBlocked(bool isEscape) const { return isEscape && escClaimed; }

private:
    void close(CancelReason why) { captured.reset(); lastClose = why; }
};
}
