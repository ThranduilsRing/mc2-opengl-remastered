//******************************************************************************************
//	mission.cpp - This file contains the mission class code
//		Missions are what scenarios were in MechCommander 1.
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//----------------------------------------------------------------------------------

#include "mission_internal.h"
#include "mc2fmt.h"
#include "../GameOS/gameos/gos_dev_shell.h"
#include <algorithm>
#include <limits>

namespace {
struct MissionOrderAccess {
    Mission& owner;
    MoverPtr resolve(mc2::sim::ObjectRef ref) const { return owner.resolveOrderRecipient(ref); }
    float simulationTime() const { return scenarioTime; }
    void installed(const mc2::sim::OrderEnvelope& e, mc2::sim::ObjectRef ref) const {
        owner.recordAuthoredInstallation(e, ref);
    }
};
using MissionOrderApplication = mc2::sim::MoverOrderApplication<::TacticalOrder, MissionOrderAccess>;
static_assert(TACTICAL_ORDER_NONE == 0 && TACTICAL_ORDER_STOP == 11 && TACTICAL_ORDER_MOVETO_POINT == 2);
static_assert(TACTICAL_ORDER_TRAVERSE_PATH == 6 && TACTICAL_ORDER_PATROL_PATH == 7);
static_assert(ORDER_ORIGIN_PLAYER == 0 && ORDER_ORIGIN_COMMANDER == 1);
static_assert(ORDERSTATE_GENERAL == 0 && ORDERSTATE_PLAYER == 1);

bool submitOrderFixture(const char*, std::string& data, std::string& error) {
    const bool ok = mission && mission->runOrderIngressFixture();
    mc2::fmt::print_err("[ORDER_INGRESS_FIXTURE v1] event=submit ok={} mission={}\n", ok, mission != nullptr);
    if (!ok) {
        error = mission ? "mission-content fixture admission failed (see fixture admission log)" : "mission unavailable";
        return false;
    }
    data = "{}";
    return true;
}
bool checkOrderFixture(const char*, std::string& data, std::string& error) {
    const bool ok = mission && mission->orderIngressFixtureApplied();
    mc2::fmt::print_err("[ORDER_INGRESS_FIXTURE v1] event=check ok={} mission={}\n", ok, mission != nullptr);
    if (!ok) {
        error = mission ? "mission-content fixture did not install and automatically retire every receipt" : "mission unavailable";
        return false;
    }
    data = "{}";
    return true;
}
bool authoredOrderEvidence(const char*, std::string& data, std::string& error) {
    if (!mission) { error = "mission unavailable"; return false; }
    data = mission->authoredRouteEvidence();
    return true;
}
}

