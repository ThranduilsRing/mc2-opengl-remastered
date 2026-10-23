#include "command_replay.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "appearance_rng.h"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cstdio>
#include <charconv>
#include "sim_rng_state.h"

namespace mc2::replay {
namespace {
struct Command {
    sim::Tick admitted = 0;
    sim::OrderEnvelope envelope;
    std::optional<sim::OrderReceipt> receipt;
};
struct Checkpoint { std::uint64_t tick; std::array<std::uint64_t, 5> values; };
struct Session {
    std::ofstream record;
    bool playback = false, failed = false, complete = false;
    std::vector<Command> commands;
    std::vector<Checkpoint> checkpoints;
    std::size_t nextHash = 0;
    std::size_t recordedCommands = 0, recordedHashes = 0;
};
Session session;
constexpr std::array names{"movers", "objects", "abl", "animation", "rng"};
std::string encode(sim::OrderEnvelope envelope) {
    save::Archive archive;
    archive(envelope);
    if (!archive.good()) throw std::runtime_error("replay command serialization failed");
    std::ostringstream text;
    for (auto byte : archive.bytes()) text << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return text.str();
}
sim::OrderEnvelope decode(std::string_view text) {
    if (text.size() % 2 || text.size() > save::maxFileBytes * 2) throw std::runtime_error("invalid replay command size");
    save::Bytes bytes;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned byte = 0;
        const auto pair = std::from_chars(text.data() + i, text.data() + i + 2, byte, 16);
        if (pair.ec != std::errc{} || pair.ptr != text.data() + i + 2 || byte > 255)
            throw std::runtime_error("invalid replay command encoding");
        bytes.push_back(static_cast<std::uint8_t>(byte));
    }
    sim::OrderEnvelope envelope;
    save::Archive archive(bytes);
    archive(envelope);
    if (!archive.finish()) throw std::runtime_error("invalid replay command payload");
    return envelope;
}
std::expected<sim::OrderReceipt, sim::OrderError> install(sim::TacticalOrderQueue& queue,
    Command& command, const sim::OrderAdmissionValidator& validator) {
    if (command.receipt) return *command.receipt;
    const auto registered = queue.useReplaySource(command.envelope.sourceId);
    if (!registered) return std::unexpected(registered.error());
    auto result = queue.submit(command.envelope, validator);
    if (result) command.receipt = *result;
    return result;
}
}

void begin(std::string_view mission, std::uint32_t seed) {
    session = Session{};
    const auto record = gate::str<"MC2_COMMAND_REPLAY_RECORD">();
    const auto play = gate::str<"MC2_COMMAND_REPLAY_PLAY">();
    if (record.empty() && play.empty()) return;
    if (!record.empty() && !play.empty()) throw std::runtime_error("choose replay record or play");
    const auto content = gate::str<"MC2_REPLAY_CONTENT">();
    if (content.empty()) throw std::runtime_error("replay requires mission/mod fingerprint (tools/command_replay.py)");
    if (!record.empty()) {
        session.record.open(record, std::ios::trunc);
        if (!session.record) throw std::runtime_error("cannot open replay recorder");
        session.record << "MC2REPLAY 1 " << std::quoted(std::string(mission)) << ' ' << seed << ' '
                       << mc2GetSimRngState() << ' ' << mc2_anim_rng::state() << ' ' << std::quoted(content) << '\n' << std::flush;
        return;
    }
    std::ifstream file(play); // path-audit: loose-only (explicit replay artifact, outside the content VFS)
    std::string magic, recordedMission, recordedContent;
    unsigned version = 0;
    std::uint32_t recordedSeed = 0, initialSim = 0, initialAnim = 0;
    file >> magic >> version >> std::quoted(recordedMission) >> recordedSeed >> initialSim >> initialAnim >> std::quoted(recordedContent);
    if (!file || magic != "MC2REPLAY" || version != 1 || recordedMission != mission || recordedSeed != seed ||
        initialSim != mc2GetSimRngState() || initialAnim != mc2_anim_rng::state() || recordedContent != content)
        throw std::runtime_error("replay mission, RNG or content fingerprint mismatch");
    std::string line;
    std::getline(file, line);
    bool ended = false;
    while (std::getline(file, line)) {
        if (ended) throw std::runtime_error("data after replay footer");
        std::istringstream row(line);
        char kind = 0;
        row >> kind;
        if (kind == 'C') {
            Command command;
            std::string bytes;
            row >> command.admitted >> bytes;
            if (!row) throw std::runtime_error("truncated replay command");
            command.envelope = decode(bytes);
            session.commands.push_back(std::move(command));
        } else if (kind == 'H') {
            Checkpoint checkpoint{};
            row >> checkpoint.tick >> std::hex;
            for (auto& value : checkpoint.values) row >> value;
            if (!row || (!session.checkpoints.empty() && checkpoint.tick != session.checkpoints.back().tick + 1))
                throw std::runtime_error("missing replay checkpoint");
            session.checkpoints.push_back(checkpoint);
        } else if (kind == 'E') {
            std::size_t commands = 0, checkpoints = 0;
            row >> commands >> checkpoints;
            if (!row || commands != session.commands.size() || checkpoints != session.checkpoints.size())
                throw std::runtime_error("replay footer count mismatch");
            ended = true;
        } else throw std::runtime_error("invalid replay record");
    }
    if (!ended || session.checkpoints.empty()) throw std::runtime_error("incomplete replay recording");
    session.playback = true;
}
bool active() { return !session.complete && (session.playback || session.record.is_open()); }
bool playing() { return session.playback && !session.complete; }
bool complete() { return session.complete; }
bool failed() { return session.failed; }

