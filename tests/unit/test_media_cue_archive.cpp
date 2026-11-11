#include "doctest.h"
#include "media_cue_archive.h"

namespace {
using namespace mc2::save;

MediaCueState sample() {
    MediaCueState s;
    s.nextRequestId = 9;
    s.nextSequence = 12;
    LogicalRequest music;
    music.requestId = 3; music.sequence = 4; music.kind = CueKind::Music;
    music.assetKey = BoundedText<256>(1, "music/theme01");
    music.startTick = 30; music.durationTicks = 900; music.loop = true;
    music.status = CueStatus::Active;
    LogicalRequest cam;
    cam.requestId = 8; cam.sequence = 11; cam.kind = CueKind::CameraCue;
    cam.sourceInstanceId = 5u; cam.startTick = 100; cam.endTick = 400; cam.durationTicks = 300;
    cam.status = CueStatus::Completed; cam.completionLatched = true;
    CueTrajectory t;
    t.interpolation = CueInterpolation::Step;
    t.keyframes = {{0, {1, 2, 3}, {0.5f, 0, 0}, 1.0f}, {150, {4, 5, 6}, {0, 0.25f, 0}, 1.5f}};
    cam.trajectory = t;
    s.requests = {music, cam};
    return s;
}

bool decodes(const MediaCueState& s) {
    MediaCueState out;
    return decodeTaggedRecord(encodeTaggedRecord(s, tagMediaCue), out, tagMediaCue);
}
} // namespace

TEST_CASE("media cue archive: roundtrip is exact and re-encodes to the same bytes") {
    const auto state = sample();
    const auto bytes = encodeTaggedRecord(state, tagMediaCue);
    REQUIRE_FALSE(bytes.empty());
    MediaCueState out;
    REQUIRE(decodeTaggedRecord(bytes, out, tagMediaCue));
    CHECK(out == state);
    CHECK(encodeTaggedRecord(out, tagMediaCue) == bytes);
}

TEST_CASE("media cue archive: empty state roundtrips") {
    MediaCueState empty, out;
    out.nextRequestId = 77;
    REQUIRE(decodeTaggedRecord(encodeTaggedRecord(empty, tagMediaCue), out, tagMediaCue));
    CHECK(out == empty);
}

TEST_CASE("media cue archive: wrong tag and truncation are rejected without touching the output") {
    const auto bytes = encodeTaggedRecord(sample(), tagMediaCue);
    MediaCueState out;
    out.nextSequence = 5;
    CHECK_FALSE(decodeTaggedRecord(bytes, out, tagAblEnvironment));
    auto cut = bytes;
    cut.pop_back();
    CHECK_FALSE(decodeTaggedRecord(cut, out, tagMediaCue));
    CHECK(out.nextSequence == 5);
}

TEST_CASE("media cue archive: schema invariants refuse to encode") {
    auto s = sample();
    s.requests[1].sequence = 4; // not ascending
    CHECK_FALSE(decodes(s));
    s = sample();
    s.requests[1].requestId = 9; // >= nextRequestId
    CHECK_FALSE(decodes(s));
    s = sample();
    s.requests[1].endTick = 50; // before start
    CHECK_FALSE(decodes(s));
    s = sample();
    s.requests[0].trajectory = CueTrajectory{}; // trajectory on a music cue
    CHECK_FALSE(decodes(s));
    s = sample();
    s.requests[1].trajectory->keyframes[1].offsetTicks = 0; // allowed (equal offsets), then break order
    s.requests[1].trajectory->keyframes[0].offsetTicks = 5;
    CHECK_FALSE(decodes(s));
    s = sample();
    s.requests[0].kind = static_cast<CueKind>(9);
    CHECK_FALSE(decodes(s));
}

TEST_CASE("media cue archive: corrupted enum bytes are rejected on read") {
    auto bytes = encodeTaggedRecord(sample(), tagMediaCue);
    // header: tag(4) version(4) nextRequestId(8) nextSequence(8) count(4?) then request 0: id(8) seq(8) kind(1)
    MediaCueState probe;
    bool flipped = false;
    for (std::size_t i = 8; i < bytes.size() && !flipped; ++i) {
        auto copy = bytes;
        copy[i] = 0xEE;
        if (!decodeTaggedRecord(copy, probe, tagMediaCue)) flipped = true;
    }
    CHECK(flipped);
}
