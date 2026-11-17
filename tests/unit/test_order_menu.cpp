#include "doctest.h"
#include "../../code/order_menu.h"
using namespace mc2::order_menu;

TEST_CASE("order menu maps choices with captured recipients and target") {
    const Item items[] = {Item::Attack, Item::Short, Item::Medium, Item::Long};
    const Range ranges[] = {Range::Optimal, Range::Short, Range::Medium, Range::Long};
    for (int i = 0; i < 4; ++i) {
        Menu menu;
        std::vector<std::uint32_t> selected{7, 11};
        std::uint32_t target = 19;
        menu.captured = Order{selected, target};
        selected = {99}; target = 88;
        auto order = menu.choose(items[i]);
        REQUIRE(order);
        CHECK(order->recipients == std::vector<std::uint32_t>{7, 11});
        CHECK(order->target == 19);
        CHECK(order->range == ranges[i]);
        CHECK_FALSE(menu.captured);
    }
}

TEST_CASE("order menu consumes open dismiss and release without click through") {
    for (auto item : {Item::Attack, Item::Cancel}) {
        Menu menu;
        CHECK_FALSE(menu.consume(false));
        menu.captured = Order{{1}, 2};
        CHECK(menu.consume(false));
        CHECK(menu.consume(true));
        menu.choose(item);
        CHECK(menu.consume(true));
        CHECK(menu.consume(true));
        CHECK(menu.consume(false)); // closing mouse/Esc release still owned
        CHECK_FALSE(menu.consume(false));
        CHECK_FALSE(menu.choose(Item::Attack));
    }
}

TEST_CASE("order menu disabled alpha and priority cannot dispatch") {
    Menu menu;
    menu.captured = Order{{1}, 2};
    CHECK_FALSE(menu.choose(Item::Alpha));
    CHECK_FALSE(menu.choose(Item::Priority));
    CHECK(menu.captured.has_value());
    CHECK_FALSE(menu.choose(Item::Cancel));
    CHECK_FALSE(menu.captured);
}