void Mission::resetOrderIngress() {
    orderRecipients.clear();
    authoredMissionText.clear();
    authoredOpordPilots.clear();
    authoredRoutes.clear();
    routeExecutions.clear();
    rejectedRouteSignals.clear();
    nextPartIncarnation = nextPilotIncarnation = nextRouteExecution = nextOrderRevision = 1;
    authoredDiagnostics.clear();
    orderFixtureReceipt.reset();
    orderFixtureApplied = false;
    orderFixtureSubmitted = orderFixtureCompleted = 0;
    orderIngress.reset();
}
void Mission::bindOrderRecipients() {
    // Session tokens isolate recipient slots; route identities belong to this mission.
    if (!orderRecipients.empty()) return;
    for (long partId = 1; partId <= numParts; ++partId) {
        auto* object = ObjectManager->getByWatchID(parts[partId].objectWID);
        if (!object || !object->isMover() || !object->getPilot() || !object->getExists()) continue;
        auto* mover = static_cast<MoverPtr>(object);
        if (nextPartIncarnation == std::numeric_limits<std::uint64_t>::max() ||
            nextPilotIncarnation == std::numeric_limits<std::uint64_t>::max()) {
            for (auto& route : authoredRoutes) {
                if (route.originalPart != partId || route.diagnosed) continue;
                diagnoseAuthoredRoute(route.originalPart, route.selectedPart, route.pilot, "Rejected", "identity");
                route.diagnosed = true;
            }
            continue;
        }
        orderRecipients.push_back({{static_cast<std::uint32_t>(orderRecipients.size() + 1), 1},
            partId, static_cast<std::uint32_t>(parts[partId].objectWID), mover, mover->getPilot()});
        orderRecipients.back().partIncarnation = nextPartIncarnation++;
        orderRecipients.back().pilotIncarnation = nextPilotIncarnation++;
    }
    gos_dev_shell::registerCommand("order_ingress_fixture_submit", submitOrderFixture);
    gos_dev_shell::registerCommand("order_ingress_fixture_check", checkOrderFixture);
    gos_dev_shell::registerCommand("authored_order_evidence", authoredOrderEvidence);
    if (mc2::gate::get<"MC2_DEV_SHELL">())
        mc2::fmt::print_err("[ORDER_INGRESS_FIXTURE v1] event=registered recipients={}\n", orderRecipients.size());
}
void Mission::admitAuthoredRoutes() {
    using namespace mc2::sim;
    for (auto& route : authoredRoutes) {
        if (route.diagnosed) continue;
        const auto ref = orderRecipient(route.originalPart);
        auto* mover = ref ? resolveOrderRecipient(*ref) : nullptr;
        if (!mover || mover->getCommanderId() < 0 || route.pilot >= MAX_WARRIORS ||
            mover->getPilot() != MechWarrior::warriorList[route.pilot]) {
            diagnoseAuthoredRoute(route.originalPart, route.selectedPart, route.pilot, "Rejected", "recipient");
            route.diagnosed = true;
            continue;
        }
        OrderEnvelope envelope;
        envelope.sourceId = 1;
        envelope.sequence = orderIngress.queue().sources().front().nextSequence;
        envelope.issuer = CommanderId{static_cast<std::uint32_t>(mover->getCommanderId())};
        envelope.recipients = {*ref};
        envelope.payload.orderId = route.originalPart;
        envelope.payload.origin = OrderOrigin::Commander;
        envelope.payload.kind = route.kind;
        envelope.payload.move.emplace();
        envelope.payload.move->wayPath = route.path;
        const auto receipt = submitMissionOrder(orderSession(), std::move(envelope));
        if (receipt) route.receiptId = receipt->receiptId;
        else {
            diagnoseAuthoredRoute(route.originalPart, route.selectedPart, route.pilot, "Rejected", "admission");
            route.diagnosed = true;
        }
    }
}
void Mission::recordAuthoredInstallation(const mc2::sim::OrderEnvelope& e, mc2::sim::ObjectRef ref) {
    for (auto& route : authoredRoutes) {
        if (!route.receiptId || route.installed || route.diagnosed || route.originalPart != e.payload.orderId ||
            e.sourceId != 1 || orderRecipient(route.originalPart) != ref) continue;
        auto* mover = resolveOrderRecipient(ref);
        if (!mover || route.pilot >= MAX_WARRIORS || mover->getPilot() != MechWarrior::warriorList[route.pilot]) continue;
        if (nextRouteExecution == std::numeric_limits<std::uint64_t>::max() ||
            nextOrderRevision == std::numeric_limits<std::uint64_t>::max()) {
            diagnoseAuthoredRoute(route.originalPart, route.selectedPart, route.pilot, "Rejected", "identity");
            route.diagnosed = true;
            continue;
        }
        const auto& binding = orderRecipients[ref.slot - 1];
        mc2::sim::RouteExecution execution;
        execution.key = {binding.partIncarnation, nextRouteExecution++};
        execution.pilotIncarnation = binding.pilotIncarnation;
        execution.originalPart = route.originalPart;
        execution.selectedPart = route.selectedPart;
        execution.pilot = route.pilot;
        execution.stance = route.stance;
        execution.commander = mover->getCommanderId();
        execution.kind = route.kind;
        execution.path = route.path;
        execution.revision = nextOrderRevision++;
        execution.lastTransitionTick = e.applyTick;
        routeExecutions.push_back(std::move(execution));
        route.installed = true;
        mc2::fmt::print_err("[AUTHORED_ORDER v1] status=Installed original={} selected={} pilot={} stance={} points={}\n",
            route.originalPart, route.selectedPart, route.pilot, route.stance, route.path.points.size());
    }
}
bool Mission::routeLive(const mc2::sim::RouteExecution& route) {
    const auto binding = std::ranges::find(orderRecipients, route.key.partIncarnation,
        &OrderRecipientBinding::partIncarnation);
    if (binding == orderRecipients.end()) return false;
    auto* mover = resolveOrderRecipient(binding->ref);
    return mover && !mover->isDisabled() && route.pilot < MAX_WARRIORS &&
        binding->pilotIncarnation == route.pilotIncarnation &&
        mover->getPilot() == MechWarrior::warriorList[route.pilot] &&
        mover->getPilot()->getStatus() == WARRIOR_STATUS_NORMAL && mover->getCommanderId() == route.commander;
}
mc2::sim::RouteExecution* Mission::ownedRoute(const MechWarrior* pilot) {
    if (routeExecutions.empty()) return nullptr;
    for (auto& route : routeExecutions) {
        if (route.terminal()) continue;
        const auto binding = std::ranges::find(orderRecipients, route.key.partIncarnation,
            &OrderRecipientBinding::partIncarnation);
        if (binding == orderRecipients.end() || binding->pilot != pilot ||
            binding->pilotIncarnation != route.pilotIncarnation) continue;
        if (!routeLive(route)) {
            signalRoute(route, mc2::sim::RouteAction::Removed);
            return nullptr;
        }
        return &route;
    }
    return nullptr;
}
bool Mission::routeMapsReady(MechWarrior* pilot) const {
    const auto level = pilot->getVehicle()->getMoveLevel();
    return PathFindMap[SECTOR_PATHMAP] && PathFindMap[SIMPLE_PATHMAP] &&
        GlobalMoveMap[level] && !GlobalMoveMap[level]->badLoad && GlobalMoveMap[level]->areas;
}
bool Mission::endedRouteOrder(MechWarrior* pilot) const {
    if (pilot->getOrderState() != ORDERSTATE_GENERAL) return false;
    const auto* order = pilot->getCurTacOrder();
    if (order->stage != 2) return false;
    for (const auto& route : routeExecutions) {
        if (!route.terminal() || order->code != static_cast<int>(route.kind)) continue;
        const auto binding = std::ranges::find(orderRecipients, route.key.partIncarnation,
            &OrderRecipientBinding::partIncarnation);
        if (binding != orderRecipients.end() && binding->pilot == pilot) return true;
    }
    return false;
}
void Mission::signalRoute(mc2::sim::RouteExecution& route, mc2::sim::RouteAction action, bool mapsReady) {
    onRouteEvent({route.key, route.revision, route.eventSequence + 1,
        orderIngress.queue().executingTick().value_or(orderIngress.queue().completedTick()),
        action, mapsReady, mapsReady, true, route.pilotIncarnation});
}
const mc2::sim::RouteExecution* Mission::routeExecution(mc2::sim::RouteKey key) const {
    const auto found = std::ranges::find(routeExecutions, key, &mc2::sim::RouteExecution::key);
    return found == routeExecutions.end() ? nullptr : &*found;
}
std::optional<mc2::sim::RouteTransition> Mission::onRouteEvent(const mc2::sim::RouteSignal& signal) {
    auto found = std::ranges::find(routeExecutions, signal.key, &mc2::sim::RouteExecution::key);
    if (found != routeExecutions.end()) {
        auto current = signal;
        if (!routeLive(*found)) current.action = mc2::sim::RouteAction::Removed;
        if (auto event = found->transition(current)) {
            found->retryPending = event->event == mc2::sim::RouteEventKind::Retrying;
            if (found->terminal() || found->state == mc2::sim::RouteState::Suspended)
                found->pendingSegment.reset();
            if (found->terminal()) {
                const auto binding = std::ranges::find(orderRecipients, found->key.partIncarnation,
                    &OrderRecipientBinding::partIncarnation);
                if (binding != orderRecipients.end() && binding->pilot) {
                    auto* pilot = binding->pilot;
                    if (PathManager) PathManager->remove(pilot);
                    pilot->setMoveGoal(MOVEGOAL_NONE, nullptr);
                    pilot->setMoveWayPath(nullptr);
                    pilot->setMoveGlobalPath(nullptr, 0);
                    for (long buffer = 0; buffer < 2; ++buffer) pilot->clearMovePath(buffer);
                    // Retired route orders must not resume through legacy path handling.
                    for (auto* order : {pilot->getCurTacOrder(), pilot->getTacOrder(ORDERSTATE_GENERAL)})
                        if (order->code == static_cast<int>(found->kind)) order->setStage(2);
                }
            }
            mc2::fmt::print_err("[ROUTE_EXECUTION v1] execution={} revision={} sequence={} tick={} event={} reason={}\n",
                event->key.executionId, event->revision, event->sequence, event->tick,
                static_cast<int>(event->event), static_cast<int>(event->reason));
            return event;
        }
    }
    if (found == routeExecutions.end()) return {};
    const auto rejected = std::ranges::find(rejectedRouteSignals, signal.key, &mc2::sim::RouteSignal::key);
    const auto duplicate = rejected != rejectedRouteSignals.end() &&
        rejected->revision == signal.revision && rejected->sequence == signal.sequence && rejected->action == signal.action;
    if (!duplicate) {
        if (rejected == rejectedRouteSignals.end()) rejectedRouteSignals.push_back(signal);
        else *rejected = signal;
        mc2::fmt::print_err("[ROUTE_EXECUTION v1] rejected execution={} revision={} sequence={} tick={} action={}\n",
            signal.key.executionId, signal.revision, signal.sequence, signal.tick, static_cast<int>(signal.action));
    }
    return {};
}
std::string Mission::authoredRouteEvidence() const {
    std::string result = "{\"installations\":[";
    bool first = true;
    for (const auto& route : authoredRoutes) {
        if (!route.installed) continue;
        if (!first) result += ',';
        first = false;
        result += std::format("{{\"status\":\"Installed\",\"original\":{},\"selected\":{},\"pilot\":{},\"stance\":{},\"kind\":{},\"points\":[",
            route.originalPart, route.selectedPart, route.pilot, route.stance, static_cast<unsigned>(route.kind));
        for (std::size_t i = 0; i < route.path.points.size(); ++i) {
            if (i) result += ',';
            const auto& p = route.path.points[i];
            result += std::format("[{},{},{},{}]", p.x, p.y, p.z, static_cast<unsigned>(route.path.mode[i]));
        }
        result += "]}";
    }
    result += "],\"diagnostics\":[";
    for (std::size_t i = 0; i < authoredDiagnostics.size(); ++i) {
        if (i) result += ',';
        result += authoredDiagnostics[i];
    }
    return result + "]}";
}
MoverPtr Mission::resolveOrderRecipient(mc2::sim::ObjectRef ref) {
    if (!ref.slot || ref.slot > orderRecipients.size()) return nullptr;
    auto& binding = orderRecipients[ref.slot - 1];
    return binding.resolve(ref, [](std::uint32_t watchId) { return ObjectManager->getByWatchID(watchId); },
        static_cast<std::uint32_t>(parts[binding.partId].objectWID));
}
std::optional<mc2::sim::ObjectRef> Mission::orderRecipient(long partId) const {
    for (const auto& binding : orderRecipients)
        if (binding.partId == partId && !binding.retired) return binding.ref;
    return std::nullopt;
}
std::expected<mc2::sim::MissionReceipt, mc2::sim::OrderError> Mission::submitMissionOrder(
    std::uint64_t session, mc2::sim::OrderEnvelope envelope) {
    MissionOrderApplication application(MissionOrderAccess{*this});
    return orderIngress.submit(session, std::move(envelope), application);
}
void Mission::beginOrderSimulationTick() {
    for (const auto& binding : orderRecipients) resolveOrderRecipient(binding.ref);
    MissionOrderApplication application(MissionOrderAccess{*this});
    const auto result = orderIngress.beginSimulationTick(true, false, MPlayer != nullptr, application);
    if (!result) mc2::fmt::print_err("[ORDER_INGRESS v1] begin_error={}\n", static_cast<int>(result.error()));
    using namespace mc2::sim;
    for (auto& route : routeExecutions) {
        if (route.terminal()) continue;
        if (!routeLive(route)) { signalRoute(route, RouteAction::Removed); continue; }
        auto* pilot = MechWarrior::warriorList[route.pilot];
        const auto team = pilot->getTeam() ? pilot->getTeam()->getId() : -1;
        if (team < 0 || team >= MAX_TEAMS || !MechWarrior::brainsEnabled[team] ||
            ((route.state == RouteState::Active || route.state == RouteState::Pending) &&
             pilot->getTacOrder(ORDERSTATE_GENERAL)->code != static_cast<int>(route.kind))) {
            signalRoute(route, RouteAction::Removed);
            continue;
        }
        if (pilot->getOrderState() != ORDERSTATE_GENERAL || route.state == RouteState::Suspended ||
            !routeMapsReady(pilot)) continue;
        if (route.state == RouteState::Pending && route.awaitingMaps) {
            signalRoute(route, RouteAction::Ready, true);
            if (!route.terminal()) dispatchOwnedRoute(pilot, route);
        } else if (route.state == RouteState::Active && (route.retryPending || route.awaitingMaps)) {
            dispatchOwnedRoute(pilot, route);
        }
    }
}
void Mission::completeOrderSimulationTick() {
    const auto result = orderIngress.completeSimulationTick();
    if (!result) {
        mc2::fmt::print_err("[ORDER_INGRESS v1] completion_error={}\n", static_cast<int>(result.error()));
        return;
    }
    // Mission content owns rejection reporting and completed-receipt retirement.
    auto& queue = orderIngress.queue();
    bool fixtureCompletedThisTick = false;
    for (const auto& receipt : queue.receipts()) {
        if (receipt.applicationStatus == mc2::sim::ApplicationStatus::Pending || receipt.acknowledged) continue;
        const auto sources = queue.sources();
        const auto source = std::ranges::find(sources, receipt.sourceId, &mc2::sim::OrderSource::sourceId);
        if (source == sources.end() || source->kind != mc2::sim::SourceKind::MissionContent) continue;
        if (orderFixtureReceipt && receipt.receiptId <= orderFixtureReceipt->receiptId &&
            receipt.receiptId > orderFixtureReceipt->receiptId - orderFixtureSubmitted) {
            orderFixtureApplied &= receipt.applicationStatus == mc2::sim::ApplicationStatus::Applied;
            ++orderFixtureCompleted;
            fixtureCompletedThisTick = true;
        }
        auto authored = std::ranges::find(authoredRoutes, std::optional{receipt.receiptId}, &mc2::sim::AuthoredRoute::receiptId);
        if (authored != authoredRoutes.end() && receipt.applicationStatus != mc2::sim::ApplicationStatus::Installed && !authored->diagnosed) {
            diagnoseAuthoredRoute(authored->originalPart, authored->selectedPart, authored->pilot, "Rejected", "installation");
            authored->diagnosed = true;
        }
        if (authored == authoredRoutes.end() && receipt.applicationStatus != mc2::sim::ApplicationStatus::Applied)
            mc2::fmt::print_err("[ORDER_INGRESS v1] rejected source={} sequence={} reason={}\n",
                receipt.sourceId, receipt.sequence, receipt.results.empty() ? 0 : receipt.results.front().reasonCode);
        const auto acknowledged = orderIngress.acknowledge({orderIngress.session(), receipt.receiptId});
        if (!acknowledged) mc2::fmt::print_err("[ORDER_INGRESS v1] acknowledgement_error={}\n",
            static_cast<int>(acknowledged.error()));
    }
    queue.retireAcknowledged(queue.nextReceiptId() - 1);
    if (fixtureCompletedThisTick && orderFixtureCompleted == orderFixtureSubmitted)
        mc2::fmt::print_err("[ORDER_INGRESS_FIXTURE v1] status={} completed={} retained={} sequence={}\n",
            orderIngressFixtureApplied() ? "applied_retired" : "failed", orderFixtureCompleted,
            queue.receipts().size(), queue.sources().front().nextSequence);
}
bool Mission::runOrderIngressFixture() {
    using namespace mc2::sim;
    if (!active || (orderFixtureReceipt && !orderIngressFixtureApplied())) {
        mc2::fmt::print_err("[ORDER_INGRESS_FIXTURE v1] event=admission_failed reason=not_ready active={} submitted={} completed={} retained={}\n",
            active != 0, orderFixtureSubmitted, orderFixtureCompleted, orderIngress.queue().receipts().size());
        return false;
    }
    const auto sequence = orderIngress.queue().sources().front().nextSequence;
    if (sequence > maxOrderReceipts) {
        mc2::fmt::print_err("[ORDER_INGRESS_FIXTURE v1] event=admission_failed reason=sequence_exhausted sequence={}\n", sequence);
        return false;
    }
    std::uint32_t unresolved = 0, unowned = 0, disabled = 0, immobile = 0;
    for (const auto& binding : orderRecipients) {
        auto* mover = resolveOrderRecipient(binding.ref);
        if (!mover) { ++unresolved; continue; }
        if (mover->getCommanderId() < 0) { ++unowned; continue; }
        if (mover->isDisabled()) { ++disabled; continue; }
        if (!mover->canMove()) { ++immobile; continue; }
        mc2::fmt::print_err("[ORDER_INGRESS_FIXTURE v1] event=eligible part={} actor_part={} commander={} simulation_time={}\n",
            binding.partId, binding.actorPartId, mover->getCommanderId(), scenarioTime);
        OrderEnvelope envelope;
        envelope.sourceId = 1;
        envelope.issuer = CommanderId{static_cast<std::uint32_t>(mover->getCommanderId())};
        envelope.recipients = {binding.ref};
        envelope.payload.origin = OrderOrigin::Commander;
        envelope.payload.kind = OrderKind::MoveToPoint;
        envelope.payload.move.emplace();
        const auto point = mover->getPosition();
        envelope.payload.move->wayPath.points.push_back({point.x + 50.0f, point.y, point.z});
        envelope.payload.move->wayPath.mode.push_back(TravelMode::Slow);
        // More than the retained-receipt capacity proves that the production consumer recovers space.
        const auto count = std::min<std::uint64_t>(maxPendingOrders, maxOrderReceipts + 1 - sequence);
        orderFixtureSubmitted = orderFixtureCompleted = 0;
        orderFixtureApplied = true;
        for (std::uint64_t i = 0; i < count; ++i) {
            envelope.sequence = sequence + i;
            auto receipt = submitMissionOrder(orderSession(), envelope);
            if (!receipt) {
                orderFixtureApplied = false;
                mc2::fmt::print_err("[ORDER_INGRESS_FIXTURE v1] event=admission_failed reason=queue error={} submitted={} sequence={}\n",
                    static_cast<int>(receipt.error()), orderFixtureSubmitted, envelope.sequence);
                return false;
            }
            orderFixtureReceipt = *receipt;
            ++orderFixtureSubmitted;
        }
        return true;
    }
    mc2::fmt::print_err("[ORDER_INGRESS_FIXTURE v1] event=admission_failed reason=no_eligible_recipient recipients={} unresolved={} unowned={} disabled={} immobile={} simulation_time={}\n",
        orderRecipients.size(), unresolved, unowned, disabled, immobile, scenarioTime);
    return false;
}