std::expected<sim::OrderReceipt, sim::OrderError> submit(sim::TacticalOrderQueue& queue,
    sim::OrderEnvelope envelope, const sim::OrderAdmissionValidator& validator) {
    if (playing()) {
        for (auto& command : session.commands)
            if (command.envelope.sourceId == envelope.sourceId && command.envelope.sequence == envelope.sequence)
                return install(queue, command, validator);
        return std::unexpected(sim::OrderError::Permission);
    }
    const auto result = queue.submitNext(envelope, validator);
    if (result && session.record.is_open()) {
        envelope.applyTick = result->applyTick;
        session.record << "C " << queue.executingTick().value_or(queue.completedTick()) << ' ' << encode(envelope) << '\n' << std::flush;
        ++session.recordedCommands;
    }
    return result;
}
void pump(sim::TacticalOrderQueue& queue, const sim::OrderAdmissionValidator& validator) {
    if (!playing()) return;
    for (auto& command : session.commands) {
        if (command.receipt || command.admitted > queue.completedTick()) continue;
        if (!install(queue, command, validator)) throw std::runtime_error("replay command admission failed");
    }
}
void hashes(std::uint64_t tick, const std::array<std::uint64_t, 5>& values) {
    if (session.record.is_open()) {
        session.record << "H " << std::dec << tick << std::hex;
        for (auto value : values) session.record << ' ' << value;
        session.record << '\n' << std::dec << std::flush;
        if (++session.recordedHashes >= static_cast<std::size_t>(gate::get<"MC2_COMMAND_REPLAY_TICKS">())) {
            session.record << "E " << session.recordedCommands << ' ' << session.recordedHashes << '\n' << std::flush;
            session.record.close();
            session.complete = true;
            std::fprintf(stderr, "[REPLAY_RECORDED v1] tick=%llu commands=%zu checkpoints=%zu\n",
                static_cast<unsigned long long>(tick), session.recordedCommands, session.recordedHashes);
            std::fflush(stderr);
        }
    }
    if (!playing() || session.failed || session.complete) return;
    const auto& expected = session.checkpoints[session.nextHash];
    if (tick != expected.tick) {
        session.failed = true;
        std::fprintf(stderr, "[REPLAY_DIVERGED v1] tick=%llu subsystem=clock expected=%llu actual=%llu\n",
            static_cast<unsigned long long>(expected.tick), static_cast<unsigned long long>(expected.tick),
            static_cast<unsigned long long>(tick));
        std::fflush(stderr);
        return;
    }
    for (std::size_t i = 0; i < values.size(); ++i) if (values[i] != expected.values[i]) {
        session.failed = true;
        std::fprintf(stderr, "[REPLAY_DIVERGED v1] tick=%llu subsystem=%s expected=%016llx actual=%016llx\n",
            static_cast<unsigned long long>(tick), names[i], static_cast<unsigned long long>(expected.values[i]),
            static_cast<unsigned long long>(values[i]));
        std::fflush(stderr);
        return;
    }
    if (++session.nextHash == session.checkpoints.size()) {
        session.complete = true;
        std::fprintf(stderr, "[REPLAY_COMPLETE v1] tick=%llu commands=%zu checkpoints=%zu\n",
            static_cast<unsigned long long>(tick), session.commands.size(), session.checkpoints.size());
        std::fflush(stderr);
    }
}
}
