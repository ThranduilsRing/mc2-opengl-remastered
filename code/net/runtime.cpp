#include "runtime.h"
#include "user_paths.h"
#include "Gates/render_options.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>
#ifdef MC2_NET_RUNTIME_TESTS
#include "doctest.h"
#endif

namespace mc2::net {
namespace {
bool enabled = false, hosting = false;
int status = 0;
std::string endpoint;
std::filesystem::path executable;
std::unique_ptr<Session> session;
std::unique_ptr<Session> lobbyTransport;
LobbyState lobby;
Identity missionIdentity, lobbyIdentity;
bool missionHandshakePending = false, fromLobby = false;
MissionOrigin origin = MissionOrigin::None;
bool missionRunning = false, coopOpen = false;
std::vector<save::Bytes> lobbyPending;
std::chrono::steady_clock::time_point lobbyBegan;
std::unique_ptr<Lockstep> gate;
std::vector<sim::ObjectRef> roster;
std::vector<std::uint32_t> rosterWatchIds;
std::ofstream trace;
sim::TacticalOrderQueue* orderQueue = nullptr;
std::unique_ptr<sim::OrderAdmissionValidator> admission;
PlayerSubmit playerSubmit;
TargetLookup targetLookup;
std::chrono::steady_clock::time_point began;
std::chrono::steady_clock::time_point lastProgress;
struct ScriptOrder { sim::Tick tick; std::uint32_t mover; std::string verb; float x = 0, y = 0; std::uint32_t target = 0; };
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
    std::ifstream file(executable, std::ios::binary);  // path-audit: loose-only (hash of the running exe)
    if (!file) fail("cannot hash MP executable");
    save::Bytes bytes((std::istreambuf_iterator<char>(file)), {});
    if (!file.eof() && file.fail()) fail("cannot read MP executable");
    return save::sha256(bytes);
}
save::Digest contentHash(std::vector<Identity::Component>& components) {
    // Inherited limitation: this disk-tree digest is not the mounted VFS identity.
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
    std::map<std::string, save::Archive> groups;
    std::ofstream listing(userFile("content-digests.tsv"), std::ios::trunc);
    if (!listing) fail("cannot write MP content diagnostics");
    save::Bytes buffer(1024 * 1024);
    for (const auto& path : files) {
        auto name = path.generic_string();
        auto length = static_cast<std::uint64_t>(std::filesystem::file_size(path));
        std::vector<std::uint8_t> nameBytes(name.begin(), name.end());
        manifest(nameBytes, length);
        const auto slash = name.find('/');
        const auto boundary = slash == std::string::npos ? slash : name.find('/', slash + 1);
        auto& group = groups[name.substr(0, boundary)];
        group(nameBytes, length);
        save::Archive fileManifest;
        fileManifest(length);
        std::ifstream file(path, std::ios::binary);  // path-audit: loose-only (byte hash of on-disk content)
        if (!file) fail("cannot hash MP content: " + name);
        while (file) {
            file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            if (file.gcount()) {
                auto digest = save::sha256(std::span(buffer).first(static_cast<std::size_t>(file.gcount())));
                manifest(digest);
                group(digest);
                fileManifest(digest);
            }
        }
        if (!file.eof()) fail("cannot read MP content: " + name);
        listing << name << '\t' << length << '\t' << digestText(save::sha256(fileManifest.bytes())) << '\n';
    }
    for (const auto& [name, group] : groups) {
        if (!group.good()) fail("MP content component exceeds limit");
        components.push_back({name, save::sha256(group.bytes())});
    }
    save::Archive options;
    auto lowCost = mc2::options::lowCost();
    auto buildings = mc2::options::current().remasteredBuildings;
    options(lowCost, buildings);
    components.push_back({"render-options", save::sha256(options.bytes())});
    if (!manifest.good()) fail("MP content manifest exceeds limit");
    return contentIdentity(save::sha256(manifest.bytes()), mc2::options::lowCost(),
        mc2::options::current().remasteredBuildings);
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
        if (row.verb == "attack") {
            e.payload.kind = sim::OrderKind::AttackObject;
            if (!targetLookup) fail("script target lookup unavailable");
            e.payload.target = targetLookup(row.target);
            if (!e.payload.target) fail("script attack target unavailable");
            e.payload.attack.emplace().pursue = true;
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
    if (!enabled && userDirectory.empty()) return;
    if (userDirectory.empty()) throw std::runtime_error("MP requires a unique --mp-user-dir");
    std::filesystem::create_directories(userDirectory);
    // Immutable startup settings are copied once; all subsequent preference writes are private.
    for (const auto* name : {"prefs.cfg", "options.cfg", "orgprefs.cfg"})
        if (std::filesystem::exists(name) && !std::filesystem::exists(userFile(name)))
            std::filesystem::copy_file(name, userFile(name));
    if (std::filesystem::exists(userFile("orders.txt"))) {
        std::ifstream file(userFile("orders.txt"));  // path-audit: loose-only (MP proof user dir)
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
            if ((row.verb != "move" && row.verb != "stop" && row.verb != "reject" && row.verb != "attack") ||
                (row.verb == "move" && !(input >> row.x >> row.y)) ||
                (row.verb == "attack" && !(input >> row.target)) || (!scripted.empty() && row.tick < scripted.back().tick))
                throw std::runtime_error("invalid MP script (tick mover move x y | stop | attack target-part)");
            scripted.push_back(row);
        }
    }
}
void redirectLogs() {
    if (userDirectory.empty()) return;
    if (!std::freopen(userFile("stdout.log").c_str(), "w", stdout) ||
        !std::freopen(userFile("stderr.log").c_str(), "w", stderr)) throw std::runtime_error("cannot isolate MP logs");
    setvbuf(stdout, nullptr, _IONBF, 0); setvbuf(stderr, nullptr, _IONBF, 0);
}
bool active() { return enabled; }
MissionOrigin missionOrigin() { return origin; }
bool& lobbyVisible() { return coopOpen; }
void endMission() {
    // takeLobbyStart precedes destruction of the empty front-end Mission object.
    if (!missionRunning) return;
    session.reset();
    gate.reset();
    enabled = hosting = false;
    endpoint.clear();
    orderQueue = nullptr;
    delayed.clear();
    admission.reset();
    playerSubmit = {};
    targetLookup = {};
    roster.clear();
    rosterWatchIds.clear();
    missionIdentity = lobbyIdentity = {};
    missionHandshakePending = fromLobby = false;
    closeLobby();
    lobbyBegan = began = lastProgress = {};
    scripted.clear();
    nextScript = forcedDesyncTick = delayMs = 0;
    if (trace.is_open()) trace.close();
    trace.clear();
    coopOpen = missionRunning = false;
    origin = MissionOrigin::None;
}
void closeLobby() {
    lobbyTransport.reset();
    lobbyPending.clear();
    lobby = {};
}
const LobbyState& lobbyState() { return lobby; }
bool lobbyConnected() { return lobbyTransport && lobbyTransport->connected() && lobby.error.empty(); }
void openLobby(bool host, std::string_view address, std::string_view mission) {
    closeLobby();
    lobby.hosting = host;
    lobby.mission = mission;
    try {
        if (enabled) throw std::runtime_error("MP mission already active");
        if (userDirectory.empty()) {
            const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            auto directory = std::filesystem::absolute("mp-sessions/" + std::to_string(stamp)).string();
            auto exe = executable.string();
            std::string option = "--mp-user-dir";
            char* args[] = {exe.data(), option.data(), directory.data()};
            configure(3, args);
        }
        Identity identity{executableHash(), {}, "lobby", 0};
        identity.content = contentHash(identity.components);
        lobbyIdentity = identity;
        lobbyTransport = std::make_unique<Session>();
        lobbyTransport->open(host, address, std::move(identity));
        lobbyBegan = std::chrono::steady_clock::now();
    } catch (const std::exception& error) { lobby.error = error.what(); }
    catch (const SessionStopped&) { lobby.error = "MP content identity could not be read"; status = 0; }
}
void pumpLobby() {
    if (!lobbyTransport || !lobby.error.empty() || lobby.started) return;
    for (const auto& packet : lobbyTransport->pump()) {
        if (lobby.started) lobbyPending.push_back(packet);
        else lobby.receive(packet);
    }
    if (!lobbyTransport->error().empty()) lobby.error = lobbyTransport->error();
    if (lobbyConnected() && lobby.hosting && !lobby.offered) lobbyTransport->send(lobby.offer());
    if (!lobbyConnected() && std::chrono::steady_clock::now() - lobbyBegan >= std::chrono::seconds(600))
        lobby.error = "MP loading timeout (600 seconds)";
    if (!lobby.error.empty()) {
        lobby.offered = lobby.localReady = lobby.peerReady = lobby.started = false;
    }
}
void lobbyReady(bool value) {
    if (!lobbyConnected()) return;
    const auto packet = lobby.ready(value);
    if (!packet.empty()) lobbyTransport->send(packet);
}
void lobbyStart() {
    if (!lobbyConnected()) return;
    const auto packet = lobby.start();
    if (!packet.empty()) lobbyTransport->send(packet);
}
std::string takeLobbyStart() {
    if (!lobby.started || !lobbyTransport || !lobby.error.empty()) return {};
    session = std::move(lobbyTransport);
    session->missionLoading(true);
    hosting = lobby.hosting;
    enabled = fromLobby = true;
    origin = MissionOrigin::Lobby;
    coopOpen = false;
    auto mission = std::move(lobby.mission);
    lobby = {};
    return mission;
}
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
    if (!enabled) { origin = MissionOrigin::SinglePlayer; missionRunning = true; return; }
    if (missionRunning) fail("MP mission already active");
    missionRunning = true;
    if (!fromLobby) origin = MissionOrigin::CommandLine;
    // One overall rendezvous cap, including local cold loading and identity work.
    began = lastProgress = std::chrono::steady_clock::now();
    if (session && !fromLobby) fail("MP mission replacement is not supported");
    trace.open(userFile("session.trace"), std::ios::trunc);
    if (!trace) fail("cannot open MP trace");
    Identity identity = fromLobby ? lobbyIdentity : Identity{executableHash(), {}, {}, 0};
    identity.mission = mission;
    identity.seed = seed;
    if (!fromLobby) identity.content = contentHash(identity.components);
    for (const auto& component : identity.components)
        trace << "CONTENT " << component.name << ' ' << digestText(component.digest) << '\n';
    trace << "IDENTITY executable=";
    for (const auto byte : identity.executable) trace << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    trace << " content=";
    for (const auto byte : identity.content) trace << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    trace << std::dec << " mission=" << mission << " seed=" << seed << '\n' << std::flush;
    if (fromLobby) {
        missionIdentity = identity;
        missionHandshakePending = true;
        session->send(hello(identity));
        fromLobby = false;
    } else {
        session = std::make_unique<Session>();
        try { session->open(hosting, endpoint, identity); }
        catch (const std::exception& error) { fail(error.what()); }
        session->missionLoading(true);
    }
    std::fprintf(stderr, "Multiplayer: slot=%u mission=%s seed=%u awaiting peer\n", localSlot(), identity.mission.c_str(), seed);
}
void setRoster(std::vector<sim::ObjectRef> initial, std::vector<std::uint32_t> watchIds) {
    if (initial.size() != watchIds.size()) fail("MP roster identity mismatch");
    roster = std::move(initial); rosterWatchIds = std::move(watchIds);
}
void combatEvent(std::string_view event, std::uint32_t attacker, std::uint32_t target, float damage) {
    if (!enabled || !gate || !trace) return;
    const auto found = std::ranges::find(rosterWatchIds, attacker);
    if (found == rosterWatchIds.end()) return;
    const auto index = static_cast<std::uint32_t>(found - rosterWatchIds.begin());
    trace << "B " << nextTick() << ' ' << moverOwner(index, static_cast<std::uint32_t>(roster.size()), true)
          << ' ' << event << ' ' << attacker << ' ' << target << ' ' << damage << '\n' << std::flush;
}
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
void bindQueue(sim::TacticalOrderQueue& queue, std::unique_ptr<sim::OrderAdmissionValidator> validator, PlayerSubmit submit, TargetLookup target) {
    orderQueue = &queue; admission = std::move(validator); playerSubmit = std::move(submit);
    targetLookup = std::move(target);
}
std::uint64_t nextTick() { return gate ? gate->completed() + 1 : 1; }
bool prepareTick(bool due) {
    if (!session) fail("MP mission session was not initialized");
    // This first main-thread tick is the local LOADED milestone; it never resets the cap.
    session->missionLoading(false);
    if (!gate) {
        const auto overallAge = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - began).count();
        if (const auto* timeout = progressTimeout(false, static_cast<std::uint64_t>(overallAge))) fail(timeout);
    }
    if (!orderQueue || !admission) fail("MP order queue was not bound");
    auto& queue = *orderQueue;
    const auto& validator = *admission;
    auto packets = std::move(lobbyPending);
    lobbyPending.clear();
    auto incoming = session->pump();
    packets.insert(packets.end(), std::make_move_iterator(incoming.begin()), std::make_move_iterator(incoming.end()));
    const bool hadGate = gate != nullptr;
    for (const auto& packet : packets) {
        if (missionHandshakePending) {
            const auto error = checkHello(packet, missionIdentity);
            if (!error.empty()) fail(error);
            missionHandshakePending = false;
        } else {
            if (!gate) {
                if (!session->connected()) fail("MP input before LOADED barrier");
                gate = std::make_unique<Lockstep>(localSlot(), session->inputDelay());
            }
            gate->receive(packet);
        }
    }
    if (!gate && session->connected() && !missionHandshakePending) {
        gate = std::make_unique<Lockstep>(localSlot(), session->inputDelay());
    }
    if (gate && !hadGate) {
        lastProgress = std::chrono::steady_clock::now();
        trace << "LOADED both peers; rendezvous_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(lastProgress - began).count() << '\n';
        trace << "NETWORK rtt_ms=" << session->rtt() << " jitter_ms=" << session->jitter()
              << " input_delay=" << session->inputDelay() << " measured_rtt_ms=" << session->measuredRtt()
              << " measured_jitter_ms=" << session->measuredJitter() << '\n' << std::flush;
    }
    if (!gate && !session->error().empty()) fail(session->error());
    // A final divergent hash and disconnect can arrive in the same pump.
    // Preserve the first divergent tick before reporting transport failure or timeout.
    if (gate) checkError();
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - (gate ? lastProgress : began)).count();
    if (const auto* timeout = progressTimeout(gate != nullptr, static_cast<std::uint64_t>(age))) fail(timeout);
    if (!gate) return false;
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
bool waitingForPeer() {
    // Runtime and UI state are main-thread-only; the transport exposes copied snapshots.
    return enabled && session && (!gate || std::chrono::steady_clock::now() - lastProgress >= std::chrono::milliseconds(500));
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
#ifdef MC2_NET_RUNTIME_TESTS
namespace {
struct RecordedSession final : Session {
    std::vector<save::Bytes>& sent;
    std::vector<save::Bytes> incoming;
    std::string fault;
    explicit RecordedSession(std::vector<save::Bytes>& output) : sent(output) {}
    void send(std::span<const std::uint8_t> bytes) override { sent.emplace_back(bytes.begin(), bytes.end()); }
    std::vector<save::Bytes> pump() override { return std::exchange(incoming, {}); }
    bool connected() const override { return fault.empty(); }
    void missionLoading(bool) override {}
    std::string error() const override { return fault; }
};
struct RuntimeAdmission final : sim::OrderAdmissionValidator {
    bool permitted(const sim::OrderSource&, const sim::OrderEnvelope&) const override { return true; }
};
}
void testLobbyPeerLoss() {
    std::vector<save::Bytes> sent;
    auto transport = std::make_unique<RecordedSession>(sent);
    transport->fault = "MP peer disconnected";
    lobbyTransport = std::move(transport);
    lobby.offered = lobby.localReady = lobby.peerReady = true;
    pumpLobby();
    CHECK(lobby.error == "MP peer disconnected");
    CHECK_FALSE(lobby.offered);
    CHECK_FALSE(lobby.localReady);
    CHECK_FALSE(lobby.peerReady);
    CHECK_FALSE(lobbyConnected());
    closeLobby();
}
void testMissionLifecycle() {
    const auto previousDirectory = userDirectory;
    userDirectory = std::filesystem::current_path() / "net-runtime-test";
    std::filesystem::create_directories(userDirectory);
    struct Cleanup {
        std::filesystem::path previous;
        ~Cleanup() {
            endMission(); closeLobby();
            std::filesystem::remove_all(userDirectory);
            userDirectory = previous;
        }
    } cleanup{previousDirectory};
    sim::TacticalOrderQueue queue;
    unsigned callbacks = 0;
    std::vector<save::Bytes> firstSent, secondSent;
    auto launch = [&](std::vector<save::Bytes>& sent) {
        lobbyTransport = std::make_unique<RecordedSession>(sent);
        lobby.hosting = lobby.offered = lobby.localReady = lobby.peerReady = lobby.started = true;
        lobby.mission = "mc2_01";
        coopOpen = true;
        REQUIRE(takeLobbyStart() == "mc2_01");
        CHECK_FALSE(lobby.started); CHECK_FALSE(lobby.offered); CHECK_FALSE(coopOpen);
        endMission(); // Destruction of the empty menu Mission must preserve the pending launch.
        REQUIRE(active());
        REQUIRE_NOTHROW(beginMission("mc2_01", 12345));
        CHECK(trace.is_open()); CHECK(trace.good());
        CHECK(missionOrigin() == MissionOrigin::Lobby);
        CHECK_FALSE(fromLobby);
    };
    auto clean = [&] {
        CHECK_FALSE(active()); CHECK_FALSE(session); CHECK_FALSE(gate); CHECK_FALSE(orderQueue);
        CHECK(delayed.empty()); CHECK_FALSE(admission); CHECK_FALSE(playerSubmit); CHECK_FALSE(targetLookup);
        CHECK(roster.empty()); CHECK(rosterWatchIds.empty()); CHECK_FALSE(missionHandshakePending);
        CHECK(lobbyPending.empty()); CHECK_FALSE(lobbyTransport); CHECK_FALSE(coopOpen);
        CHECK_FALSE(trace.is_open()); CHECK(missionOrigin() == MissionOrigin::None);
        CHECK(scripted.empty()); CHECK(nextScript == 0); CHECK(delayMs == 0); CHECK(forcedDesyncTick == 0);
    };
    launch(firstSent);
    bindQueue(queue, std::make_unique<RuntimeAdmission>(), [&](sim::OrderEnvelope) { ++callbacks; return true; },
        [&](std::uint32_t) -> std::optional<sim::ObjectRef> { ++callbacks; return {}; });
    roster = {{1, 1}}; rosterWatchIds = {77};
    const save::Bytes stale{0xde, 0xad, 0xbe, 0xef};
    delayed.push_back({std::chrono::steady_clock::now(), stale});
    lobbyPending.push_back(stale);
    gate = std::make_unique<Lockstep>(0);
    scripted.push_back({0, 0, "reject"});
    delayMs = 250; forcedDesyncTick = 42;
    REQUIRE(missionHandshakePending); REQUIRE(admission); REQUIRE(playerSubmit); REQUIRE(targetLookup);
    REQUIRE_FALSE(delayed.empty()); REQUIRE_FALSE(lobbyPending.empty());
    endMission(); clean();
    beginMission("mc2_01", 12345);
    CHECK(missionOrigin() == MissionOrigin::SinglePlayer); CHECK_FALSE(active());
    endMission(); clean();
    launch(secondSent);
    auto* recording = static_cast<RecordedSession*>(session.get());
    recording->incoming.push_back(hello(missionIdentity));
    bindQueue(queue, std::make_unique<RuntimeAdmission>(), {}, {});
    CHECK_FALSE(prepareTick(false)); // Transmits the new mission's sealed batch through the stub.
    REQUIRE(secondSent.size() >= 2);
    for (const auto& packet : secondSent) CHECK(packet != stale);
    CHECK(callbacks == 0);
    CHECK_FALSE(missionHandshakePending);
    endMission(); clean();
}
#endif
}