static float simSpeedClamp(float v)
{
	if (v < 0.25f) v = 0.25f;
	if (v > 4.0f) v = 4.0f;
	return v;
}

static float s_simSpeedRuntime = -1.0f;   // < 0 = unseeded (read env on first use)

float simSpeedGetMultiplier()
{
	if (s_simSpeedRuntime < 0.0f)
	{
		s_simSpeedRuntime = mc2::gate::get<"MC2_SIM_SPEED">();   // the row clamps to [0.25, 4]
	}
	return s_simSpeedRuntime;
}

void simSpeedSetMultiplier(float v)
{
	const float prev = simSpeedGetMultiplier();
	s_simSpeedRuntime = simSpeedClamp(v);
	mc2::fmt::print_err("[SIM-SPEED v1] multiplier={:.2f} (was {:.2f})\n", (double)s_simSpeedRuntime, (double)prev);
	fflush(stderr);
}

void simSpeedResetToSeed()
{
	s_simSpeedRuntime = -1.0f;   // re-seed from MC2_SIM_SPEED on next read
}


//----------------------------------------------------------------------------------
// Macro Definitions
//#define NUM_FIRERANGES		3

// CP-1: frame counter used in the per-mission shadow reset probe.
// Defined in mclib/tgl.cpp; incremented by GameOS frame-end path.
extern uint32_t g_mc2FrameCounter;

