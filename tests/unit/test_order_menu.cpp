#include "doctest.h"
#include "../../code/order_menu.h"
using namespace mc2::order_menu;
using input::capture::Edge;

namespace {
Menu openMenu(std::uint64_t frame = 1) {
    Menu menu;
    menu.open(Order{{7, 11}, 19}, frame);
    return menu;
}
}

TEST_CASE("order menu: a release is never filtered in any state, for any key") {
    // Every held-mode hotkey release (S M L A J C G Space I, the waypoint key) and Esc itself.
    for (bool esc : {false, true})
        for (bool open : {false, true})
            for (bool claimed : {false, true}) {
                Menu menu;
                if (open) menu.open(Order{{1}, 2}, 1);
                menu.escClaimed = claimed;
                CAPTURE(esc); CAPTURE(open); CAPTURE(claimed);
                CHECK(keyAction(Edge::Released, menu.pressBlocked(esc)) == KeyAction::Release);
            }
    // Also after the menu closed for each reason.
    for (int reason = 0; reason < 5; ++reason) {
        Menu menu = openMenu();
        if (reason == 0) menu.beginFrame(2, true, true);
        else if (reason == 1) menu.service(2, false, std::nullopt);
        else if (reason == 2) menu.beginFrame(2, false, false);
        else if (reason == 3) menu.beginFrame(9, true, false);
        else menu.service(2, true, Item::Cancel);
        CHECK_FALSE(menu.isOpen());
        CHECK(keyAction(Edge::Released, menu.pressBlocked(true)) == KeyAction::Release);
    }
}

TEST_CASE("order menu: only the Esc press is filtered, only while claimed") {
    Menu menu = openMenu();
    CHECK(menu.beginFrame(2, true, true) == CancelReason::Esc);
    CHECK(menu.pressBlocked(true));
    CHECK_FALSE(menu.pressBlocked(false)); // every other key keeps working
    CHECK_FALSE(menu.beginFrame(3, true, true)); // Esc still down, menu closed
    CHECK(menu.pressBlocked(true));
    menu.beginFrame(4, true, false);
    CHECK_FALSE(menu.pressBlocked(true));
    CHECK(keyAction(Edge::Pressed, true) == KeyAction::Skip);
    CHECK(keyAction(Edge::Held, false) == KeyAction::Press);
    CHECK(keyAction(Edge::Free, false) == KeyAction::None);
}

TEST_CASE("order menu: every cancel path ends closed with no order") {
    Menu dismissed = openMenu();
    CHECK_FALSE(dismissed.service(2, false, Item::Attack));
    CHECK(dismissed.lastClose == CancelReason::Dismissed);
    Menu ctx = openMenu();
    CHECK(ctx.beginFrame(2, false, false) == CancelReason::Context);
    CHECK_FALSE(ctx.service(2, true, Item::Attack)); // nothing dispatches after a same-frame cancel
    Menu dog = openMenu(5);
    CHECK_FALSE(dog.beginFrame(6, true, false)); // previous frame was serviced
    CHECK(dog.beginFrame(8, true, false) == CancelReason::Watchdog); // frames 6 and 7 never reached the service point
    Menu cancel = openMenu();
    CHECK_FALSE(cancel.service(2, true, Item::Cancel));
    CHECK(cancel.lastClose == CancelReason::CancelRow);
    for (auto* m : {&dismissed, &ctx, &dog, &cancel}) { CHECK_FALSE(m->isOpen()); CHECK_FALSE(m->escClaimed); }
}

TEST_CASE("order menu: each context input going false closes the menu") {
    CHECK(contextValid(ContextFacts{}));
    auto closes = [](ContextFacts f) {
        Menu menu = openMenu();
        const bool closed = menu.beginFrame(2, contextValid(f), false) == CancelReason::Context;
        return closed && !menu.isOpen();
    };
    CHECK(closes([] { ContextFacts f; f.classic = false; return f; }()));
    CHECK(closes([] { ContextFacts f; f.movie = true; return f; }()));
    CHECK(closes([] { ContextFacts f; f.tutorialAnimation = true; return f; }()));
    CHECK(closes([] { ContextFacts f; f.guiFrozen = true; return f; }()));
    CHECK(closes([] { ContextFacts f; f.hotkeyReference = true; return f; }()));
    CHECK(closes([] { ContextFacts f; f.formationLine = true; return f; }()));
    CHECK(closes([] { ContextFacts f; f.legacyPauseMenu = true; return f; }()));
    CHECK(closes([] { ContextFacts f; f.mpPaused = true; return f; }()));
    CHECK(closes([] { ContextFacts f; f.placementMode = true; return f; }()));
    CHECK(closes([] { ContextFacts f; f.targetAttackable = false; return f; }()));
    CHECK(closes([] { ContextFacts f; f.selectionMatches = false; return f; }()));
    CHECK(closes([] { ContextFacts f; f.recipientAlive = false; return f; }()));
}

TEST_CASE("order menu: dispatch returns the captured recipients and target with the row's range") {
    const Item items[] = {Item::Attack, Item::Short, Item::Medium, Item::Long};
    const Range ranges[] = {Range::Optimal, Range::Short, Range::Medium, Range::Long};
    for (int i = 0; i < 4; ++i) {
        Menu menu = openMenu();
        auto order = menu.service(2, true, items[i]);
        REQUIRE(order);
        CHECK(order->recipients == std::vector<std::uint32_t>{7, 11});
        CHECK(order->target == 19);
        CHECK(order->range == ranges[i]);
        CHECK_FALSE(menu.isOpen());
    }
    Menu menu = openMenu();
    CHECK_FALSE(menu.service(2, true, Item::Alpha));
    CHECK_FALSE(menu.service(3, true, Item::Priority));
    CHECK(menu.isOpen()); // disabled rows leave it open
    CHECK_FALSE(menu.service(4, true, std::nullopt));
    CHECK(menu.isOpen());
}

TEST_CASE("order menu: classify matches what a classic attack accepts") {
    CHECK(classify({}) == TargetClass::Attackable); // hostile or neutral
    CHECK(classify({true, false, false, false}) == TargetClass::Rejected);
    CHECK(classify({false, true, false, false}) == TargetClass::Rejected);
    CHECK(classify({false, false, true, false}) == TargetClass::Rejected);
    CHECK(classify({false, false, false, true}) == TargetClass::Rejected);
}
