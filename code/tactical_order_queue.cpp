#include "tactical_order_queue.h"

#include <algorithm>
#include <tuple>

namespace mc2::sim {
namespace {
bool supportKind(OrderKind kind) { return kind >= OrderKind::Airstrike && kind <= OrderKind::MissionControl; }
bool moveKind(OrderKind kind) {
    return (kind >= OrderKind::MoveToPoint && kind <= OrderKind::Guard) ||
           kind == OrderKind::Withdraw || kind == OrderKind::Scramble;
}
bool targetKind(OrderKind kind) {
    switch (kind) {
    case OrderKind::MoveToObject: case OrderKind::JumpToObject: case OrderKind::Escort:
    case OrderKind::Follow: case OrderKind::AttackObject:
    case OrderKind::Capture: case OrderKind::Refit: case OrderKind::GetFixed:
    case OrderKind::LoadIntoCarrier: case OrderKind::Recover: return true;
    default: return false;
    }
}
template<class T> bool unique(const std::vector<T>& values) {
    for (std::size_t i = 0; i < values.size(); ++i)
        if (std::find(values.begin() + static_cast<std::ptrdiff_t>(i + 1), values.end(), values[i]) != values.end())
            return false;
    return true;
}
bool before(const OrderEnvelope& a, const OrderEnvelope& b) {
    return std::tie(a.applyTick, a.sourceId, a.sequence) < std::tie(b.applyTick, b.sourceId, b.sequence);
}
bool sameIdentity(const OrderEnvelope& e, const OrderReceipt& r) {
    return e.sourceId == r.sourceId && e.sequence == r.sequence && e.applyTick == r.applyTick;
}
bool sourcePermits(const OrderSource& source, const OrderEnvelope& e) {
    if (source.kind == SourceKind::ABL) {
        const auto* issuer = std::get_if<InstanceId>(&e.issuer);
        return issuer && source.instanceId == *issuer;
    }
    if (source.kind == SourceKind::Replay) return true;
    return std::holds_alternative<CommanderId>(e.issuer);
}
bool validOutcome(const ApplicationOutcome& result) {
    return (result.result == RecipientStatus::Applied && result.reasonCode == 0) ||
           (result.result == RecipientStatus::Rejected && result.reasonCode != 0);
}
} // namespace

void WayPath::serialize(Archive& a) {
    orderCodec::sequence(a, points, maxOrderWaypoints);
    a(curPoint);
    if (!a.good()) return;
    if (a.reading()) {
        if (!a.allocate(points.size(), sizeof(TravelMode))) return;
        mode.resize(points.size());
    }
    if (mode.size() != points.size()) { a.reject("order_waypath_modes"); return; }
    for (auto& value : mode) orderCodec::tag(a, value, 0u, 2u);
}
void WayPath::validate(Archive& a) const {
    if (points.size() > maxOrderWaypoints || curPoint > points.size() || mode.size() != points.size())
        a.reject("order_waypath");
}
void ReinforcementCommand::validate(Archive& a) const {
    if (rosterEntryIds.empty() || rosterEntryIds.size() > maxReinforcementRoster || !unique(rosterEntryIds))
        a.reject("order_reinforcement_roster");
}
void MissionControlCommand::validate(Archive& a) const {
    if (static_cast<std::uint32_t>(action) != arguments.index() + 1) a.reject("order_mission_arguments");
    if (const auto* resource = std::get_if<ResourceArgument>(&arguments); resource && resource->resourcePoints < 0)
        a.reject("order_resource_points");
}
void TacticalOrder::serialize(Archive& a) {
    a(orderId); orderCodec::tag(a, origin, 0u, 2u); orderCodec::tag(a, kind, 0u, 29u);
    a(unitOrder, subOrder);
    if (!a.good()) return;
    if (supportKind(kind)) {
        if (a.reading()) { support.emplace(); move.reset(); attack.reset(); }
        if (!support) { a.reject("order_support_payload"); return; }
        a(*support);
    } else {
        if (a.reading()) support.reset();
        a(move, attack);
    }
    a(target, formationOrdinal, groupFlags);
}
void TacticalOrder::validate(Archive& a) const {
    if (kind == OrderKind::None) { a.reject("order_none"); return; }
    if (supportKind(kind)) {
        if (!support || move || attack || target ||
            support->index() != static_cast<std::uint32_t>(kind) - 27) a.reject("order_support_shape");
        return;
    }
    const bool attacking = kind == OrderKind::AttackObject || kind == OrderKind::AttackPoint;
    if (support || (moveKind(kind) && !move) ||
        (!moveKind(kind) && kind != OrderKind::Wait && move) || attacking != attack.has_value() ||
        (kind != OrderKind::Guard && targetKind(kind) != target.has_value())) a.reject("order_payload_shape");
    else if (moveKind(kind) && !target && move->wayPath.points.empty()) a.reject("order_goal_missing");
}
void OrderExecution::validate(Archive& a) const {
    if (notBeforeTick < issuedTick || (lastUpdateTick != 0 && lastUpdateTick < issuedTick))
        a.reject("order_execution_ticks");
}
void OrderSource::validate(Archive& a) const {
    if ((kind == SourceKind::ABL) != instanceId.has_value()) { a.reject("order_source_instance"); return; }
    if (lastAdmittedSequence) {
        if (*lastAdmittedSequence == std::numeric_limits<std::uint64_t>::max() ||
            nextSequence != *lastAdmittedSequence + 1) a.reject("order_source_sequence");
    } else if (nextSequence != 0) a.reject("order_source_sequence");
}
void OrderEnvelope::validate(Archive& a) const {
    if (applyTick == 0 || recipients.size() > maxOrderRecipients || !unique(recipients) ||
        supportKind(payload.kind) != recipients.empty()) { a.reject("order_envelope_shape"); return; }
    if (const auto* commander = std::get_if<CommanderId>(&issuer); commander && payload.support) {
        std::visit([&](const auto& command) {
            if constexpr (requires { command.commanderId; }) {
                if (command.commanderId != *commander) a.reject("order_support_commander");
            } else if (const auto* resource = std::get_if<ResourceArgument>(&command.arguments);
                       resource && resource->commanderId != *commander) a.reject("order_support_commander");
        }, *payload.support);
    }
}
void RecipientResult::validate(Archive& a) const {
    if ((result == RecipientStatus::Applied) != (reasonCode == 0)) a.reject("order_result_reason");
}
void ApplicationOutcome::validate(Archive& a) const {
    if (!validOutcome(*this)) a.reject("order_command_reason");
}
void OrderReceipt::validate(Archive& a) const {
    if (receiptId == 0 || applyTick == 0 || results.size() > maxOrderRecipients) {
        a.reject("order_receipt_shape"); return;
    }
    if (applicationStatus == ApplicationStatus::Pending) {
        if (acknowledged || !results.empty() || commandResult) a.reject("order_pending_receipt");
        return;
    }
    // Support commands have no recipients; their result is the only durable explanation of failure.
    if (results.empty() != commandResult.has_value()) { a.reject("order_receipt_result_shape"); return; }
    if (commandResult) {
        const auto status = commandResult->result == RecipientStatus::Applied
            ? ApplicationStatus::Applied : ApplicationStatus::Rejected;
        if (status != applicationStatus) a.reject("order_application_status");
        return;
    }
    std::size_t applied = 0;
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (results[i].ordinal != i) { a.reject("order_result_ordinal"); return; }
        for (std::size_t j = 0; j < i; ++j)
            if (results[j].recipient == results[i].recipient) { a.reject("order_result_recipient"); return; }
        if (results[i].result == RecipientStatus::Applied) ++applied;
    }
    const auto status = applied == results.size() ? ApplicationStatus::Applied :
        applied == 0 ? ApplicationStatus::Rejected : ApplicationStatus::Partial;
    if (status != applicationStatus) a.reject("order_application_status");
}