//----------------------------------------------------------------------------------
// Static globals
float minFrameRate = 4.0;
float minFrameLength = 1.0/4.0;

long MissionStartTime =	0;			//No Idea

Mission *mission = nullptr;
unsigned long scenarioResult = mis_PLAYING;
long scenarioEndTurn = -1;
long MechSalvageChance = 100;

long globalPlayerSkills[4];		//Per spec.  Integer value = percentage * 100
long globalPlayerWeapons[4];
long globalEnemySkills[4];
long globalEnemyWeapons[4];

float globalMissionValues[MAX_GLOBAL_MISSION_VALUES];

UserHeapPtr missionHeap = nullptr;

unsigned int MultiPlayTeamId = 0xFFFFFFFF;
unsigned int MultiPlayCommanderId = 0xFFFFFFFF;

bool useSensors = true;
bool useCollisions = true;
long missionLineChanged = 0;
bool GeneralAlarm = false;
#ifndef FINAL
float CheatHitDamage = 0.0f;
#endif

bool neverEndingStory = false;

void GetBlockedDoorCells (int moveLevel, int door, char* openCells);
void PlaceStationaryMovers (MoveMap* map);
void PlaceMovers (void);
//---------------------------------------------------------------------------
void initABL (void);
void closeABL (void);
bool showFrameRate = false;

