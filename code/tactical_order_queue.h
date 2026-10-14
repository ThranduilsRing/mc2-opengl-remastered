#pragma once

#include "save_archive.h"

#include <expected>

namespace mc2::sim {
using save::Archive;
using Tick = std::uint64_t;

inline constexpr std::uint32_t maxOrderSources = 256, maxPendingOrders = 4096;
inline constexpr std::uint32_t maxOrderReceipts = 8192, maxOrderRecipients = 128;
inline constexpr std::uint32_t maxOrderWaypoints = 128, maxReinforcementRoster = 128;

namespace orderCodec {
template<class E> void tag(Archive& a, E& value, std::underlying_type_t<E> first,
                          std::underlying_type_t<E> last) {
    auto wire = static_cast<std::underlying_type_t<E>>(value);
    a(wire);
    if (!a.good()) return;
    if (wire < first || wire > last) { a.reject("order_tag"); return; }
    if (a.reading()) value = static_cast<E>(wire);
}

template<class T> void sequence(Archive& a, std::vector<T>& values, std::uint32_t cap) {
    if (!a.good()) return;
    if (!a.reading() && values.size() > cap) { a.reject("order_length"); return; }
    auto count = static_cast<std::uint32_t>(values.size());
    a(count);
    if (!a.good()) return;
    if (count > cap) { a.reject("order_length"); return; }
    if (a.reading()) {
        if (!a.allocate(count, sizeof(T))) return;
        values.resize(count);
    }
    for (auto& value : values) { if (!a.good()) break; a(value); }
}
} // namespace orderCodec

// Shared save/sim identity: watch IDs remain a separate mapping.
struct ObjectRef {
    std::uint32_t slot = 0, generation = 0;
    void serialize(Archive& a) { a(slot, generation); }
    bool operator==(const ObjectRef&) const = default;
};
struct CommanderId {
    std::uint32_t value = 0;
    void serialize(Archive& a) { a(value); }
    bool operator==(const CommanderId&) const = default;
};
struct InstanceId {
    std::uint32_t value = 0;
    void serialize(Archive& a) { a(value); }
    bool operator==(const InstanceId&) const = default;
};
using OrderIssuer = std::variant<CommanderId, InstanceId>;

enum class OrderOrigin : std::uint32_t { Player = 0, Commander = 1, Self = 2 };
enum class OrderKind : std::uint32_t {
    None = 0, Wait = 1, MoveToPoint = 2, MoveToObject = 3, JumpToPoint = 4,
    JumpToObject = 5, TraversePath = 6, PatrolPath = 7, Escort = 8, Follow = 9,
    Guard = 10, Stop = 11, PowerUp = 12, PowerDown = 13, WaypointsDone = 14,
    Eject = 15, AttackObject = 16, AttackPoint = 17, HoldFire = 18, Withdraw = 19,
    Scramble = 20, Capture = 21, Refit = 22, GetFixed = 23, LoadIntoCarrier = 24,
    DeployElementals = 25, Recover = 26, Airstrike = 27, Reinforcement = 28, MissionControl = 29
};
enum class TravelMode : std::uint32_t { Slow = 0, Fast = 1, Jump = 2 };
enum class MoveMode : std::uint32_t { Normal = 0, Minelaying = 1 };
enum class AttackType : std::uint32_t { None = 0, Destroy = 1, Disable = 2, ConserveAmmo = 3 };
enum class AttackMethod : std::uint32_t { Ranged = 0, DFA = 1, Ramming = 2 };
enum class FireRange : std::int32_t {
    Default = -4, Ramming = -3, Longest = -2, Optimal = -1, Short = 0, Medium = 1, Long = 2, Current = 3
};
enum class Tactic : std::uint32_t {
    None = 0, FlankLeft = 1, FlankRight = 2, FlankRear = 3, StopAndFire = 4,
    Turret = 5, Joust = 6, IndirectFire = 7, HullDown = 8, FightingWithdraw = 9,
    Pursue = 10, HitAndRun = 11
};

struct WayPath {
    std::vector<save::Vec3> points;
    std::uint32_t curPoint = 0;
    std::vector<TravelMode> mode;
    void serialize(Archive& a);
    void validate(Archive& a) const;
    bool operator==(const WayPath&) const = default;
};
struct TacMoveParams {
    WayPath wayPath;
    bool faceObject = false, wait = false;
    MoveMode mode = MoveMode::Normal;
    bool escapeTile = false, jump = false;
    std::int32_t fromArea = -1;
    bool keepMoving = false;
    void serialize(Archive& a) {
        a(wayPath, faceObject, wait); orderCodec::tag(a, mode, 0u, 1u);
        a(escapeTile, jump, fromArea, keepMoving);
    }
    bool operator==(const TacMoveParams&) const = default;
};
struct TacAttackParams {
    AttackType type = AttackType::Destroy;
    AttackMethod method = AttackMethod::Ranged;
    FireRange range = FireRange::Default;
    Tactic tactic = Tactic::None;
    std::int32_t aimLocation = -1;
    bool pursue = false, obliterate = false;
    save::Vec3 targetPoint;
    void serialize(Archive& a) {
        orderCodec::tag(a, type, 0u, 3u); orderCodec::tag(a, method, 0u, 2u);
        orderCodec::tag(a, range, -4, 3); orderCodec::tag(a, tactic, 0u, 11u);
        a(aimLocation, pursue, obliterate, targetPoint);
    }
    bool operator==(const TacAttackParams&) const = default;
};

enum class StrikeKind : std::uint32_t { Small = 1, Large = 2, Sensor = 3 };
struct AirstrikeCommand {
    CommanderId commanderId;
    save::Vec3 worldPoint;
    StrikeKind strikeKind = StrikeKind::Small;
    void serialize(Archive& a) { a(commanderId, worldPoint); orderCodec::tag(a, strikeKind, 1u, 3u); }
    bool operator==(const AirstrikeCommand&) const = default;
};
struct RosterEntryId {
    std::uint32_t value = 0;
    void serialize(Archive& a) { a(value); }
    bool operator==(const RosterEntryId&) const = default;
};
struct ReinforcementCommand {
    CommanderId commanderId;
    std::vector<RosterEntryId> rosterEntryIds;
    save::Vec3 worldPoint;
    void serialize(Archive& a) {
        a(commanderId); orderCodec::sequence(a, rosterEntryIds, maxReinforcementRoster); a(worldPoint);
    }
    void validate(Archive& a) const;
    bool operator==(const ReinforcementCommand&) const = default;
};
enum class MissionAction : std::uint32_t { SetObjective = 1, SetResource = 2, SetMissionResult = 3 };
struct ObjectiveArgument {
    std::uint32_t objectiveId = 0;
    bool completed = false;
    void serialize(Archive& a) { a(objectiveId, completed); }
    bool operator==(const ObjectiveArgument&) const = default;
};
struct ResourceArgument {
    CommanderId commanderId;
    std::int32_t resourcePoints = 0;
    void serialize(Archive& a) { a(commanderId, resourcePoints); }
    bool operator==(const ResourceArgument&) const = default;
};
enum class MissionResult : std::uint32_t { Running = 0, Won = 1, Lost = 2 };
struct MissionResultArgument {
    MissionResult result = MissionResult::Running;
    void serialize(Archive& a) { orderCodec::tag(a, result, 0u, 2u); }
    bool operator==(const MissionResultArgument&) const = default;
};
struct MissionControlCommand {
    MissionAction action = MissionAction::SetObjective;
    std::variant<ObjectiveArgument, ResourceArgument, MissionResultArgument> arguments;
    void serialize(Archive& a) { orderCodec::tag(a, action, 1u, 3u); a(arguments); }
    void validate(Archive& a) const;
    bool operator==(const MissionControlCommand&) const = default;
};
using SupportCommand = std::variant<AirstrikeCommand, ReinforcementCommand, MissionControlCommand>;

struct TacticalOrder {
    std::uint64_t orderId = 0;
    OrderOrigin origin = OrderOrigin::Player;
    OrderKind kind = OrderKind::Stop;
    bool unitOrder = false, subOrder = false;
    std::optional<TacMoveParams> move;
    std::optional<TacAttackParams> attack;
    std::optional<ObjectRef> target;
    std::uint32_t formationOrdinal = 0, groupFlags = 0;
    std::optional<SupportCommand> support;
    void serialize(Archive& a);
    void validate(Archive& a) const;
    bool operator==(const TacticalOrder&) const = default;
};

enum class ExecutionStage : std::uint32_t { Initial = 0, Active = 1, Complete = 2 };
enum class ExecutionStatus : std::uint32_t { Pending = 0, Running = 1, Succeeded = 2, Failed = 3 };
struct OrderExecution {
    TacticalOrder payload;
    Tick issuedTick = 0, notBeforeTick = 0, lastUpdateTick = 0;
    ExecutionStage stage = ExecutionStage::Initial;
    ExecutionStatus status = ExecutionStatus::Pending;
    save::Vec3 lastMoveGoal;
    std::optional<ObjectRef> pointUnit;
    void serialize(Archive& a) {
        a(payload, issuedTick, notBeforeTick, lastUpdateTick);
        orderCodec::tag(a, stage, 0u, 2u); orderCodec::tag(a, status, 0u, 3u); a(lastMoveGoal, pointUnit);
    }
    void validate(Archive& a) const;
    bool operator==(const OrderExecution&) const = default;
};

enum class SourceKind : std::uint32_t { UI = 1, ABL = 2, Replay = 3, Peer = 4 };
struct OrderSource {
    std::uint32_t sourceId = 0;
    SourceKind kind = SourceKind::UI;
    std::optional<InstanceId> instanceId;
    std::uint64_t nextSequence = 0;
    std::optional<std::uint64_t> lastAdmittedSequence;
    void serialize(Archive& a) {
        a(sourceId); orderCodec::tag(a, kind, 1u, 4u); a(instanceId, nextSequence, lastAdmittedSequence);
    }
    void validate(Archive& a) const;
    bool operator==(const OrderSource&) const = default;
};
enum class QueueMode : std::uint32_t { Replace = 1, Append = 2, Prepend = 3 };
struct OrderEnvelope {
    Tick applyTick = 0;
    std::uint32_t sourceId = 0;
    std::uint64_t sequence = 0;
    OrderIssuer issuer;
    std::vector<ObjectRef> recipients;
    QueueMode queueMode = QueueMode::Replace;
    TacticalOrder payload;
    void serialize(Archive& a) {
        a(applyTick, sourceId, sequence, issuer);
        orderCodec::sequence(a, recipients, maxOrderRecipients);
        orderCodec::tag(a, queueMode, 1u, 3u); a(payload);
    }
    void validate(Archive& a) const;
    bool operator==(const OrderEnvelope&) const = default;
};
enum class AdmissionStatus : std::uint32_t { Accepted = 1 };
enum class ApplicationStatus : std::uint32_t { Pending = 0, Applied = 1, Rejected = 2, Partial = 3 };
enum class RecipientStatus : std::uint32_t { Applied = 1, Rejected = 2 };
struct ApplicationOutcome {
    RecipientStatus result = RecipientStatus::Applied;
    std::uint32_t reasonCode = 0;
    void serialize(Archive& a) { orderCodec::tag(a, result, 1u, 2u); a(reasonCode); }
    void validate(Archive& a) const;
    bool operator==(const ApplicationOutcome&) const = default;
};
struct RecipientResult {
    ObjectRef recipient;
    std::uint32_t ordinal = 0;
    RecipientStatus result = RecipientStatus::Applied;
    std::uint32_t reasonCode = 0;
    void serialize(Archive& a) {
        a(recipient, ordinal); orderCodec::tag(a, result, 1u, 2u); a(reasonCode);
    }
    void validate(Archive& a) const;
    bool operator==(const RecipientResult&) const = default;
};
struct OrderReceipt {
    std::uint64_t receiptId = 0;
    std::uint32_t sourceId = 0;
    std::uint64_t sequence = 0;
    Tick applyTick = 0;
    AdmissionStatus admissionStatus = AdmissionStatus::Accepted;
    ApplicationStatus applicationStatus = ApplicationStatus::Pending;
    std::vector<RecipientResult> results;
    std::optional<ApplicationOutcome> commandResult;
    bool acknowledged = false;
    void serialize(Archive& a) {
        a(receiptId, sourceId, sequence, applyTick);
        orderCodec::tag(a, admissionStatus, 1u, 1u); orderCodec::tag(a, applicationStatus, 0u, 3u);
        orderCodec::sequence(a, results, maxOrderRecipients); a(commandResult, acknowledged);
    }
    void validate(Archive& a) const;
    bool operator==(const OrderReceipt&) const = default;
};

enum class OrderError {
    Shape, UnknownSource, Permission, Duplicate, Sequence, Capacity, Late,
    Overflow, TickContext, UnknownReceipt, AlreadyApplied, Pending, ApplicationResult
};

// Mission authority supplies both admission permissions and live application checks.
struct OrderAdmissionValidator {
    virtual ~OrderAdmissionValidator() = default;
    virtual bool permitted(const OrderSource&, const OrderEnvelope&) const = 0;
};
struct OrderApplication {
    virtual ~OrderApplication() = default;
    virtual std::uint32_t validateCommand(const OrderEnvelope&) const = 0;
    virtual std::uint32_t validateRecipient(const OrderEnvelope&, ObjectRef, std::uint32_t ordinal) const = 0;
    virtual ApplicationOutcome applyRecipient(const OrderEnvelope&, ObjectRef, std::uint32_t ordinal) = 0;
    virtual ApplicationOutcome applySupport(const OrderEnvelope&) = 0;
};

class TacticalOrderQueue {
public:
    explicit TacticalOrderQueue(Tick completed = 0) : completed_(completed) {}
    std::expected<void, OrderError> registerSource(OrderSource source);
    std::expected<OrderReceipt, OrderError> submit(OrderEnvelope envelope, const OrderAdmissionValidator& validator);
    std::expected<OrderReceipt, OrderError> submitNext(OrderEnvelope envelope, const OrderAdmissionValidator& validator);
    std::expected<std::span<const OrderEnvelope>, OrderError> beginTick(Tick tick);
    std::expected<void, OrderError> apply(std::uint64_t receiptId, OrderApplication& application);
    std::expected<void, OrderError> endTick();
    std::expected<void, OrderError> acknowledge(std::uint64_t receiptId);
    void retireAcknowledged(std::uint64_t throughReceiptId);
    std::span<const OrderEnvelope> snapshotPending() const { return pending_; }
    std::span<const OrderSource> sources() const { return sources_; }
    std::span<const OrderReceipt> receipts() const { return receipts_; }
    std::uint64_t nextReceiptId() const { return nextReceiptId_; }
    Tick completedTick() const { return completed_; }
    std::optional<Tick> executingTick() const { return executing_; }
    void serialize(Archive& a);
    bool restore(std::span<const std::uint8_t> bytes, Tick completed, save::Diagnostic diagnostic = {});
private:
    std::vector<OrderEnvelope> pending_;
    std::vector<OrderSource> sources_;
    std::vector<OrderReceipt> receipts_;
    std::uint64_t nextReceiptId_ = 1;
    Tick completed_ = 0;
    std::optional<Tick> executing_;
    std::vector<OrderEnvelope> applying_;
    std::size_t nextApplying_ = 0;
    bool applicationActive_ = false;
    std::expected<Tick, OrderError> admissionBoundary() const;
    void fields(Archive& a);
    void validate(Archive& a) const;
};
} // namespace mc2::sim
