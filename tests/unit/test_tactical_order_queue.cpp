#include "doctest.h"
#include "tactical_order_queue.h"
#include "save_field_probe.h"

#include <algorithm>

namespace mc2::sim {
struct TacticalOrderQueueFieldProbe {
    static void check(TacticalOrderQueue& sample) {
        using T = TacticalOrderQueue;
        SAVE_FIELD_SHAPE(sample, pending, sources, receipts, nextReceiptId, completed,
            executing, applying, nextApplying, applicationActive);
        CHECK(save_probe::memberBytes<T>(&T::pending_, &T::sources_, &T::receipts_,
            &T::nextReceiptId_, &T::completed_, &T::executing_, &T::applying_,
            &T::nextApplying_, &T::applicationActive_) + 7 == sizeof(T));
        save::Archive writer;
        sample.serialize(writer);
        REQUIRE(writer.good());
        // completed_ belongs to the caller's tick boundary; the other non-wire
        // members are idle execution scratch and serialize rejects an active tick.
        T decoded(sample.completed_);
        save::Archive reader(writer.bytes());
        decoded.serialize(reader);
        REQUIRE(reader.finish());
        CHECK(decoded.pending_ == sample.pending_);
        CHECK(decoded.sources_ == sample.sources_);
        CHECK(decoded.receipts_ == sample.receipts_);
        CHECK(decoded.nextReceiptId_ == sample.nextReceiptId_);
    }
};
}

namespace {
using namespace mc2::sim;
using mc2::save::Bytes;
struct Permit : OrderAdmissionValidator {
    bool allowed = true;
    bool permitted(const OrderSource&, const OrderEnvelope&) const override { return allowed; }
};
struct Application : OrderApplication {
    std::uint32_t commandReason = 0, rejectedSlot = 999;
    std::vector<std::uint32_t> appliedSlots;
    std::size_t supportCalls = 0;
    ApplicationOutcome supportOutcome;
    std::uint32_t validateCommand(const OrderEnvelope&) const override { return commandReason; }
    std::uint32_t validateRecipient(const OrderEnvelope&, ObjectRef recipient, std::uint32_t) const override {
        return recipient.slot == rejectedSlot ? 17 : 0;
    }
    ApplicationOutcome applyRecipient(const OrderEnvelope&, ObjectRef recipient, std::uint32_t) override {
        appliedSlots.push_back(recipient.slot); return {};
    }
    ApplicationOutcome applySupport(const OrderEnvelope&) override { ++supportCalls; return supportOutcome; }
};
OrderEnvelope order(std::uint32_t source = 1, std::uint64_t sequence = 0, Tick tick = 1) {
    OrderEnvelope e;
    e.sourceId = source; e.sequence = sequence; e.applyTick = tick;
    e.issuer = CommanderId{7};
    e.recipients = {{30,2}, {10,9}, {20,4}};
    e.payload.orderId = 0x0102030405060708ull;
    e.payload.kind = OrderKind::Stop;
    return e;
}

TEST_CASE("order recipients cover every admitted mover and reject one more") {
    auto envelope = order();
    envelope.recipients.clear();
    for (uint32_t slot = 1; slot <= MAX_MOVERS; ++slot)
        envelope.recipients.push_back({slot, 1});
    mc2::save::Archive write;
    write(envelope);
    REQUIRE(write.good());
    mc2::sim::OrderEnvelope restored;
    REQUIRE(mc2::save::decodeRecord(write.bytes(), restored));
    CHECK(restored == envelope);
    envelope.recipients.push_back({MAX_MOVERS + 1, 1});
    mc2::save::Archive overflow;
    overflow(envelope);
    CHECK_FALSE(overflow.good());
}
Bytes wire(TacticalOrderQueue& queue) {
    mc2::save::Archive a;
    a(queue);
    REQUIRE(a.finish());
    return a.bytes();
}
// The individual codecs leave cross-record consistency to the owning queue.
Bytes ingress(std::vector<OrderEnvelope> pending, std::vector<OrderSource> sources,
              std::vector<OrderReceipt> receipts, std::uint64_t nextId = 1) {
    mc2::save::Archive a;
    auto records = [&](auto& values) {
        auto size = static_cast<std::uint32_t>(values.size());
        a(size);
        for (auto& value : values) value.serialize(a);
    };
    records(pending); records(sources); records(receipts); a(nextId);
    REQUIRE(a.finish());
    return a.bytes();
}
void drain(TacticalOrderQueue& queue, Tick tick, Application& application) {
    auto batch = queue.beginTick(tick);
    REQUIRE(batch);
    for (const auto& e : *batch) {
        const auto receipt = std::ranges::find_if(queue.receipts(), [&](const auto& r) {
            return r.sourceId == e.sourceId && r.sequence == e.sequence;
        });
        REQUIRE(receipt != queue.receipts().end());
        REQUIRE(queue.apply(receipt->receiptId, application));
    }
    REQUIRE(queue.endTick());
}
} // namespace

