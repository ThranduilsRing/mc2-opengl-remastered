#include "sim_rng.h"

#include <algorithm>
#include <cstdio>

namespace mc2::sim {
namespace {
void report(const save::Diagnostic& diagnostic, std::string_view reason) {
    const std::string line = "[SIM_RNG v1] reject reason=" + std::string(reason);
    if (diagnostic) diagnostic(line);
    else { std::fwrite(line.data(), 1, line.size(), stderr); std::fputc('\n', stderr); }
}

std::uint32_t derivedAblSeed(std::uint32_t root, std::uint32_t instance, Tick tick,
                             std::uint64_t ordinal) {
    std::uint32_t value = root ^ instance ^ static_cast<std::uint32_t>(tick) ^
        static_cast<std::uint32_t>(tick >> 32) ^ static_cast<std::uint32_t>(ordinal) ^
        static_cast<std::uint32_t>(ordinal >> 32) ^ 0x41424c52u;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
} // namespace

void AblReseedState::validate(Archive& a) const {
    if (reseedOrdinal != reseedHistory.size()) { a.reject("abl_reseed_ordinal"); return; }
    Tick previous = 0;
    for (std::size_t i = 0; i < reseedHistory.size(); ++i) {
        const auto& r = reseedHistory[i];
        if (r.ordinal != i + 1 || r.tick == 0 || r.tick < previous || r.requestedSeed < -1 ||
            (r.requestedSeed >= 0 && r.resolvedSeed != static_cast<std::uint32_t>(r.requestedSeed))) {
            a.reject("abl_reseed_history"); return;
        }
        previous = r.tick;
    }
}

SimRng::SimRng(std::uint32_t rootSeed) : rootSeed_(rootSeed) {
    for (std::size_t i = 0; i < streams_.size(); ++i) {
        const auto id = static_cast<std::uint32_t>(simStreamTable[i].id);
        streams_[i] = {id, 1, 1, rootSeed ^ id, rootSeed ^ id, 0};
    }
}

const RngStream* SimRng::stream(StreamId id) const {
    const auto found = std::ranges::find(streams_, static_cast<std::uint32_t>(id), &RngStream::streamId);
    return found == streams_.end() ? nullptr : &*found;
}

std::expected<std::uint16_t, RngError> SimRng::draw(StreamId id, save::Diagnostic diagnostic) {
    const auto index = std::ranges::find(streams_, static_cast<std::uint32_t>(id), &RngStream::streamId);
    if (index == streams_.end()) {
        report(diagnostic, "unknown_stream"); return std::unexpected(RngError::UnknownStream);
    }
    if (index->drawCount == std::numeric_limits<std::uint64_t>::max()) {
        report(diagnostic, "draw_count_overflow"); return std::unexpected(RngError::DrawCountOverflow);
    }
    index->state = index->state * 1664525u + 1013904223u;
    ++index->drawCount;
    return static_cast<std::uint16_t>((index->state >> 17) & 0x7fffu);
}

std::expected<AblReseed, RngError> SimRng::reseedAbl(AblReseedState& history,
    std::uint32_t instanceId, Tick executingTick, std::int32_t requestedSeed,
    save::Diagnostic diagnostic) {
    Archive check([](std::string_view) {});
    history.validate(check);
    if (!check.good() || requestedSeed < -1 || executingTick == 0 ||
        (!history.reseedHistory.empty() && executingTick < history.reseedHistory.back().tick)) {
        report(diagnostic, "invalid_reseed"); return std::unexpected(RngError::InvalidReseed);
    }
    if (history.reseedOrdinal == std::numeric_limits<std::uint64_t>::max() ||
        history.reseedHistory.size() >= save::maxSequenceLength) {
        report(diagnostic, "reseed_history_capacity"); return std::unexpected(RngError::HistoryCapacity);
    }
    const auto ordinal = history.reseedOrdinal + 1;
    const auto resolved = requestedSeed == -1 ? derivedAblSeed(rootSeed_, instanceId, executingTick, ordinal)
                                             : static_cast<std::uint32_t>(requestedSeed);
    const AblReseed record{ordinal, instanceId, executingTick, requestedSeed, resolved};
    history.reseedHistory.push_back(record);
    history.reseedOrdinal = ordinal;
    const auto found = std::ranges::find(streams_, static_cast<std::uint32_t>(StreamId::ABL), &RngStream::streamId);
    found->state = resolved;
    return record;
}

void SimRng::serialize(Archive& a) {
    auto root = rootSeed_;
    auto records = streams_;
    std::uint32_t count = 8;
    a(root, count);
    if (!a.good()) return;
    if (count != 8) { a.reject("rng_stream_count"); return; }
    a(records);
    for (std::size_t i = 0; i < records.size() && a.good(); ++i) {
        const auto& r = records[i];
        if (r.streamId != static_cast<std::uint32_t>(simStreamTable[i].id)) a.reject("rng_stream_id");
        else if (r.algorithmId != 1 || r.algorithmVersion != 1) a.reject("rng_algorithm");
        else if (r.initialSeed != (root ^ r.streamId)) a.reject("rng_initial_seed");
    }
    if (a.good() && a.reading()) { rootSeed_ = root; streams_ = records; }
}
} // namespace mc2::sim