std::expected<Tick, OrderError> TacticalOrderQueue::admissionBoundary() const {
    const auto base = executing_.value_or(completed_);
    if (base == std::numeric_limits<Tick>::max()) return std::unexpected(OrderError::Overflow);
    return base + 1;
}
std::expected<void, OrderError> TacticalOrderQueue::registerSource(OrderSource source) {
    Archive check([](std::string_view) {});
    check(source);
    if (!check.good() || source.nextSequence != 0 || source.lastAdmittedSequence)
        return std::unexpected(OrderError::Shape);
    auto position = std::ranges::lower_bound(sources_, source.sourceId, {}, &OrderSource::sourceId);
    if (position != sources_.end() && position->sourceId == source.sourceId)
        return std::unexpected(OrderError::Duplicate);
    if (sources_.size() == maxOrderSources) return std::unexpected(OrderError::Capacity);
    sources_.insert(position, std::move(source));
    return {};
}
std::expected<OrderReceipt, OrderError> TacticalOrderQueue::submitNext(OrderEnvelope envelope,
    const OrderAdmissionValidator& validator) {
    const auto boundary = admissionBoundary();
    if (!boundary) return std::unexpected(boundary.error());
    envelope.applyTick = *boundary;
    return submit(std::move(envelope), validator);
}
std::expected<OrderReceipt, OrderError> TacticalOrderQueue::submit(OrderEnvelope envelope,
    const OrderAdmissionValidator& validator) {
    const auto boundary = admissionBoundary();
    if (!boundary) return std::unexpected(boundary.error());
    if (envelope.applyTick < *boundary) return std::unexpected(OrderError::Late);
    auto source = std::ranges::lower_bound(sources_, envelope.sourceId, {}, &OrderSource::sourceId);
    if (source == sources_.end() || source->sourceId != envelope.sourceId)
        return std::unexpected(OrderError::UnknownSource);
    if (source->lastAdmittedSequence && envelope.sequence <= *source->lastAdmittedSequence)
        return std::unexpected(OrderError::Duplicate);
    if (envelope.sequence != source->nextSequence) return std::unexpected(OrderError::Sequence);
    if (source->nextSequence == std::numeric_limits<std::uint64_t>::max() ||
        nextReceiptId_ == std::numeric_limits<std::uint64_t>::max()) return std::unexpected(OrderError::Overflow);
    Archive check([](std::string_view) {});
    check(envelope);
    if (!check.good()) return std::unexpected(OrderError::Shape);
    if (!sourcePermits(*source, envelope) || !validator.permitted(*source, envelope))
        return std::unexpected(OrderError::Permission);
    if (pending_.size() == maxPendingOrders || receipts_.size() == maxOrderReceipts)
        return std::unexpected(OrderError::Capacity);
    OrderReceipt receipt;
    receipt.receiptId = nextReceiptId_;
    receipt.sourceId = envelope.sourceId;
    receipt.sequence = envelope.sequence;
    receipt.applyTick = envelope.applyTick;
    // Both reservations precede admission so allocation failure cannot consume a sequence or ID.
    if (pending_.size() == pending_.capacity())
        pending_.reserve(std::min<std::size_t>(maxPendingOrders, std::max<std::size_t>(8, pending_.size() * 2)));
    if (receipts_.size() == receipts_.capacity())
        receipts_.reserve(std::min<std::size_t>(maxOrderReceipts, std::max<std::size_t>(8, receipts_.size() * 2)));
    pending_.insert(std::ranges::lower_bound(pending_, envelope, before), std::move(envelope));
    receipts_.push_back(receipt);
    source->lastAdmittedSequence = receipt.sequence;
    ++source->nextSequence;
    ++nextReceiptId_;
    return receipt;
}
std::expected<std::span<const OrderEnvelope>, OrderError> TacticalOrderQueue::beginTick(Tick tick) {
    if (executing_ || completed_ == std::numeric_limits<Tick>::max() || tick != completed_ + 1)
        return std::unexpected(OrderError::TickContext);
    const auto end = std::ranges::upper_bound(pending_, tick, {}, &OrderEnvelope::applyTick);
    applying_.assign(pending_.begin(), end);
    pending_.erase(pending_.begin(), end);
    executing_ = tick;
    nextApplying_ = 0;
    return std::span<const OrderEnvelope>(applying_);
}
std::expected<void, OrderError> TacticalOrderQueue::apply(std::uint64_t receiptId, OrderApplication& application) {
    if (!executing_ || applicationActive_) return std::unexpected(OrderError::TickContext);
    auto receipt = std::ranges::lower_bound(receipts_, receiptId, {}, &OrderReceipt::receiptId);
    if (receipt == receipts_.end() || receipt->receiptId != receiptId)
        return std::unexpected(OrderError::UnknownReceipt);
    if (receipt->applicationStatus != ApplicationStatus::Pending) return std::unexpected(OrderError::AlreadyApplied);
    const auto envelope = std::ranges::find_if(applying_, [&](const auto& e) { return sameIdentity(e, *receipt); });
    if (envelope == applying_.end()) return std::unexpected(OrderError::TickContext);
    // Application is ordered just like the immutable batch; callers cannot reverse system effects.
    if (envelope != applying_.begin() + static_cast<std::ptrdiff_t>(nextApplying_))
        return std::unexpected(OrderError::Sequence);
    struct ActiveApplication {
        bool& active;
        explicit ActiveApplication(bool& value) : active(value) { active = true; }
        ~ActiveApplication() { active = false; }
    } active(applicationActive_);
    std::vector<RecipientResult> results;
    std::optional<ApplicationOutcome> commandResult;
    results.reserve(envelope->recipients.size());
    const auto commandReason = application.validateCommand(*envelope);
    std::size_t applied = 0;
    ApplicationStatus status;
    if (supportKind(envelope->payload.kind)) {
        auto outcome = commandReason ? ApplicationOutcome{RecipientStatus::Rejected, commandReason}
                                           : application.applySupport(*envelope);
        if (!validOutcome(outcome)) outcome = {RecipientStatus::Rejected, 0xffffffffu};
        commandResult = outcome;
        status = outcome.result == RecipientStatus::Applied
            ? ApplicationStatus::Applied : ApplicationStatus::Rejected;
    } else {
        for (std::size_t i = 0; i < envelope->recipients.size(); ++i) {
            const auto ordinal = static_cast<std::uint32_t>(i);
            const auto recipient = envelope->recipients[i];
            const auto reason = commandReason ? commandReason : application.validateRecipient(*envelope, recipient, ordinal);
            auto outcome = reason ? ApplicationOutcome{RecipientStatus::Rejected, reason}
                                  : application.applyRecipient(*envelope, recipient, ordinal);
            if (!validOutcome(outcome)) outcome = {RecipientStatus::Rejected, 0xffffffffu};
            results.push_back({recipient, ordinal, outcome.result, outcome.reasonCode});
            if (outcome.result == RecipientStatus::Applied) ++applied;
        }
        status = applied == results.size() ? ApplicationStatus::Applied :
            applied == 0 ? ApplicationStatus::Rejected : ApplicationStatus::Partial;
    }
    // ABL application may admit another command and grow the retained receipt storage.
    receipt = std::ranges::lower_bound(receipts_, receiptId, {}, &OrderReceipt::receiptId);
    receipt->applicationStatus = status;
    receipt->results = std::move(results);
    receipt->commandResult = commandResult;
    ++nextApplying_;
    return {};
}
std::expected<void, OrderError> TacticalOrderQueue::endTick() {
    if (!executing_) return std::unexpected(OrderError::TickContext);
    for (const auto& receipt : receipts_)
        if (receipt.applyTick <= *executing_ && receipt.applicationStatus == ApplicationStatus::Pending)
            return std::unexpected(OrderError::Pending);
    completed_ = *executing_;
    executing_.reset();
    applying_.clear();
    return {};
}
std::expected<void, OrderError> TacticalOrderQueue::acknowledge(std::uint64_t receiptId) {
    auto receipt = std::ranges::lower_bound(receipts_, receiptId, {}, &OrderReceipt::receiptId);
    if (receipt == receipts_.end() || receipt->receiptId != receiptId)
        return std::unexpected(OrderError::UnknownReceipt);
    if (receipt->applicationStatus == ApplicationStatus::Pending) return std::unexpected(OrderError::Pending);
    receipt->acknowledged = true;
    return {};
}
void TacticalOrderQueue::retireAcknowledged(std::uint64_t throughReceiptId) {
    // Retiring during a drain would invalidate the receipt identity of its immutable batch.
    if (executing_) return;
    std::erase_if(receipts_, [&](const auto& r) { return r.receiptId <= throughReceiptId && r.acknowledged; });
}