TEST_SUITE("TacticalOrderQueue") {
TEST_CASE("save fields: ORDQ private state census and continuation image") {
    Permit permit;
    TacticalOrderQueue queue;
    Application application;
    constexpr uint32_t source = 0x01010101, instance = 0x02020202;
    REQUIRE(queue.registerSource({source, SourceKind::ABL, InstanceId{instance}}));
    auto first = order(source, 0, 1);
    first.issuer = InstanceId{instance};
    first.queueMode = QueueMode::Append;
    REQUIRE(queue.submit(first, permit));
    drain(queue, 1, application);
    auto second = first;
    second.sequence = 1;
    second.applyTick = 10;
    REQUIRE(queue.submit(second, permit));
    TacticalOrderQueueFieldProbe::check(queue);
}
TEST_CASE("mission content source codec bounds and permission rules") {
    Permit permit;
    TacticalOrderQueue queue;
    REQUIRE(queue.registerSource({1, SourceKind::MissionContent}));
    CHECK_FALSE(queue.registerSource({2, SourceKind::MissionContent, InstanceId{7}}));
    auto player = order();
    CHECK(queue.submit(player, permit).error() == OrderError::Permission);
    auto authored = order(); authored.payload.origin = OrderOrigin::Commander;
    REQUIRE(queue.submit(authored, permit));
    OrderSource source{9, SourceKind::MissionContent};
    mc2::save::Archive writer;
    writer(source);
    REQUIRE(writer.finish());
    OrderSource decoded;
    REQUIRE(mc2::save::decodeRecord(writer.bytes(), decoded));
    CHECK(decoded.kind == SourceKind::MissionContent);
    source.kind = static_cast<SourceKind>(6);
    mc2::save::Archive invalid([](std::string_view) {});
    invalid(source);
    CHECK_FALSE(invalid.good());
}
TEST_CASE("tactic table: sim enum values and TacAttackParams tactic bytes are the shipped ones") {
    // Literals from the pre-table enum; TacAttackParams wire = type(4) method(4) range(4) tactic(4) ...
    CHECK(static_cast<std::uint32_t>(Tactic::None) == 0);
    CHECK(static_cast<std::uint32_t>(Tactic::FlankLeft) == 1);
    CHECK(static_cast<std::uint32_t>(Tactic::FlankRight) == 2);
    CHECK(static_cast<std::uint32_t>(Tactic::FlankRear) == 3);
    CHECK(static_cast<std::uint32_t>(Tactic::StopAndFire) == 4);
    CHECK(static_cast<std::uint32_t>(Tactic::Turret) == 5);
    CHECK(static_cast<std::uint32_t>(Tactic::Joust) == 6);
    CHECK(static_cast<std::uint32_t>(Tactic::IndirectFire) == 7);
    CHECK(static_cast<std::uint32_t>(Tactic::HullDown) == 8);
    CHECK(static_cast<std::uint32_t>(Tactic::FightingWithdraw) == 9);
    CHECK(static_cast<std::uint32_t>(Tactic::Pursue) == 10);
    CHECK(static_cast<std::uint32_t>(Tactic::HitAndRun) == 11);
    for (std::uint32_t v = 0; v <= 11; ++v) {
        TacAttackParams p;
        p.tactic = static_cast<Tactic>(v);
        mc2::save::Archive a;
        p.serialize(a);
        REQUIRE(a.finish());
        const Bytes b = a.bytes();
        REQUIRE(b.size() >= 16);
        CHECK(b[12] == v);
        CHECK(b[13] == 0); CHECK(b[14] == 0); CHECK(b[15] == 0);
    }
    TacAttackParams bad;
    bad.tactic = static_cast<Tactic>(12);
    mc2::save::Archive a;
    bad.serialize(a);
    CHECK_FALSE(a.finish());
}
TEST_CASE("fixed enum IDs issuer envelope and TacticalOrder wire field order") {
    CHECK(static_cast<std::uint32_t>(OrderKind::AttackObject) == 16);
    CHECK(static_cast<std::uint32_t>(OrderKind::Recover) == 26);
    CHECK(static_cast<std::uint32_t>(OrderKind::MissionControl) == 29);
    CHECK(static_cast<std::uint32_t>(QueueMode::Append) == 2);
    CHECK(static_cast<std::uint32_t>(SourceKind::ABL) == 2);
    auto e = order(0x11223344, 0x1020304050607080ull, 0x0102030405060708ull);
    e.recipients = {{0x12345678,0x87654321}};
    mc2::save::Archive a;
    a(e);
    REQUIRE(a.finish());
    const Bytes expected{
        8,7,6,5,4,3,2,1, 0x44,0x33,0x22,0x11, 0x80,0x70,0x60,0x50,0x40,0x30,0x20,0x10,
        1,7,0,0,0, 1,0,0,0, 0x78,0x56,0x34,0x12,0x21,0x43,0x65,0x87, 1,0,0,0,
        8,7,6,5,4,3,2,1, 0,0,0,0, 11,0,0,0, 0,0, 0,0, 0, 0,0,0,0, 0,0,0,0
    };
    CHECK(a.bytes() == expected);
    OrderEnvelope restored;
    REQUIRE(mc2::save::decodeRecord(a.bytes(), restored));
    CHECK(restored == e);
}

TEST_CASE("canonical ordering roster order strict sequence and duplicate drain") {
    Permit permit; TacticalOrderQueue queue; Application application;
    REQUIRE(queue.registerSource({8,SourceKind::UI}));
    REQUIRE(queue.registerSource({2,SourceKind::UI}));
    CHECK(queue.sources()[0].sourceId == 2);
    REQUIRE(queue.submit(order(8,0,2), permit));
    auto later = queue.submit(order(2,0,1), permit); REQUIRE(later);
    auto first = queue.submit(order(8,1,1), permit); REQUIRE(first);
    REQUIRE(queue.snapshotPending().size() == 3);
    CHECK(queue.snapshotPending()[0].sourceId == 2);
    CHECK(queue.snapshotPending()[1].sourceId == 8);
    CHECK(queue.snapshotPending()[1].sequence == 1);
    auto duplicate = queue.submit(order(8,1,1), permit); REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error() == OrderError::Duplicate);
    auto gap = queue.submit(order(8,3,1), permit); REQUIRE_FALSE(gap);
    CHECK(gap.error() == OrderError::Sequence);
    auto batch = queue.beginTick(1); REQUIRE(batch); REQUIRE(batch->size() == 2);
    CHECK_FALSE(queue.beginTick(1));
    CHECK_FALSE(queue.apply(first->receiptId, application));
    CHECK_FALSE(queue.endTick());
    REQUIRE(queue.apply(later->receiptId, application));
    REQUIRE(queue.apply(first->receiptId, application));
    auto reapplied = queue.apply(first->receiptId, application); REQUIRE_FALSE(reapplied);
    CHECK(reapplied.error() == OrderError::AlreadyApplied);
    CHECK(application.appliedSlots == std::vector<std::uint32_t>{30,10,20,30,10,20});
    REQUIRE(queue.endTick());
    CHECK_FALSE(queue.beginTick(1));
    drain(queue, 2, application);
    CHECK(queue.snapshotPending().empty());
    CHECK(queue.receipts().size() == 3);
    CHECK_FALSE(queue.submitNext(order(8,0), permit));
}