bool Mission::terminationCounterStarted = false;
double Mission::missionTerminationTime = -1.0;
unsigned long Mission::terminationResult = mis_PLAYING;
// GAMEPLAY-DRIVER-2: see mission.h. mis_PLAYING == unarmed.
unsigned long Mission::forcedTerminationResult = mis_PLAYING;

long GameVisibleVertices		= 500;
float BaseHeadShotElevation		= 1.0f;

bool DisplayCameraAngle = false;

bool loadInMissionSave = false;
bool saveInMissionSave = false;

// Set by the quicksave/quickload boot fixture when both are armed: the game then saves again right after the load, to testgame_resave.ims, so a tool can
// compare that file with the first save (tools/saveload_roundtrip.py resave).
bool g_resaveAfterQuickload = false;

float forcedFrameRate = -1.0f;

// ---- MC2_MISSION_SPLIT: wall-ms split of the Mission::update sub-calls -----
// Locate which logic sub-call owns the 1K-map ~50ms (HITCH logic phase). Default
// OFF, zero behavior change. Emits per-call ns totals + averages at exit (stdout).
//--------------------------------------------------
// Game System Constants -- Definitions here.
float maxVisualRange = 0.0;
unsigned long MaxTreeLOSCellBlock = 0;
float MaxVisualRadius = 0.0;
float fireVisualRange = 0.0;
float baseSensorRange = 0.0;
unsigned char revealTacMap = 0;		//Is Tacmap always revealed?
unsigned char footPrints = 0;		//Do we use/draw footprints?
bool CantTouchThis = false;

long tonnageDivisor = 1.0;			//Amount we divide tons under drop weight by to calc resource bonus
long resourcesPerTonDivided = 0;	//Tons per above divided number

float MineDamage = 0.0;			//Damage mine does per tile
float MineSplashDamage = 0.0;	//Damage mine explosion does to all in tile.
float MineSplashRange = 0.0;	//Range of Splash Damage
long MineExplosion = -1;		//Object ID of mine explosion
long MineLayThrottle = 0;		//Throttle setting of mine Layer
long MineSweepThrottle = 0;		//Throttle setting of mine sweeper
float StrikeWaitTime = 0.0f;	//Time between aerospace Spotter strike calls
float StrikeTimeToImpact = 0.0f;//Time it takes air strike to come in.
float MineWaitTime = 0.0;		//Time Mine Layer must sit in Tile before mine is laid.

long totalSmokeSpheres = 0;			//Maximum number of smoke spheres in world.
long totalSmokeShapeSize = 0;		//Amount we allocate to smoke heap for spheres/textures.

long maxFiresBurning = 0;			//Maximum number of fires allowed in world.
float maxFireBurnTime = 0.0;		//Maximum time a fire can be burning before it dies out.

bool force64MB = TRUE;				//Do we have between 64 and 32 Mb of memory?
bool force32Mb = FALSE;				//Do we have 32Mb of memory only?

float InfluenceRadius = -1.0;		//Capture Radius
float InfluenceTime = 0.0;			//Time inside to Capture

//----------------------------------------------------------------------------------

long Mission::getStatus (void) {

	if (terminationCounterStarted)
		return(terminationResult);
	return(mis_PLAYING);
}

//----------------------------------------------------------------------------------
// GAMEPLAY-DRIVER-2: see the contract comment in mission.h.
void Mission::forceTerminate (unsigned long result) {

	terminationResult = result;
	forcedTerminationResult = result;   // survive the mission brain's per-frame write
	terminationCounterStarted = true;
	// The authored callers use actualTime + 5.0 to let the objective-results
	// GUI play out. Fire immediately instead -- the update() guard still waits
	// on ControlGui::resultsDone(), so the results screen is not skipped, only
	// the 5-second grace is.
	missionTerminationTime = actualTime;
}

//----------------------------------------------------------------------------------

long Mission::render (void)
{
	ZoneScopedN("Camera.UpdateRenderers Mission.render");
	if (active)
	{
		unsigned char tempAmbientLight[3];
		if (KillAmbientLight) {
			tempAmbientLight[0] = eye->ambientRed;
			tempAmbientLight[1] = eye->ambientGreen;
			tempAmbientLight[2] = eye->ambientBlue;
			eye->ambientRed = 0xFF;
			eye->ambientGreen = 0xFF;
			eye->ambientBlue = 0xFF;
		}
		{ ZoneScopedN("Camera.UpdateRenderers eye.render"); eye->render(); }
		{ ZoneScopedN("Camera.UpdateRenderers clearCellDebugs"); GameMap->clearCellDebugs(0); }

		//-----------------------------------------------------
		// FOG time.  Set Render state to FOG on!
		DWORD fogColor = eye->fogColor;
		if (useFog)
		{
			gos_SetRenderState( gos_State_Fog, fogColor);
		}
		else
		{
			gos_SetRenderState( gos_State_Fog, 0);
		}

		gos_SetRenderState( gos_State_Fog, 0);

		{ ZoneScopedN("Camera.UpdateRenderers FloatHelp.renderAll"); FloatHelp::renderAll(); }

		currentFloatHelp = 0;

		if (missionInterface)
		{
			ZoneScopedN("Camera.UpdateRenderers missionInterface.render");
			missionInterface->render();
		}

		if (KillAmbientLight) {
			eye->ambientRed = tempAmbientLight[0];
			eye->ambientGreen = tempAmbientLight[1];
			eye->ambientBlue = tempAmbientLight[2];
		}

		//reset the TGL RAM pools.
		{
			ZoneScopedN("Camera.UpdateRenderers tglPools.reset");
			colorPool->reset();
			vertexPool->reset();
			facePool->reset();
			shadowPool->reset();
			trianglePool->reset();
		}
	}

	return scenarioResult;
}

