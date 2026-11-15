#include "runtime.h"
#include "user_paths.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace mc2::net {
namespace {
bool enabled = false, hosting = false;
int status = 0;
std::string endpoint;
std::filesystem::path executable;
std::unique_ptr<Session> session;
std::unique_ptr<Lockstep> gate;
std::vector<sim::ObjectRef> roster;
std::ofstream trace;
sim::TacticalOrderQueue* orderQueue = nullptr;
std::unique_ptr<sim::OrderAdmissionValidator> admission;
PlayerSubmit playerSubmit;
std::chrono::steady_clock::time_point began;
std::chrono::steady_clock::time_point lastProgress;
struct ScriptOrder { sim::Tick tick; std::uint32_t mover; std::string verb; float x = 0, y = 0; };
std::vector<ScriptOrder> scripted;
std::size_t nextScript = 0;
std::uint64_t forcedDesyncTick = 0;
std::uint32_t delayMs = 0;
struct DelayedPacket { std::chrono::steady_clock::time_point due; save::Bytes bytes; };
std::deque<DelayedPacket> delayed;
[[noreturn]] void fail(std::string_view reason) {
    std::fprintf(stderr, "Multiplayer stopped: %.*s\n", static_cast<int>(reason.size()), reason.data());
    if (trace) { trace << "STOP " << reason << '\n' << std::flush; }
    std::fflush(nullptr);
    status = 3;
    throw SessionStopped{};
}
save::Digest executableHash() {
    std::ifstream file(executable, std::ios::binary);
    if (!file) fail("cannot hash MP executable");
    save::Bytes bytes((std::istreambuf_iterator<char>(file)), {});
    if (!file.eof() && file.fail()) fail("cannot read MP executable");
    return save::sha256(bytes);
}
save::Digest contentHash() {
    // Content-addressed tree: paths, lengths and fixed 1 MiB chunk digests.
    // Bound memory even for multi-gigabyte packed stock content.
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(".")) {
        const auto ext = entry.path().extension().string();
        const auto name = entry.path().filename().string();
        if (entry.is_regular_file() && (ext == ".fst" || ext == ".pak" || name == "mods.cfg" || name == "mods_active.cfg"))
            files.push_back(entry.path().lexically_normal());
    }
    constexpr std::array extensions{".pak", ".fit", ".csv", ".abl", ".abx", ".abi", ".ini", ".json", ".cfg", ".ase", ".glb", ".gltf", ".tgl", ".agl"};
    for (const auto& directory : {"data", "mods"}) {
        if (!std::filesystem::exists(directory)) continue;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
            if (entry.is_regular_file() && std::ranges::find(extensions, entry.path().extension().string()) != extensions.end())
                files.push_back(entry.path().lexically_normal());
    }
    std::ranges::sort(files);
    if (files.empty()) fail("MP content manifest is empty");
    save::Archive manifest;
    save::Bytes buffer(1024 * 1024);
    for (const auto& path : files) {
        auto name = path.generic_string();
        auto length = static_cast<std::uint64_t>(std::filesystem::file_size(path));
        std::vector<std::uint8_t> nameBytes(name.begin(), name.end());
        manifest(nameBytes, length);
        std::ifstream file(path, std::ios::binary);
        if (!file) fail("cannot hash MP content: " + name);
        while (file) {
            file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            if (file.gcount()) {
                auto digest = save::sha256(std::span(buffer).first(static_cast<std::size_t>(file.gcount())));
                manifest(digest);
            }
        }
        if (!file.eof()) fail("cannot read MP content: " + name);
    }
    if (!manifest.good()) fail("MP content manifest exceeds limit");
    return save::sha256(manifest.bytes());
}
void flushOutgoing() {
    const auto now = std::chrono::steady_clock::now();
    for (auto& packet : gate->takeOutgoing()) delayed.push_back({now + std::chrono::milliseconds(delayMs), std::move(packet)});
    while (!delayed.empty() && delayed.front().due <= now) {
        session->send(delayed.front().bytes);
        delayed.pop_front();
    }
}
void checkError() {
    if (!session->error().empty()) gate->stop(session->error());
    if (!gate->error().empty()) {
        if (const auto& d = gate->divergence()) {
            // Deliver our final hash even when the test shim is holding outgoing traffic.
            while (!delayed.empty()) { session->send(delayed.front().bytes); delayed.pop_front(); }
            trace << "[MP_DESYNC] tick=" << d->tick << " local=";
            for (const auto h : d->local) trace << std::hex << h << ' ';
            trace << "remote=";
            for (const auto h : d->remote) trace << std::hex << h << ' ';
            trace << std::dec << '\n' << std::flush;
            std::fprintf(stderr, "Multiplayer desync: tick=%llu trace=%s\n",
                static_cast<unsigned long long>(d->tick), userFile("session.trace").c_str());
        }
        fail(gate->error());
    }
}
void scriptOrders() {
    while (nextScript < scripted.size() && scripted[nextScript].tick <= gate->completed()) {
        const auto& row = scripted[nextScript++];
        if (row.mover >= roster.size()) fail("script mover index out of range");
        sim::OrderEnvelope e;
        e.recipients = {roster[row.mover]};
        e.payload.origin = sim::OrderOrigin::Player;
        e.payload.kind = row.verb == "move" ? sim::OrderKind::MoveToPoint : sim::OrderKind::Stop;
        if (row.verb == "reject") {
            if (owns(roster[row.mover], localSlot()) || submitScriptOrder(e, playerSubmit)) fail("ownership rejection fixture failed");
            trace << "OWNERSHIP slot=" << localSlot() << " mover=" << row.mover << " rejected\n" << std::flush;
            continue;
        }
        if (!owns(roster[row.mover], localSlot())) fail("script commands an unowned mover");
        if (row.verb == "move") {
            auto& move = e.payload.move.emplace();
            move.wayPath.points = {{row.x, row.y, 0}};
            move.wayPath.mode = {sim::TravelMode::Slow};
        }
        if (!submitScriptOrder(std::move(e), playerSubmit)) fail("script order could not enter mission order path");
    }
}
}
void configure(int argc, char** argv) {
    executable = std::filesystem::absolute(argv[0]);
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--host" || arg == "--join") {
            if (enabled) throw std::runtime_error("choose --host or --join");
            enabled = true; hosting = arg == "--host";
            if (i + 1 < argc && argv[i + 1][0] != '-') endpoint = argv[++i];
            else if (!hosting) throw std::runtime_error("--join requires IP:port");
        } else if (arg == "--mp-user-dir") {
            if (i + 1 == argc) throw std::runtime_error("missing MP argument value");
            userDirectory = std::filesystem::absolute(argv[++i]);
        }
    }
    if (!enabled) {
        if (!userDirectory.empty()) throw std::runtime_error("MP arguments require --host or --join");
        return;
    }
    if (userDirectory.empty()) throw std::runtime_error("MP requires a unique --mp-user-dir");
    std::filesystem::create_directories(userDirectory);
    // Immutable startup settings are copied once; all subsequent preference writes are private.
    for (const auto* name : {"prefs.cfg", "options.cfg", "orgprefs.cfg"})
        if (std::filesystem::exists(name) && !std::filesystem::exists(userFile(name)))
            std::filesystem::copy_file(name, userFile(name));
    if (std::filesystem::exists(userFile("orders.txt"))) {
        std::ifstream file(userFile("orders.txt"));
        if (!file) throw std::runtime_error("cannot open MP orders fixture");
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream input(line);
            ScriptOrder row{};
            if (!(input >> row.tick >> row.mover >> row.verb)) throw std::runtime_error("invalid MP script row");
            if (row.verb == "desync") { forcedDesyncTick = row.tick; continue; }
            if (row.verb == "delay") {
                if (!(input >> delayMs) || delayMs > 250) throw std::runtime_error("MP shim delay must be 0..250 ms");
                continue;
            }
            if ((row.verb != "move" && row.verb != "stop" && row.verb != "reject") ||
                (row.verb == "move" && !(input >> row.x >> row.y)) || (!scripted.empty() && row.tick < scripted.back().tick))
                throw std::runtime_error("invalid MP script (tick mover move x y | tick mover stop)");
            scripted.push_back(row);
        }
    }
}
void redirectLogs() {
    if (!enabled) return;
    if (!std::freopen(userFile("stdout.log").c_str(), "w", stdout) ||
        !std::freopen(userFile("stderr.log").c_str(), "w", stderr)) throw std::runtime_error("cannot isolate MP logs");
    setvbuf(stdout, nullptr, _IONBF, 0); setvbuf(stderr, nullptr, _IONBF, 0);
}
bool active() { return enabled; }
int exitStatus() { return status; }
[[noreturn]] void stop(std::string_view reason) { fail(reason); }
std::expected<sim::OrderReceipt, sim::OrderError> admitAgreedOrder(sim::TacticalOrderQueue& queue,
    sim::OrderEnvelope order, const sim::OrderAdmissionValidator& validator) {
    // Lockstep has already authenticated the wire sequence, including rejected orders.
    // A rejected admission must not poison every subsequent command from this peer.
    const auto source = std::ranges::find(queue.sources(), order.sourceId, &sim::OrderSource::sourceId);
    if (source != queue.sources().end()) order.sequence = source->nextSequence;
    return queue.submit(std::move(order), validator);
}
std::uint32_t localSlot() { return hosting ? 0u : 1u; }
void beginMission(std::string_view mission, std::uint32_t seed) {
    if (!enabled) return;
    if (session) fail("MP mission replacement is not supported");
    trace.open(userFile("session.trace"), std::ios::trunc);
    if (!trace) fail("cannot open MP trace");
    Identity identity{executableHash(), contentHash(), std::string(mission), seed};
    trace << "IDENTITY executable=";
    for (const auto byte : identity.executable) trace << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    trace << " content=";
    for (const auto byte : identity.content) trace << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    trace << std::dec << " mission=" << mission << " seed=" << seed << '\n' << std::flush;
    session = std::make_unique<Session>();
    try { session->open(hosting, endpoint, identity); }
    catch (const std::exception& error) { fail(error.what()); }
    began = std::chrono::steady_clock::now();
    lastProgress = began;
    std::fprintf(stderr, "Multiplayer: slot=%u mission=%s seed=%u awaiting peer\n", localSlot(), identity.mission.c_str(), seed);
}
void setRoster(std::vector<sim::ObjectRef> initial) { roster = std::move(initial); }
bool owns(sim::ObjectRef ref, std::uint32_t slot) {
    const auto found = std::ranges::find(roster, ref);
    return moverOwner(static_cast<std::uint32_t>(found - roster.begin()), static_cast<std::uint32_t>(roster.size()), found != roster.end()) == slot;
}
bool submitLocal(sim::OrderEnvelope envelope) {
    for (auto ref : envelope.recipients) if (!owns(ref, localSlot())) return false;
    return session && session->connected() && gate && gate->queue(std::move(envelope));
}
bool submitScriptOrder(sim::OrderEnvelope envelope, const PlayerSubmit& submit) {
    return submit && submit(std::move(envelope));
}
void bindQueue(sim::TacticalOrderQueue& queue, std::unique_ptr<sim::OrderAdmissionValidator> validator, PlayerSubmit submit) {
    orderQueue = &queue; admission = std::move(validator); playerSubmit = std::move(submit);
}
std::uint64_t nextTick() { return gate ? gate->completed() + 1 : 1; }
bool prepareTick(bool due) {
    if (!session) fail("MP mission session was not initialized");
    if (!orderQueue || !admission) fail("MP order queue was not bound");
    auto& queue = *orderQueue;
    const auto& validator = *admission;
    const auto packets = session->pump();
    if (!session->error().empty()) fail(session->error());
    if (!gate && session->connected()) {
        gate = std::make_unique<Lockstep>(localSlot(), session->inputDelay());
        lastProgress = std::chrono::steady_clock::now();
        trace << "NETWORK rtt_ms=" << session->rtt() << " jitter_ms=" << session->jitter()
              << " input_delay=" << session->inputDelay() << '\n' << std::flush;
    }
    for (const auto& packet : packets) {
        if (!gate) fail("MP data before timing agreement");
        gate->receive(packet);
    }
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - (gate ? lastProgress : began)).count();
    if (const auto* timeout = progressTimeout(bool(gate), static_cast<std::uint64_t>(age))) fail(timeout);
    if (!gate) return false;
    checkError();
    if (queue.completedTick() != gate->completed()) fail("MP order queue tick mismatch");
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
        const auto id = peerSourceBase + slot;
        if (std::ranges::find(queue.sources(), id, &sim::OrderSource::sourceId) == queue.sources().end() &&
            !queue.registerSource({id, sim::SourceKind::Peer})) fail("cannot register MP order source");
    }
    scriptOrders();
    gate->seal();
    flushOutgoing();
    if (!due || !gate->ready()) return false;
    for (auto& order : gate->beginTick()) {
        save::Archive bytes;
        bytes(order);
        trace << "C " << order.applyTick << ' ';
        for (auto byte : bytes.bytes()) trace << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
        trace << std::dec << '\n' << std::flush;
        const auto tick = order.applyTick, sequence = order.sequence;
        const auto source = order.sourceId;
        if (const auto result = admitAgreedOrder(queue, std::move(order), validator); !result) {
            constexpr std::array names{"Shape", "UnknownSource", "Permission", "Duplicate", "Sequence",
                "Capacity", "Late", "Overflow", "TickContext", "UnknownReceipt", "AlreadyApplied", "Pending", "ApplicationResult"};
            const auto error = std::to_underlying(result.error());
            trace << "R " << tick << ' ' << source << ' ' << sequence << ' ' << names[error] << '(' << error << ")\n" << std::flush;
            std::fprintf(stderr, "Multiplayer order rejected: tick=%llu source=%u sequence=%llu error=%s(%d)\n",
                static_cast<unsigned long long>(tick), source, static_cast<unsigned long long>(sequence), names[error], error);
        }
    }
    return true;
}
void stateHashes(std::uint64_t tick, const StateHash& values) {
    if (!enabled) return;
    if (!gate || tick != gate->completed() + 1) fail("MP state hash tick mismatch");
    auto wireHash = values;
    if (tick == forcedDesyncTick) {
        wireHash[0] ^= 1;
        trace << "FORCE_DESYNC tick=" << tick << '\n' << std::flush;
    }
    trace << "H " << tick;
    for (auto h : wireHash) trace << ' ' << std::hex << h;
    trace << std::dec << '\n' << std::flush;
    gate->finishTick(wireHash);
    lastProgress = std::chrono::steady_clock::now();
    if (tick % 300 == 0) trace << "NETWORK tick=" << tick << " rtt_ms=" << session->rtt()
        << " jitter_ms=" << session->jitter() << '\n' << std::flush;
    flushOutgoing();
    checkError();
}
}