TEST_CASE("between-tick executing-tick late admission and save boundary") {
    Permit permit; TacticalOrderQueue queue(10); Application application;
    REQUIRE(queue.registerSource({1,SourceKind::UI}));
    auto admitted = queue.submitNext(order(), permit); REQUIRE(admitted);
    CHECK(admitted->applyTick == 11);
    REQUIRE(queue.beginTick(11));
    auto during = queue.submitNext(order(1,1), permit); REQUIRE(during);
    CHECK(during->applyTick == 12);
    auto late = queue.submit(order(1,2,11), permit); REQUIRE_FALSE(late);
    CHECK(late.error() == OrderError::Late);
    auto future = queue.submit(order(1,2,15), permit); REQUIRE(future);
    int logs = 0;
    mc2::save::Archive a([&](auto) { ++logs; }); a(queue);
    CHECK_FALSE(a.finish()); CHECK(logs == 1);
    CHECK_FALSE(queue.restore({}, 10, [&](auto) { ++logs; })); CHECK(logs == 2);
    REQUIRE(queue.apply(admitted->receiptId, application)); REQUIRE(queue.endTick());
    TacticalOrderQueue restored;
    REQUIRE(restored.restore(wire(queue), 11));
    CHECK(wire(restored) == wire(queue));
    CHECK(restored.completedTick() == 11); CHECK_FALSE(restored.executingTick());
    drain(restored, 12, application);
    CHECK(restored.snapshotPending().size() == 1);
}

