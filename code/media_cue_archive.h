// B7.5 logical media/cue value records (design: .claude/cleanup/cxx23-step4-save-schema-design.md,
// MediaCueState / LogicalRequest / CueTrajectory / CueKeyframe). Private codec only: no runtime owner exists
// yet and no save writer calls it. Enum values are provisional until the B7.5 runtime lands.
#pragma once
#include "save_sections.h"

#include <optional>
#include <vector>

namespace mc2::save {

inline constexpr auto tagMediaCue = fourcc('M', 'C', 'U', 'E');

enum class CueKind : std::uint8_t { Music = 1, Voice = 2, Video = 3, CameraCue = 4 };
enum class CueStatus : std::uint8_t { Pending = 1, Active = 2, Completed = 3, Cancelled = 4 };
enum class CueInterpolation : std::uint8_t { Step = 1, Linear = 2 };

struct CueKeyframe {
    std::uint64_t offsetTicks = 0;
    Vec3 position, orientation;
    float zoom = 0;
    void serialize(Archive& a) { a(offsetTicks, position, orientation, zoom); }
    bool operator==(const CueKeyframe&) const = default;
};

struct CueTrajectory {
    CueInterpolation interpolation = CueInterpolation::Linear;
    std::vector<CueKeyframe> keyframes;
    void serialize(Archive& a) {
        auto tag = static_cast<std::uint8_t>(interpolation);
        a(tag);
        if (a.reading() && a.good()) {
            if (tag < 1 || tag > 2) { a.reject("cue_interpolation"); return; }
            interpolation = static_cast<CueInterpolation>(tag);
        }
        a.sequence(keyframes, maxSequenceLength);
        if (!a.good()) return;
        for (std::size_t i = 1; i < keyframes.size(); ++i)
            if (keyframes[i].offsetTicks < keyframes[i - 1].offsetTicks) { a.reject("cue_keyframe_order"); return; }
    }
    bool operator==(const CueTrajectory&) const = default;
};

struct LogicalRequest {
    std::uint64_t requestId = 0, sequence = 0;
    CueKind kind = CueKind::Music;
    std::optional<BoundedText<256>> assetKey;
    std::optional<std::uint32_t> sourceInstanceId;
    std::uint64_t startTick = 0;
    std::optional<std::uint64_t> endTick;
    std::uint64_t durationTicks = 0;
    bool loop = false;
    CueStatus status = CueStatus::Pending;
    std::optional<CueTrajectory> trajectory;
    bool completionLatched = false, consumptionLatched = false;

    void serialize(Archive& a) {
        auto k = static_cast<std::uint8_t>(kind);
        auto s = static_cast<std::uint8_t>(status);
        a(requestId, sequence, k, assetKey, sourceInstanceId, startTick, endTick, durationTicks, loop, s,
          trajectory, completionLatched, consumptionLatched);
        if (!a.good()) return;
        if (k < 1 || k > 4) { a.reject("cue_kind"); return; }
        if (s < 1 || s > 4) { a.reject("cue_status"); return; }
        if (a.reading()) { kind = static_cast<CueKind>(k); status = static_cast<CueStatus>(s); }
        if (endTick && *endTick < startTick) a.reject("cue_end_before_start");
        else if (trajectory && kind != CueKind::CameraCue) a.reject("cue_trajectory_kind");
    }
    bool operator==(const LogicalRequest&) const = default;
};

struct MediaCueState {
    std::uint64_t nextRequestId = 0, nextSequence = 0;
    std::vector<LogicalRequest> requests; // ascending sequence order

    void serialize(Archive& a) {
        a(nextRequestId, nextSequence);
        a.sequence(requests, maxSequenceLength);
        if (!a.good()) return;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            const auto& r = requests[i];
            if (r.sequence >= nextSequence || r.requestId >= nextRequestId) { a.reject("cue_counter"); return; }
            if (i && r.sequence <= requests[i - 1].sequence) { a.reject("cue_sequence_order"); return; }
        }
    }
    bool operator==(const MediaCueState&) const = default;
};

} // namespace mc2::save