//----------------------------------------------------------------------------------
float applyDifficultySkill (float chance, bool isPlayer)
{
	if (isPlayer)
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium
			case 2:			//Hard
			case 3:			//Really Hard
				chance *= (float(globalPlayerSkills[GameDifficulty]) / 100.0);
				return(chance);
				break;

			default:
				return (chance);
				break;
		}
	}
	else
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium
			case 2:			//Hard
			case 3:			//Really Hard
				chance *= (float(globalEnemySkills[GameDifficulty]) / 100.0);
				return(chance);
				break;

			default:
				return(chance);
				break;
		}
	}
}

//----------------------------------------------------------------------------------
float applyDifficultyWeapon (float dmg, bool isPlayer)
{
	if (isPlayer)
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium -- NO CHANGE!
			case 2:			//Hard
			case 3:			//Very Hard
				dmg *= (float(globalPlayerWeapons[GameDifficulty]) / 100.0);
				break;
				
			default:
				break;
		}
	}
	else
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium -- NO CHANGE!
			case 2:			//Hard
			case 3:			//Very Hard
				dmg *= (float(globalEnemyWeapons[GameDifficulty]) / 100.0);
				break;
				
			default:
				break;
		}
	}

	//------------------------------------------------------------------
	// Must keep damage an integral part of .25 or bad things happen
	// In theory only needed for Multiplayer but better safe then...
	if (((float)((long)(dmg / 0.25)) * 0.25) != dmg)
	{
		dmg = ((float)((long)(dmg / 0.25)) * 0.25);
	}

	if (dmg < 0.0)
		dmg = 0.0;

	if (dmg > 255.0)
		dmg = 255.0;
		
	return dmg;
}

//----------------------------------------------------------------------------------
void InitDifficultySettings (FitIniFile *gameSystemFile)
{
	long result = gameSystemFile->seekBlock("DifficultySettings");
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "InitDifficultySettings: gamesys.fit missing [DifficultySettings] block");

	result = gameSystemFile->readIdLongArray("PlayerSkills", globalPlayerSkills,4);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "InitDifficultySettings: [DifficultySettings] missing PlayerSkills");

	result = gameSystemFile->readIdLongArray("EnemySkills",globalEnemySkills,4);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "InitDifficultySettings: [DifficultySettings] missing EnemySkills");

	result = gameSystemFile->readIdLongArray("PlayerWeapons",globalPlayerWeapons,4);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "InitDifficultySettings: [DifficultySettings] missing PlayerWeapons");

	result = gameSystemFile->readIdLongArray("EnemyWeapons",globalEnemyWeapons,4);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "InitDifficultySettings: [DifficultySettings] missing EnemyWeapons");
}	

//----------------------------------------------------------------------------------
// Sets mission active.  Assumes that whatever we needed to do to the mission data
// set for logistis has been done and player is ready to play!
void DEBUGWINS_setGameObject (long debugObj, GameObjectPtr obj);
extern long GameObjectWindowList[3];
extern long NumGameObjectsToDisplay;

void Mission::start (void)
{
	active = true;
	mission_phase_mark("mission_ready");
	mission_phase_report(); // LOAD-PHASE-FACTS-1: emit consolidated [LOAD_PHASES v1] line

	// APPEAR-AUDIT-BEGIN (META-FIX design B PART 4 — MC2_APPEAR_AUDIT,
	// default OFF, OBSERVATIONAL-FIRST: the appear_audit smoke bucket stays
	// disarmed until a 24-map failed=0 baseline passes, because whether any
	// object class legitimately reports NULL appearance at mission_ready is
	// unverified. One O(N) pass at mission_ready: every existing registered
	// object either has a non-NULL appearance whose base shape loaded, or
	// emits one classified [APPEAR_AUDIT v1] event=missing line.
	// POSITION-FREE by the same off-map-staging carve-out as design A
	// (INVIS-VEHICLES-FIX-1): staged movers have valid appearances and sit
	// off-map by authored design — this scan keys on NO positional or
	// visibility term (no getPosition, no canBeSeen, no in-view test);
	// check-tgl-load-callers.sh pins this region position-free.)
	{
		static const bool s_appearAudit = mc2::gate::get<"MC2_APPEAR_AUDIT">();
		if (s_appearAudit && ObjectManager)
		{
			long scanned = 0, missing = 0;
			const long maxObjs = ObjectManager->getMaxObjects();
			for (long h = 1; h <= maxObjs; h++)
			{
				GameObjectPtr obj = ObjectManager->get(h);
				if (!obj || !obj->getExists())
					continue;
				scanned++;
				AppearancePtr appear = obj->getAppearance();
				const char* why = nullptr;
				if (!appear)
					why = "appearance_null";
				else
				{
					AppearanceTypePtr at = appear->getAppearanceType();
					if (at && at->baseShapeLoadFailed)
						why = "base_shape_failed";
				}
				if (why)
				{
					missing++;
					if (missing <= 32)   // bounded output; summary carries totals
						mc2AuditLinef("[APPEAR_AUDIT v1] event=missing reason=%s "
							"handle=%ld class=%d part=%ld",
							why, h, (int)obj->getObjectClass(),
							obj->getPartId());
				}
			}
			mc2AuditLinef("[APPEAR_AUDIT v1] summary scanned=%ld missing=%ld",
				scanned, missing);
		}
	}
	// APPEAR-AUDIT-END
	// MISSION-START-HOVER-TARGET-LIFETIME-1: mission world is now fully live (objects loaded,
	// active set). Re-enable hover picking that was suppressed since invalidateHoverTarget().
	if (missionInterface)
		MissionInterfaceManager::armHoverTarget();
	gos_SetHudScaleActive(true);  // enable HUD shrink only during mission
	gos_SetHudCanvasActive(true); // UI-ASPECT-ANCHOR-1: HUD chrome on the 16:9 canvas
	// LOAD-BANNER-RESIDUE-1 (queue #4 floating banner): the ProgressTimer
	// async blit keeps compositing the load-progress art over live mission
	// frames while loadProgress lingers in 1..99 after mission_ready — the
	// ~1s world-floating blue panel at mission start. Load is DONE here;
	// zero the progress so the blit's (0,100) window closes immediately.
	// MP 'waiting for players' uses loadProgress==1000 and is untouched.
	if (!MPlayer)
	{
		extern float loadProgress;
		if (loadProgress > 0.0f && loadProgress < 100.0f)
			loadProgress = 0.0f;
	}
	for (long i = 0; i < NumGameObjectsToDisplay; i++)
		DEBUGWINS_setGameObject(-1, ObjectManager->getByWatchID(parts[GameObjectWindowList[i]].objectWID));
}