TEST_CASE("application authority handle validation results acknowledgements and sequence persistence") {
    Permit permit; TacticalOrderQueue queue; Application application;
    application.rejectedSlot = 10;
    REQUIRE(queue.registerSource({1,SourceKind::ABL,InstanceId{42}}));
    auto e = order(); e.issuer = InstanceId{41};
    auto forbidden = queue.submitNext(e, permit); REQUIRE_FALSE(forbidden);
    CHECK(forbidden.error() == OrderError::Permission);
    e.issuer = InstanceId{42};
    auto receipt = queue.submitNext(e, permit); REQUIRE(receipt);
    CHECK_FALSE(queue.acknowledge(receipt->receiptId));
    drain(queue, 1, application);
    const auto& result = queue.receipts()[0];
    CHECK(result.applicationStatus == ApplicationStatus::Partial);
    REQUIRE(result.results.size() == 3);
    CHECK(result.results[1].recipient == ObjectRef{10,9});
    CHECK(result.results[1].ordinal == 1); CHECK(result.results[1].reasonCode == 17);
    REQUIRE(queue.acknowledge(receipt->receiptId));
    TacticalOrderQueue restored;
    REQUIRE(restored.restore(wire(queue), 1));
    CHECK(restored.receipts()[0].acknowledged);
    CHECK(restored.receipts()[0].results == result.results);
    restored.retireAcknowledged(receipt->receiptId);
    CHECK(restored.receipts().empty());
    TacticalOrderQueue afterRetirement;
    REQUIRE(afterRetirement.restore(wire(restored), 1));
    CHECK(afterRetirement.sources()[0].nextSequence == 1);
    CHECK(afterRetirement.sources()[0].lastAdmittedSequence == 0);
    CHECK(afterRetirement.nextReceiptId() == 2);
    CHECK_FALSE(afterRetirement.submitNext(e, permit));
    e.sequence = 1;
    REQUIRE(afterRetirement.submitNext(e, permit));
    application.commandReason = 88;
    drain(afterRetirement, 2, application);
    CHECK(afterRetirement.receipts()[0].applicationStatus == ApplicationStatus::Rejected);
    CHECK(afterRetirement.receipts()[0].results[0].reasonCode == 88);
}

TEST_CASE("Guard accepts object and point path goals and rejects missing or malformed goals") {
    Permit permit; Application application;
    for (bool objectTarget : {false, true}) {
        TacticalOrderQueue queue;
        REQUIRE(queue.registerSource({1,SourceKind::UI}));
        auto e = order(); e.payload.kind = OrderKind::Guard; e.payload.move.emplace();
        if (objectTarget) e.payload.target = ObjectRef{4,9};
        else e.payload.move->wayPath = {{{1,2,3}},0,{TravelMode::Fast}};
        REQUIRE(queue.submit(e, permit));
        TacticalOrderQueue restored;
        REQUIRE(restored.restore(wire(queue), 0));
        REQUIRE(restored.snapshotPending().size() == 1);
        CHECK(restored.snapshotPending()[0] == e);
        drain(restored, 1, application);
        CHECK(restored.receipts()[0].applicationStatus == ApplicationStatus::Applied);
    }
    TacticalOrderQueue queue;
    REQUIRE(queue.registerSource({1,SourceKind::UI}));
    const auto original = wire(queue);
    auto bad = order(); bad.payload.kind = OrderKind::Guard; bad.payload.move.emplace();
    CHECK_FALSE(queue.submit(bad, permit));
    bad.payload.move->wayPath = {{{1,2,3}},0,{}};
    CHECK_FALSE(queue.submit(bad, permit));
    bad.payload.move->wayPath.mode = {TravelMode::Fast};
    bad.payload.move->wayPath.curPoint = 2;
    CHECK_FALSE(queue.submit(bad, permit));
    bad.payload.move->wayPath.curPoint = 0;
    bad.payload.move->wayPath.points[0].x = std::numeric_limits<float>::infinity();
    CHECK_FALSE(queue.submit(bad, permit));
    CHECK(wire(queue) == original);
}