void TacticalOrderQueue::fields(Archive& a) {
    orderCodec::sequence(a, pending_, maxPendingOrders);
    orderCodec::sequence(a, sources_, maxOrderSources);
    orderCodec::sequence(a, receipts_, maxOrderReceipts);
    a(nextReceiptId_);
}
void TacticalOrderQueue::validate(Archive& a) const {
    // Source sequences retain every admission, including receipts retired after acknowledgement.
    std::uint64_t expectedNextReceipt = 1;
    for (const auto& source : sources_) {
        if (source.nextSequence > std::numeric_limits<std::uint64_t>::max() - expectedNextReceipt) {
            a.reject("order_next_receipt"); return;
        }
        expectedNextReceipt += source.nextSequence;
    }
    if (nextReceiptId_ != expectedNextReceipt) { a.reject("order_next_receipt"); return; }
    for (std::size_t i = 0; i < sources_.size(); ++i)
        if (i && sources_[i - 1].sourceId >= sources_[i].sourceId) { a.reject("order_source_order"); return; }
    for (std::size_t i = 0; i < receipts_.size(); ++i) {
        const auto& r = receipts_[i];
        if ((i && receipts_[i - 1].receiptId >= r.receiptId) || r.receiptId >= nextReceiptId_) {
            a.reject("order_receipt_order"); return;
        }
        const auto source = std::ranges::lower_bound(sources_, r.sourceId, {}, &OrderSource::sourceId);
        if (source == sources_.end() || source->sourceId != r.sourceId || !source->lastAdmittedSequence ||
            r.sequence > *source->lastAdmittedSequence) { a.reject("order_receipt_source"); return; }
        if ((r.applicationStatus == ApplicationStatus::Pending) != (r.applyTick > completed_)) {
            a.reject("order_receipt_tick"); return;
        }
        for (std::size_t j = 0; j < i; ++j)
            if (receipts_[j].sourceId == r.sourceId && receipts_[j].sequence == r.sequence) {
                a.reject("order_receipt_identity"); return;
            }
    }
    std::size_t pendingReceipts = 0;
    for (const auto& r : receipts_) if (r.applicationStatus == ApplicationStatus::Pending) ++pendingReceipts;
    if (pendingReceipts != pending_.size()) { a.reject("order_pending_receipts"); return; }
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        const auto& e = pending_[i];
        if ((i && !before(pending_[i - 1], e)) || e.applyTick <= completed_) {
            a.reject("order_pending_order"); return;
        }
        const auto source = std::ranges::lower_bound(sources_, e.sourceId, {}, &OrderSource::sourceId);
        if (source == sources_.end() || source->sourceId != e.sourceId || !sourcePermits(*source, e)) {
            a.reject("order_pending_source"); return;
        }
        const auto receipt = std::ranges::find_if(receipts_, [&](const auto& r) { return sameIdentity(e, r); });
        if (receipt == receipts_.end() || receipt->applicationStatus != ApplicationStatus::Pending) {
            a.reject("order_pending_identity"); return;
        }
    }
}
void TacticalOrderQueue::serialize(Archive& a) {
    if (executing_) { a.reject("order_save_executing"); return; }
    if (!a.reading()) { fields(a); if (a.good()) validate(a); return; }
    TacticalOrderQueue candidate(completed_);
    candidate.fields(a);
    if (a.good()) candidate.validate(a);
    if (a.good()) *this = std::move(candidate);
}
bool TacticalOrderQueue::restore(std::span<const std::uint8_t> bytes, Tick completed,
                                 save::Diagnostic diagnostic) {
    Archive a(bytes, std::move(diagnostic));
    if (executing_) { a.reject("order_restore_executing"); return false; }
    TacticalOrderQueue candidate(completed);
    a(candidate);
    if (!a.finish()) return false;
    *this = std::move(candidate);
    return true;
}
} // namespace mc2::sim
