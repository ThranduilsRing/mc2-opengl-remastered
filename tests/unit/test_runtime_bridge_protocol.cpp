#include "doctest.h"
#include "../../GameOS/gameos/runtime_bridge_protocol.h"

namespace {
constexpr const char* session = "12345678-1234-1234-1234-123456789abc";
mc2::bridge::Json request() {
    return {{"version", 1}, {"session", session}, {"type", "subscribe"}, {"hz", 2},
            {"streams", {"runtime.observation", "mover.transform"}}};
}
}
TEST_CASE("runtime_bridge/wire length is big endian and bounded") {
    const auto header = mc2::bridge::frameHeader(0x01020304);
    CHECK(header[0] == 1);
    CHECK(header[3] == 4);
    CHECK(mc2::bridge::frameSize(header.data()) == 0x01020304);
    CHECK(mc2::bridge::maxFrameBytes == 1024 * 1024);
}
TEST_CASE("runtime_bridge/session requires a GUID") {
    CHECK(mc2::bridge::validSession(session));
    CHECK_FALSE(mc2::bridge::validSession(""));
    CHECK_FALSE(mc2::bridge::validSession("localhost:1234"));
    CHECK_FALSE(mc2::bridge::validSession("zzzzzzzz-1234-1234-1234-123456789abc"));
}
TEST_CASE("runtime_bridge/subscriptions authenticate version session rate and streams") {
    using namespace mc2::bridge;
    auto message = request();
    auto subscription = subscribe(message, session);
    REQUIRE(subscription);
    CHECK(subscription->hz == 2);
    CHECK(subscription->state);
    CHECK(subscription->movers);
    for (const auto& bad : {Json(0), Json(31), Json(-1), Json("2"), Json(nullptr)}) {
        message = request(); message["hz"] = bad;
        CHECK_FALSE(subscribe(message, session));
    }
    message = request(); message["version"] = 2;
    CHECK_FALSE(subscribe(message, session));
    message = request(); message["session"] = "stale-session";
    CHECK_FALSE(subscribe(message, session));
    message = request(); message["streams"] = {"eval"};
    CHECK_FALSE(subscribe(message, session));
    for (const char* command : {"pause", "resume", "stop", "eval", "teleport"}) {
        message = request(); message["type"] = command;
        CHECK_FALSE(subscribe(message, session));
    }
    message = request(); message["streams"] = Json::array();
    subscription = subscribe(message, session);
    REQUIRE(subscription);
    CHECK_FALSE(subscription->state);
    CHECK_FALSE(subscription->movers);
}