TEST_CASE("typed movement attack support and execution records round trip and reject malformed arms") {
    Permit permit; TacticalOrderQueue queue; Application application;
    REQUIRE(queue.registerSource({1,SourceKind::Replay}));
    std::vector<TacticalOrder> payloads;
    auto move = order().payload; move.kind = OrderKind::PatrolPath; move.move.emplace();
    move.move->wayPath = {{{1,2,3},{-0.0f,5,6}},1,{TravelMode::Fast,TravelMode::Jump}};
    move.move->keepMoving = true; move.move->fromArea = -1; payloads.push_back(move);
    auto attack = order().payload; attack.kind = OrderKind::AttackObject;
    attack.attack.emplace(); attack.attack->range = FireRange::Optimal;
    attack.move.emplace(); attack.move->faceObject = true; attack.move->keepMoving = true;
    attack.attack->tactic = Tactic::HullDown; attack.target = ObjectRef{4,9}; payloads.push_back(attack);
    auto support = order().payload; support.kind = OrderKind::Airstrike;
    support.support = AirstrikeCommand{CommanderId{7},{1,2,3},StrikeKind::Large}; payloads.push_back(support);
    support.kind = OrderKind::Reinforcement;
    support.support = ReinforcementCommand{CommanderId{7},{{3},{1},{2}},{5,6,7}}; payloads.push_back(support);
    support.kind = OrderKind::MissionControl;
    support.support = MissionControlCommand{MissionAction::SetResource,ResourceArgument{CommanderId{7},25}};
    payloads.push_back(support);
    support.support = MissionControlCommand{MissionAction::SetObjective,ObjectiveArgument{19,true}};
    payloads.push_back(support);
    support.support = MissionControlCommand{MissionAction::SetMissionResult,MissionResultArgument{MissionResult::Won}};
    payloads.push_back(support);
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        auto e = order(1,i); e.payload = payloads[i]; e.queueMode = QueueMode::Append;
        if (e.payload.support) e.recipients.clear();
        REQUIRE(queue.submit(e, permit));
        mc2::save::Archive a; a(e); REQUIRE(a.finish());
        OrderEnvelope restored; REQUIRE(mc2::save::decodeRecord(a.bytes(), restored)); CHECK(restored == e);
        mc2::save::Archive encodedAgain; encodedAgain(restored);
        REQUIRE(encodedAgain.finish()); CHECK(encodedAgain.bytes() == a.bytes());
    }
    TacticalOrderQueue restored; REQUIRE(restored.restore(wire(queue), 0));
    drain(restored, 1, application); CHECK(application.supportCalls == 5);
    CHECK(restored.receipts().size() == 7);
    OrderExecution execution{attack,1,2,3,ExecutionStage::Active,ExecutionStatus::Running,{4,5,6},ObjectRef{5,2}};
    mc2::save::Archive a; a(execution); REQUIRE(a.finish());
    OrderExecution resumed; REQUIRE(mc2::save::decodeRecord(a.bytes(), resumed)); CHECK(resumed == execution);
    auto bad = order(1,7); bad.payload = move; bad.payload.move->wayPath.mode.pop_back();
    CHECK_FALSE(queue.submit(bad, permit));
    bad.payload = support;
    bad.recipients.clear();
    bad.payload.support = MissionControlCommand{MissionAction::SetObjective,ResourceArgument{CommanderId{7},25}};
    CHECK_FALSE(queue.submit(bad, permit));
    bad.payload.kind = OrderKind::Reinforcement;
    bad.payload.support = ReinforcementCommand{CommanderId{7},{{3},{3}},{1,2,3}};
    CHECK_FALSE(queue.submit(bad, permit));
}

TEST_CASE("support command outcomes and rejection reasons survive save restore and acknowledgement") {
    const std::array<SupportCommand,3> commands{
        AirstrikeCommand{CommanderId{7},{1,2,3},StrikeKind::Large},
        ReinforcementCommand{CommanderId{7},{{3},{1}},{5,6,7}},
        MissionControlCommand{MissionAction::SetResource,ResourceArgument{CommanderId{7},25}}
    };
    for (std::size_t i = 0; i < commands.size(); ++i) {
        for (int scenario = 0; scenario < 6; ++scenario) {
            Permit permit; Application application; TacticalOrderQueue queue;
            REQUIRE(queue.registerSource({1,SourceKind::UI}));
            auto e = order(); e.recipients.clear(); e.payload.support = commands[i];
            e.payload.kind = static_cast<OrderKind>(27 + i);
            if (scenario == 1) application.commandReason = 88;
            if (scenario == 2) application.supportOutcome = {RecipientStatus::Rejected,17};
            if (scenario == 3) application.supportOutcome = {RecipientStatus::Rejected,0};
            if (scenario == 4) application.supportOutcome = {RecipientStatus::Applied,17};
            if (scenario == 5) application.supportOutcome = {static_cast<RecipientStatus>(99),17};
            REQUIRE(queue.submit(e, permit));
            CHECK_FALSE(queue.receipts()[0].commandResult);
            TacticalOrderQueue pending;
            REQUIRE(pending.restore(wire(queue), 0));
            CHECK_FALSE(pending.receipts()[0].commandResult);
            drain(pending, 1, application);
            REQUIRE(pending.receipts()[0].commandResult);
            const ApplicationOutcome expected = scenario == 0 ? ApplicationOutcome{} :
                ApplicationOutcome{RecipientStatus::Rejected,scenario == 1 ? 88u : scenario == 2 ? 17u : 0xffffffffu};
            CHECK(*pending.receipts()[0].commandResult == expected);
            CHECK(pending.receipts()[0].results.empty());
            CHECK(pending.receipts()[0].applicationStatus ==
                  (scenario == 0 ? ApplicationStatus::Applied : ApplicationStatus::Rejected));
            CHECK(application.supportCalls == (scenario == 1 ? 0 : 1));
            const auto saved = wire(pending);
            TacticalOrderQueue restored;
            REQUIRE(restored.restore(saved, 1));
            CHECK(restored.receipts()[0] == pending.receipts()[0]);
            CHECK(wire(restored) == saved);
            REQUIRE(restored.acknowledge(1));
            TacticalOrderQueue acknowledged;
            REQUIRE(acknowledged.restore(wire(restored), 1));
            CHECK(acknowledged.receipts()[0].acknowledged);
            CHECK(acknowledged.receipts()[0].commandResult == expected);
        }
    }
}