//----------------------------------------------------------------------------------
void Mission::initTGLForMission()
{
	ZoneScopedN("Mission::initTGLForMission");
	if (colorPool || vertexPool || facePool || shadowPool || trianglePool)
	{
		ZoneScopedN("Mission::initTGLForMission shutdown");
		logTglPoolPeaks("mission_unload");
		if (colorPool)
		{
			colorPool->destroy();
			delete colorPool;
			colorPool = nullptr;
		}
		
		if (vertexPool)
		{
			vertexPool->destroy();
			delete vertexPool;
			vertexPool = nullptr;
		}

		if (facePool)
		{
			facePool->destroy();
			delete facePool;
			facePool = nullptr;
		}

		if (shadowPool)
		{
			shadowPool->destroy();
			delete shadowPool;
			shadowPool = nullptr;
		}

		if (trianglePool)
		{
			trianglePool->destroy();
			delete trianglePool;
			trianglePool = nullptr;
		}
	}

	loadProgress += 1.0f;

	{
		ZoneScopedN("Mission::initTGLForMission startup");
		// Pools cover detailed model overrides across active blocks, including
		// offscreen static props, so early props cannot exhaust storage for mechs.
		colorPool 		= new TG_VertexPool;
		colorPool->init(32000000);

		vertexPool 		= new TG_GOSVertexPool;
		vertexPool->init(32000000);

		facePool 		= new TG_DWORDPool;
		facePool->init(16000000);

		shadowPool 		= new TG_ShadowPool;
		shadowPool->init(32000000);

		trianglePool 	= new TG_TrianglePool;
		trianglePool->init(16000000);
	}

	loadProgress += 4.0f;

	// Logistics resets the render settings needed by the mission.
	useFog = true;
	useShadows = prefs.useShadows;
}


//----------------------------------------------------------------------------------
// TEAM-COMMANDER-OWNERSHIP-1: the ONE owner / ONE destroy path for the
// Team::teams[]/Commander::commanders[] static arrays. Previously the identical
// free loop was duplicated inline in Mission::init (re-init guard) AND
// Mission::destroy -- two copies that could drift, double-free, or (the live
// hazard) leave Team::home/Commander::home dangling into a freed slot across a
// mission cycle. Both sites now call this single helper.
//
// Contract: idempotent + null-safe. Every live slot up to the recorded count is
// deleted and nulled, the counts are zeroed, and the home aliases are nulled.
// A second back-to-back call is a guaranteed no-op (counts are 0), which is the
// property the MC2_MISSION_CYCLE_TEST probe in destroy() exercises to prove the
// double-free can no longer happen.
void Mission::resetTeamsAndCommanders()
{
	long numt = Team::numTeams;
	for (long i = 0; i < numt; i++)
		if (Team::teams[i]) {
			delete Team::teams[i];
			Team::teams[i] = nullptr;
		}
	Team::numTeams = 0;
	// Null the raw alias into teams[] -- it pointed at a slot just freed above
	// (or was already NULL). Leaving it dangling is the cross-cycle hazard the
	// game-layer review flagged; readers (mission.cpp:659/930/... , update HUD /
	// objectives) guard some but not all dereferences.
	Team::home = nullptr;

	long numC = Commander::numCommanders;
	for (long i = 0; i < numC; i++)
		if (Commander::commanders[i]) {
			delete Commander::commanders[i];
			Commander::commanders[i] = nullptr;
		}
	Commander::numCommanders = 0;
	Commander::home = nullptr;

	// Post-condition: exactly one teardown authority left the statics empty.
	// Silent when TRUE; only the cold path (a leftover live slot / stale count
	// -> a would-be leak or double-free source) logs/stops per MC2_VERIFY_MODE.
	MC2_VERIFY(Team::numTeams == 0 && Commander::numCommanders == 0,
		"resetTeamsAndCommanders: counts not zeroed (teams=%ld commanders=%ld)",
		Team::numTeams, Commander::numCommanders);
}


