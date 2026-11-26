// Session sentinel (crash_session_sentinel.h): a session that died with no handler is found at the next launch, and a live or
// reused pid is not misreported.
// Trigger paths: GameOS/gameos/crash_session_sentinel.h, GameOS/gameos/gos_crashbundle.cpp.
#include "doctest.h"
#include "crash_session_sentinel.h"

using namespace mc2::crash;

TEST_CASE("session sentinel: round trip, CRLF and junk") {
    const SessionSentinel s{38760, 134388056540495114ull, "MC2FP1|520a0976dacc|0|claude/stage-1125c|FP1END"};
    const auto back = parseSentinel(formatSentinel(s));
    REQUIRE(back);
    CHECK(back->pid == s.pid);
    CHECK(back->created == s.created);
    CHECK(back->build == s.build);
    CHECK(parseSentinel("pid=12\r\ncreated=34\r\n")->created == 34);
    CHECK_FALSE(parseSentinel(""));
    CHECK_FALSE(parseSentinel("pid=12\n"));            // truncated write
    CHECK_FALSE(parseSentinel("pid=x\ncreated=1\n"));
    CHECK_FALSE(parseSentinel("pid=0\ncreated=1\n"));
}

TEST_CASE("session sentinel: orphaned only when the session's process is gone") {
    const SessionSentinel s{38760, 1000, ""};
    CHECK(sentinelOrphaned(s, false, 0));       // pid gone: died silently
    CHECK(sentinelOrphaned(s, true, 2000));     // pid reused by another process
    CHECK_FALSE(sentinelOrphaned(s, true, 1000)); // still running (a second instance in the same install)
}

TEST_CASE("session sentinel: failed record is retried and only a matching harness stop suppresses it") {
    const SessionSentinel s{38760, 1000, "build"};
    int writes = 0;
    auto failed = [&] { ++writes; return false; };
    CHECK_FALSE(retireOrphanedSentinel(s, std::nullopt, failed));
    CHECK_FALSE(retireOrphanedSentinel(s, std::nullopt, failed)); // next launch retries
    CHECK(writes == 2);
    CHECK(retireOrphanedSentinel(s, std::nullopt, [] { return true; }));
    CHECK(retireOrphanedSentinel(s, s, failed));
    CHECK(writes == 2);
    CHECK_FALSE(retireOrphanedSentinel(s, SessionSentinel{38760, 999, ""}, failed)); // reused PID
    CHECK_FALSE(retireOrphanedSentinel(s, SessionSentinel{1, 1000, ""}, failed));
    CHECK_FALSE(retireOrphanedSentinel(s, parseSentinel("truncated"), failed));
    CHECK_FALSE(retireOrphanedSentinel({38760, 0, ""}, SessionSentinel{38760, 0, ""}, failed));
    CHECK(writes == 6);
}