TEST_CASE("malformed command outcomes and receipt result shapes reject once without replacing the destination") {
    Permit permit; Application application; TacticalOrderQueue queue;
    REQUIRE(queue.registerSource({1,SourceKind::UI}));
    auto e = order(); e.recipients.clear(); e.payload.kind = OrderKind::Airstrike;
    e.payload.support = AirstrikeCommand{CommanderId{7},{1,2,3},StrikeKind::Small};
    REQUIRE(queue.submit(e, permit));
    application.commandReason = 88;
    drain(queue, 1, application);
    const auto original = wire(queue);
    const std::vector<OrderSource> sources(queue.sources().begin(), queue.sources().end());
    const auto receipt = queue.receipts()[0];
    std::vector<Bytes> malformed;
    auto bad = receipt; bad.commandResult.reset(); malformed.push_back(ingress({},sources,{bad},2));
    bad = receipt; bad.applicationStatus = ApplicationStatus::Applied;
    malformed.push_back(ingress({},sources,{bad},2));
    bad = receipt; bad.applicationStatus = ApplicationStatus::Partial;
    malformed.push_back(ingress({},sources,{bad},2));
    bad = receipt; bad.applicationStatus = ApplicationStatus::Pending;
    malformed.push_back(ingress({e},sources,{bad},2));
    bad = receipt; bad.results = {{{30,2},0,RecipientStatus::Rejected,88}};
    malformed.push_back(ingress({},sources,{bad},2));
    for (const auto& bytes : malformed) {
        int logs = 0;
        CHECK_FALSE(queue.restore(bytes, 1, [&](auto) { ++logs; }));
        CHECK(logs == 1); CHECK(wire(queue) == original);
    }
    for (auto [result, reason] : {std::pair{1u,17u}, std::pair{2u,0u}, std::pair{99u,17u}}) {
        mc2::save::Archive a; a(result, reason); REQUIRE(a.finish());
        ApplicationOutcome destination{RecipientStatus::Rejected,88};
        const auto before = destination;
        int logs = 0;
        CHECK_FALSE(mc2::save::decodeRecord(a.bytes(), destination, [&](auto) { ++logs; }));
        CHECK(logs == 1); CHECK(destination == before);
    }
}

TEST_CASE("malformed queue decode rejects once and preserves existing owner") {
    Permit permit; TacticalOrderQueue queue;
    REQUIRE(queue.registerSource({1,SourceKind::UI}));
    auto admitted = queue.submit(order(), permit); REQUIRE(admitted);
    const auto original = wire(queue);
    const std::vector<OrderEnvelope> pending(queue.snapshotPending().begin(), queue.snapshotPending().end());
    const std::vector<OrderSource> sources(queue.sources().begin(), queue.sources().end());
    const std::vector<OrderReceipt> receipts(queue.receipts().begin(), queue.receipts().end());
    std::vector<Bytes> malformed;
    auto duplicated = sources; duplicated.push_back(sources[0]);
    malformed.push_back(ingress(pending,duplicated,receipts,2));
    auto badSource = sources; badSource[0].nextSequence = 8;
    malformed.push_back(ingress(pending,badSource,receipts,2));
    malformed.push_back(ingress(pending,{},receipts,2));
    malformed.push_back(ingress(pending,sources,{},2));
    auto badReceipt = receipts; badReceipt[0].acknowledged = true;
    malformed.push_back(ingress(pending,sources,badReceipt,2));
    malformed.push_back(ingress(pending,sources,receipts,1));
    auto duplicatePending = pending; duplicatePending.push_back(pending[0]);
    malformed.push_back(ingress(duplicatePending,sources,receipts,2));
    auto truncated = original; truncated.pop_back(); malformed.push_back(truncated);
    auto trailing = original; trailing.push_back(1); malformed.push_back(trailing);
    auto tooMany = original; tooMany[0] = 1; tooMany[1] = 0x10; malformed.push_back(tooMany);
    // Pending envelope starts after its u32 count; issuer's one-byte variant tag is at +20.
    auto unknownIssuer = original; unknownIssuer[24] = 9; malformed.push_back(unknownIssuer);
    auto tooManyRecipients = original; tooManyRecipients[29] = 129; malformed.push_back(tooManyRecipients);
    auto unknownQueueMode = original; unknownQueueMode[57] = 0; malformed.push_back(unknownQueueMode);
    auto unknownOrderKind = original; unknownOrderKind[73] = 255; malformed.push_back(unknownOrderKind);
    for (const auto& bytes : malformed) {
        std::vector<std::string> logs;
        CHECK_FALSE(queue.restore(bytes, 0, [&](auto line) { logs.emplace_back(line); }));
        REQUIRE(logs.size() == 1); CHECK(logs[0].starts_with("[SAVE v1] reject reason="));
        CHECK(wire(queue) == original);
    }
    int logs = 0;
    CHECK_FALSE(queue.restore(original, 1, [&](auto) { ++logs; })); CHECK(logs == 1);
    CHECK(wire(queue) == original);
}