//----------------------------------------------------------------------------------
void Mission::destroy (bool initLogistics)
{
    orderRecipients.clear();
    authoredRoutes.clear();
    routeExecutions.clear();
    rejectedRouteSignals.clear();
    nextPartIncarnation = nextPilotIncarnation = nextRouteExecution = nextOrderRevision = 1;
    orderFixtureReceipt.reset();
    orderFixtureApplied = false;
    orderFixtureSubmitted = orderFixtureCompleted = 0;
    orderIngress.close();
	mc2verify::MissionSummary(missionFileName);

	gos_SetHudScaleActive(false);
	gos_SetHudCanvasActive(false);

	gpu_cull::readback_shutdown();
	frameJobsShutdown();
	gpu_cull::compute_shutdown();
	gpu_cull::substrate_shutdown();

	GameAdapters::StaticProp::endMissionEarly();
	GpuMechBatcher::instance().onMapUnload();
	GameAdapters::StaticProp::endMissionLate();
	GameAdapters::Mech::endMission();
	GameAdapters::Vegetation::missionUnloaded();

	if (missionInterface)
	{
		MissionInterfaceManager::invalidateHoverTarget();
		missionInterface->destroy();

		delete missionInterface;
		missionInterface = nullptr;
	}

	delete missionBrain;
	missionBrain = nullptr;

	delete missionBrainParams;
	missionBrainParams = nullptr;


	if (eye)
	{
		eye->destroy();

		delete eye;
		eye = nullptr;
		// Camera aliases outlive the camera and must not expose freed matrices during load.
		TG_Shape::s_cameraOrigin = nullptr;
		TG_Shape::s_cameraToClip = nullptr;
	}

	if (PathManager) {
		PathManager->destroy();
		delete PathManager;
		PathManager = nullptr;
	}
	MOVE_cleanup();

	MechWarrior::shutdown();

	delete weather;
	weather = nullptr;

	if (land)
	{
		land->destroy();

		delete land;
		land = nullptr;
	}

	if (ObjectManager)
	{
		ObjectManager->destroy();
		delete ObjectManager;
		ObjectManager = nullptr;
	}


	if (MasterComponent::masterList)
	{
		MasterComponent::freeMasterList();
	}

	if (weaponEffects)
		weaponEffects->destroy();

	delete weaponEffects;
	weaponEffects = nullptr;

	if ( craterManager )
	{
		craterManager->~CraterManager();
		missionHeap->Free( craterManager );
		craterManager = nullptr;
	}


	// Teardown order is documented at the declaration (contact.h): after the objects, before the teams.
	SensorManager.reset();

	Mission::resetTeamsAndCommanders();

	if (Mover::triggerAreaMgr) {
		delete Mover::triggerAreaMgr;
		Mover::triggerAreaMgr = nullptr;
	}

	if (MoverGroup::goalMap)
	{
		systemHeap->Free(MoverGroup::goalMap);
		MoverGroup::goalMap = nullptr;
	}

	closeABL();

	if (missionHeap)
	{
		missionHeap->destroy();

		delete missionHeap;
		missionHeap = nullptr;
	}

	// The registry destructor releases the types, then the Sensor shapes.
	appearanceTypes.reset();
	appearanceTypeList = nullptr;

	if (initLogistics)
		initTGLForLogistics();
	else
	{
		if (colorPool || vertexPool || facePool || shadowPool || trianglePool)
		{
			logTglPoolPeaks("mission_unload");
			if (colorPool)
			{
				colorPool->destroy();
				delete colorPool;
				colorPool = nullptr;
			}

			if (vertexPool)
			{
				vertexPool->destroy();
				delete vertexPool;
				vertexPool = nullptr;
			}

			if (facePool)
			{
				facePool->destroy();
				delete facePool;
				facePool = nullptr;
			}

			if (shadowPool)
			{
				shadowPool->destroy();
				delete shadowPool;
				shadowPool = nullptr;
			}

			if (trianglePool)
			{
				trianglePool->destroy();
				delete trianglePool;
				trianglePool = nullptr;
			}
		}
	}


	active = FALSE;
	scenarioResult = mis_PLAYING;
	GeneralAlarm = false;

	EllipseElement::removeTextureHandle();

	Mover::holdFireIconHandle = 0;

	if (mcTextureManager) {
		mcTextureManager->flush();
		mcTextureManager->freeVertices();
		mcTextureManager->freeShapes();
	}

	if (soundSystem) soundSystem->purgeSoundSystem();

	missionFileName[0] = 0;

	if (CObjective::s_markerFont)
	{
		delete CObjective::s_markerFont;
		CObjective::s_markerFont = nullptr;
	}


}

//----------------------------------------------------------------------------------

void Mission::handleMultiplayMessage (long code, long param1) {

	if (missionBrainCallback) {
		CurMultiplayCode = code;
		CurMultiplayParam = param1;
		missionBrain->execute(nullptr, missionBrainCallback);
		CurMultiplayCode = 0;
		CurMultiplayParam = 0;
	}
}

//----------------------------------------------------------------------------------

void Mission::startObjectiveTimers (void) {

	for (long i=0;i<(long)numObjectives;i++)
	{
		if (objectives[i].timeLeft > 0.0)
		{
			setObjectiveTimer(i,objectives[i].timeLeft);
		}
	}
}

//----------------------------------------------------------------------------------
long Mission::setObjectiveTimer (long objectiveNum, float timeLeft)
{
	gosASSERT((objectiveNum >= 0) && objectiveNum < (long)numObjectives);
		
	long timerNumber = OBJECTIVE_1_TIMER + objectiveNum;
	
	//------------
	// Add Timer.
	TimerPtr timer = timerManager->getTimer(timerNumber);
	gosASSERT(timer != nullptr);
	timer->setTimer(timeLeft);

	return(NO_ERR);
}	

//----------------------------------------------------------------------------------
float Mission::checkObjectiveTimer (long objectiveNum)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);

	long timerNumber = OBJECTIVE_1_TIMER + objectiveNum;
	unsigned long timeLeft = 0;

	TimerPtr timer = timerManager->getTimer(timerNumber);
	gosASSERT(timer != nullptr);

	timeLeft = timer->getCurrentTime();
	
	return(timeLeft);
}	

//----------------------------------------------------------------------------------
long Mission::setObjectiveStatus (long objectiveNum, ObjectiveStatus status)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);
	
	if (objectives)
		objectives[objectiveNum].status = status;
	
	return(NO_ERR);
}	

//----------------------------------------------------------------------------------
bool Mission::checkObjectiveSuccess (void)
{
	//NOW uses NEW(tm) objectives.
	long count = 0;
	bool result = false;
	if (Team::home->objectives.Count())
	{
		for ( CObjective* pObjective : Team::home->objectives )
		{
			//Must iterate through ALL objectives and check.
			result |= pObjective->StatusChangedSuccess();
			count++;
		}
	}
	
	return result;
}

//----------------------------------------------------------------------------------
bool Mission::checkObjectiveFailed (void)
{
	//NOW uses NEW(tm) objectives.
	long count = 0;
	bool result = false;
	if (Team::home->objectives.Count())
	{
		for ( CObjective* pObjective : Team::home->objectives )
		{
			//Must iterate through ALL objectives and check.
			result |= pObjective->StatusChangedFailed();
			count++;
		}
	}
	
	return result;
}

//----------------------------------------------------------------------------------
ObjectiveStatus Mission::checkObjectiveStatus (long objectiveNum)
{
	//NOW uses NEW(tm) objectives.
	long count = 0;
	if (Team::home->objectives.Count())
	{
		for ( CObjective* pObjective : Team::home->objectives )
		{
			if (count == objectiveNum)
			{
				return pObjective->Status(Team::home->objectives);
			}
			count++;
		}
	}
	
	return OS_UNDETERMINED;
}	

//----------------------------------------------------------------------------------
long Mission::setObjectiveType (long objectiveNum, ObjectiveType type)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);

	if (objectives)
		objectives[objectiveNum].type = type;
	
	return(NO_ERR);
}	

//----------------------------------------------------------------------------------
ObjectiveType Mission::checkObjectiveType (long objectiveNum)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);
		
	ObjectiveType result = PrimaryGoal;
	
	if (objectives)
		result = objectives[objectiveNum].type;
	
	return(result);
}	

//----------------------------------------------------------------------------------
void Mission::setObjectivePos (long objectiveNum, float realX, float realY, float realZ)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);
		
	if (objectives)
	{
		objectives[objectiveNum].position.x = realX;
		objectives[objectiveNum].position.y = realY;
		objectives[objectiveNum].position.z = realZ;
	}
}
