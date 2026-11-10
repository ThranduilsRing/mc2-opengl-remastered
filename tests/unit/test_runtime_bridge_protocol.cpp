#include "doctest.h"
#include "../../GameOS/gameos/runtime_bridge_protocol.h"
#include "../../3rdparty/include/nlohmann/json.hpp"

namespace {
using Json = nlohmann::json;
constexpr const char* session = "12345678-1234-1234-1234-123456789abc";
Json request() {
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
    auto subscription = subscribe(message.dump(), session);
    REQUIRE(subscription);
    CHECK(subscription->hz == 2);
    CHECK(subscription->state);
    CHECK(subscription->movers);
    for (const auto& bad : {Json(0), Json(31), Json(-1), Json("2"), Json(nullptr)}) {
        message = request(); message["hz"] = bad;
        CHECK_FALSE(subscribe(message.dump(), session));
    }
    message = request(); message["version"] = 2;
    CHECK_FALSE(subscribe(message.dump(), session));
    message = request(); message["session"] = "stale-session";
    CHECK_FALSE(subscribe(message.dump(), session));
    message = request(); message["streams"] = {"eval"};
    CHECK_FALSE(subscribe(message.dump(), session));
    for (const char* command : {"pause", "resume", "stop", "eval", "teleport"}) {
        message = request(); message["type"] = command;
        CHECK_FALSE(subscribe(message.dump(), session));
    }
    message = request(); message["streams"] = Json::array();
    subscription = subscribe(message.dump(), session);
    REQUIRE(subscription);
    CHECK_FALSE(subscription->state);
    CHECK_FALSE(subscription->movers);
    CHECK_FALSE(subscribe("{", session));
}
namespace {
Json commandMessage(const char* cmd) {
    return {{"version", 1}, {"session", session}, {"type", "command"}, {"cmd", cmd}};
}
}
TEST_CASE("runtime_bridge/commands parse into the closed enum") {
    using namespace mc2::bridge;
    auto message = commandMessage("pause");
    auto parsed = parseCommand(message.dump(), session);
    REQUIRE(parsed.command);
    CHECK(parsed.command->kind == CommandKind::Pause);
    CHECK(parseCommand(commandMessage("resume").dump(), session).command->kind == CommandKind::Resume);
    message = commandMessage("step"); message["n"] = 7;
    parsed = parseCommand(message.dump(), session);
    REQUIRE(parsed.command);
    CHECK(parsed.command->kind == CommandKind::Step);
    CHECK(parsed.command->steps == 7);
    message = commandMessage("set_speed"); message["speed"] = 2.5;
    parsed = parseCommand(message.dump(), session);
    REQUIRE(parsed.command);
    CHECK(parsed.command->kind == CommandKind::SetSpeed);
    CHECK(parsed.command->speed == 2.5f);
}
TEST_CASE("runtime_bridge/commands reject unknown, malformed and out-of-range input") {
    using namespace mc2::bridge;
    for (const char* name : {"eval", "abl", "teleport", "stop", "PAUSE", ""}) {
        const auto parsed = parseCommand(commandMessage(name).dump(), session);
        CHECK_FALSE(parsed.command);
        CHECK(parsed.error == "unknown_command");
    }
    for (const auto& bad : {Json(0), Json(-3), Json(100001), Json(1.5), Json("3"), Json(nullptr)}) {
        auto message = commandMessage("step"); message["n"] = bad;
        CHECK(parseCommand(message.dump(), session).error == "bad_argument");
    }
    CHECK(parseCommand(commandMessage("step").dump(), session).error == "bad_argument");
    for (const auto& bad : {Json(0.1), Json(4.5), Json(0), Json("2"), Json(nullptr)}) {
        auto message = commandMessage("set_speed"); message["speed"] = bad;
        CHECK(parseCommand(message.dump(), session).error == "bad_argument");
    }
    auto message = commandMessage("pause"); message["session"] = "stale";
    CHECK(parseCommand(message.dump(), session).error == "bad_message");
    message = commandMessage("pause"); message["type"] = "subscribe";
    CHECK(parseCommand(message.dump(), session).error == "bad_message");
    message = commandMessage("pause"); message["cmd"] = 5;
    CHECK(parseCommand(message.dump(), session).error == "bad_message");
    CHECK(parseCommand("{", session).error == "bad_message");
}
TEST_CASE("runtime_bridge/control pause holds, step is bounded and consumed, resume clears") {
    using namespace mc2::bridge;
    Control control;
    CHECK_FALSE(control.paused());
    CHECK_FALSE(control.apply({CommandKind::Step, 3, 1.0f}));   // step only while paused
    CHECK(control.apply({CommandKind::Pause, 0, 1.0f}));
    CHECK(control.paused());
    CHECK(control.apply({CommandKind::Step, 6, 1.0f}));
    CHECK(control.takeSteps(4) == 4);
    CHECK(control.pendingSteps() == 2);
    CHECK(control.takeSteps(4) == 2);
    CHECK(control.takeSteps(4) == 0);
    CHECK(control.apply({CommandKind::Step, 5, 1.0f}));
    CHECK(control.apply({CommandKind::Resume, 0, 1.0f}));
    CHECK_FALSE(control.paused());
    CHECK(control.pendingSteps() == 0);
    float speed = 0;
    CHECK_FALSE(control.takeSpeed(speed));
    CHECK(control.apply({CommandKind::SetSpeed, 0, 2.0f}));
    CHECK(control.takeSpeed(speed));
    CHECK(speed == 2.0f);
    CHECK_FALSE(control.takeSpeed(speed));
    control.noteTicks(3); control.noteTicks(0);
    CHECK(control.ticks() == 3);
}