TEST_CASE("restore rejects receipt reuse and admitted total overflow after retirement once without mutation") {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    Permit permit; Application application; TacticalOrderQueue queue;
    REQUIRE(queue.registerSource({1,SourceKind::UI}));
    REQUIRE(queue.registerSource({2,SourceKind::UI}));
    REQUIRE(queue.submit(order(1), permit));
    REQUIRE(queue.submit(order(2), permit));
    drain(queue, 1, application);
    REQUIRE(queue.acknowledge(1)); REQUIRE(queue.acknowledge(2));
    queue.retireAcknowledged(2);
    REQUIRE(queue.receipts().empty());
    const auto original = wire(queue);
    const std::vector<OrderSource> sources(queue.sources().begin(), queue.sources().end());
    const std::vector<Bytes> malformed{
        ingress({},sources,{},1), ingress({},sources,{},2), ingress({},sources,{},4),
        ingress({},{{1,SourceKind::UI,{},max,max-1}},{},max),
        ingress({},{{1,SourceKind::UI,{},max-1,max-2},{2,SourceKind::UI,{},1,0}},{},max),
        ingress({},{{1,SourceKind::UI,{},max-1,max-2},{2,SourceKind::UI,{},2,1}},{},1)
    };
    for (const auto& bytes : malformed) {
        std::vector<std::string> logs;
        CHECK_FALSE(queue.restore(bytes, 0, [&](auto line) { logs.emplace_back(line); }));
        REQUIRE(logs.size() == 1);
        CHECK(logs[0] == "[SAVE v1] reject reason=order_next_receipt");
        CHECK(wire(queue) == original);
        CHECK(queue.completedTick() == 1);
        CHECK_FALSE(queue.executingTick());
    }
    TacticalOrderQueue restored;
    REQUIRE(restored.restore(original, 1));
    const auto receipt = restored.submitNext(order(1,1), permit); REQUIRE(receipt);
    CHECK(receipt->receiptId == 3);
}

TEST_CASE("pending receipt and source capacity rejection consumes no IDs or sequences") {
    Permit permit; TacticalOrderQueue queue; Application application;
    for (std::uint32_t i = 0; i < maxOrderSources; ++i) REQUIRE(queue.registerSource({i,SourceKind::UI}));
    auto extraSource = queue.registerSource({maxOrderSources,SourceKind::UI}); REQUIRE_FALSE(extraSource);
    CHECK(extraSource.error() == OrderError::Capacity);
    std::uint64_t sequence = 0;
    for (int batch = 0; batch < 2; ++batch) {
        for (std::uint32_t i = 0; i < maxPendingOrders; ++i) {
            auto e = order(0,sequence++); e.recipients = {{1,1}};
            REQUIRE(queue.submitNext(e, permit));
        }
        auto full = queue.submitNext(order(0,sequence), permit); REQUIRE_FALSE(full);
        CHECK(full.error() == OrderError::Capacity);
        CHECK(queue.sources()[0].nextSequence == sequence);
        CHECK(queue.nextReceiptId() == sequence + 1);
        drain(queue, static_cast<Tick>(batch + 1), application);
    }
    CHECK(queue.receipts().size() == maxOrderReceipts);
    auto full = queue.submitNext(order(0,sequence), permit); REQUIRE_FALSE(full);
    CHECK(full.error() == OrderError::Capacity);
    REQUIRE(queue.acknowledge(1)); queue.retireAcknowledged(1);
    REQUIRE(queue.submitNext(order(0,sequence), permit));
    CHECK(queue.nextReceiptId() == sequence + 2);
}

