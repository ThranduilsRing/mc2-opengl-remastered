#pragma once

#include "save_archive.h"

#include <expected>

namespace mc2::sim {
using save::Archive;
using Tick = std::uint64_t;

enum class StreamId : std::uint32_t {
    MissionInit = 0x494e4954, Combat = 0x434f4d42, PilotChecks = 0x50494c54,
    AI = 0x41494445, Path = 0x50415448, ABL = 0x41424c52,
    Physical = 0x50485953, Environment = 0x454e5652
};

struct StreamDefinition {
    StreamId id;
    std::string_view owner;
};
// Numeric ID order is the save order; presentation owners have no entry here.
inline constexpr std::array<StreamDefinition, 8> simStreamTable{{
    {StreamId::ABL, "Mission ABL host"}, {StreamId::AI, "DecisionSystem"},
    {StreamId::Combat, "CombatSystem"}, {StreamId::Environment, "Environment timeline"},
    {StreamId::MissionInit, "Mission loader/kernel"}, {StreamId::Path, "PathSystem"},
    {StreamId::Physical, "Physical/impact system"}, {StreamId::PilotChecks, "WarriorSystem"}
}};

struct RngStream {
    std::uint32_t streamId = 0, algorithmId = 1, algorithmVersion = 1;
    std::uint32_t initialSeed = 0, state = 0;
    std::uint64_t drawCount = 0;
    void serialize(Archive& a) {
        a(streamId, algorithmId, algorithmVersion, initialSeed, state, drawCount);
    }
    bool operator==(const RngStream&) const = default;
};

struct AblReseed {
    std::uint64_t ordinal = 0;
    std::uint32_t instanceId = 0;
    Tick tick = 0;
    std::int32_t requestedSeed = 0;
    std::uint32_t resolvedSeed = 0;
    void serialize(Archive& a) { a(ordinal, instanceId, tick, requestedSeed, resolvedSeed); }
    bool operator==(const AblReseed&) const = default;
};

// These fields belong to the ABL save section, not to RngState.
struct AblReseedState {
    std::uint64_t reseedOrdinal = 0;
    std::vector<AblReseed> reseedHistory;
    void serialize(Archive& a) { a(reseedOrdinal, reseedHistory); }
    void validate(Archive& a) const;
    bool operator==(const AblReseedState&) const = default;
};

enum class RngError { UnknownStream, DrawCountOverflow, InvalidReseed, HistoryCapacity };

class SimRng {
public:
    explicit SimRng(std::uint32_t rootSeed = 0);
    std::uint32_t rootSeed() const { return rootSeed_; }
    std::span<const RngStream, 8> streams() const { return streams_; }
    const RngStream* stream(StreamId id) const;
    std::expected<std::uint16_t, RngError> draw(StreamId id, save::Diagnostic diagnostic = {});
    std::expected<AblReseed, RngError> reseedAbl(AblReseedState& history,
        std::uint32_t instanceId, Tick executingTick, std::int32_t requestedSeed,
        save::Diagnostic diagnostic = {});
    void serialize(Archive& a);
    bool operator==(const SimRng&) const = default;
private:
    std::uint32_t rootSeed_ = 0;
    std::array<RngStream, 8> streams_{};
};
} // namespace mc2::sim