TEST_CASE("u64 tick source sequence and receipt overflow boundaries") {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    Permit permit; Application application;
    TacticalOrderQueue terminal(max); REQUIRE(terminal.registerSource({1,SourceKind::UI}));
    auto overflow = terminal.submitNext(order(), permit); REQUIRE_FALSE(overflow);
    CHECK(overflow.error() == OrderError::Overflow); CHECK_FALSE(terminal.beginTick(0));
    TacticalOrderQueue last(max - 1); REQUIRE(last.registerSource({1,SourceKind::UI}));
    auto admitted = last.submitNext(order(), permit); REQUIRE(admitted); CHECK(admitted->applyTick == max);
    drain(last,max,application); CHECK(last.completedTick() == max);
    CHECK_FALSE(last.submitNext(order(1,1), permit));
    TacticalOrderQueue exhausted;
    REQUIRE(exhausted.restore(ingress({},{{1,SourceKind::UI,{},max-1,max-2}},{},max),0));
    overflow = exhausted.submitNext(order(1,max-1), permit); REQUIRE_FALSE(overflow);
    CHECK(overflow.error() == OrderError::Overflow);
    REQUIRE(exhausted.restore(ingress({},{{1,SourceKind::UI,{},max-2,max-3}},{},max-1),0));
    admitted = exhausted.submitNext(order(1,max-2), permit); REQUIRE(admitted); CHECK(admitted->receiptId == max-1);
    overflow = exhausted.submitNext(order(1,max-1), permit); REQUIRE_FALSE(overflow);
    CHECK(overflow.error() == OrderError::Overflow); CHECK(exhausted.sources()[0].nextSequence == max-1);
}

TEST_CASE("application can submit next tick while receipts grow without invalidating the batch") {
    Permit permit; TacticalOrderQueue queue;
    REQUIRE(queue.registerSource({1,SourceKind::UI}));
    auto receipt = queue.submitNext(order(), permit); REQUIRE(receipt);
    REQUIRE(queue.beginTick(1));
    struct SubmittingApplication : Application {
        TacticalOrderQueue& queue;
        Permit& permit;
        SubmittingApplication(TacticalOrderQueue& q, Permit& p) : queue(q), permit(p) {}
        ApplicationOutcome applyRecipient(const OrderEnvelope& e, ObjectRef r, std::uint32_t ordinal) override {
            if (ordinal == 0) {
                for (std::uint64_t i = 1; i < 40; ++i) REQUIRE(queue.submitNext(order(1,i), permit));
                CHECK_FALSE(queue.apply(1,*this));
            }
            return Application::applyRecipient(e,r,ordinal);
        }
    } application(queue,permit);
    REQUIRE(queue.apply(receipt->receiptId,application));
    CHECK(queue.receipts()[0].applicationStatus == ApplicationStatus::Applied);
    CHECK(queue.snapshotPending().size() == 39);
    CHECK(queue.snapshotPending()[0].applyTick == 2);
    REQUIRE(queue.endTick());
}
} // TEST_SUITE

// MP-0: the drained order stream (what every lockstep peer applies) must not depend on the
// wall-clock order in which sources delivered their envelopes.
TEST_CASE("drain order is independent of cross-source admission order and survives wire round trip") {
    auto run = [](const std::vector<std::pair<std::uint32_t, std::uint64_t>>& arrival) {
        Permit permit; TacticalOrderQueue queue(3);
        REQUIRE(queue.registerSource({5, SourceKind::Peer}));
        REQUIRE(queue.registerSource({1, SourceKind::UI}));
        REQUIRE(queue.registerSource({3, SourceKind::ABL, InstanceId{42}}));
        for (auto [src, seq] : arrival) {
            auto envelope = order(src, seq, 4);
            if (src == 3) envelope.issuer = InstanceId{42};
            REQUIRE(queue.submit(envelope, permit));
        }
        auto batch = queue.beginTick(4); REQUIRE(batch);
        Bytes stream;
        for (OrderEnvelope e : *batch) {
            mc2::save::Archive a; a(e); REQUIRE(a.finish());
            OrderEnvelope back; REQUIRE(mc2::save::decodeRecord(a.bytes(), back));
            CHECK(back == e);
            stream.insert(stream.end(), a.bytes().begin(), a.bytes().end());
        }
        return stream;
    };
    const auto a = run({{1,0},{1,1},{3,0},{5,0},{5,1}});
    const auto b = run({{5,0},{3,0},{5,1},{1,0},{1,1}});
    const auto c = run({{3,0},{1,0},{5,0},{1,1},{5,1}});
    CHECK(!a.empty());
    CHECK(a == b);
    CHECK(a == c);
}
