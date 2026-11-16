#pragma once
#include "lockstep.h"
#include <functional>
namespace mc2::net {
struct SessionStopped {};
int exitStatus();
[[noreturn]] void stop(std::string_view reason);
// Wire sequence counts every agreed command; queue sequence counts admissions.
std::expected<sim::OrderReceipt, sim::OrderError> admitAgreedOrder(sim::TacticalOrderQueue&,
    sim::OrderEnvelope, const sim::OrderAdmissionValidator&);
void configure(int argc, char** argv);
void redirectLogs();
void openLobby(bool host, std::string_view endpoint, std::string_view mission);
void closeLobby();
const LobbyState& lobbyState();
bool lobbyConnected();
void pumpLobby();
void lobbyReady(bool value);
void lobbyStart();
std::string takeLobbyStart();
bool active();
std::uint32_t localSlot();
void beginMission(std::string_view mission, std::uint32_t seed);
void setRoster(std::vector<sim::ObjectRef> roster, std::vector<std::uint32_t> watchIds);
void combatEvent(std::string_view event, std::uint32_t attacker, std::uint32_t target, float damage);
bool owns(sim::ObjectRef ref, std::uint32_t slot);
bool submitLocal(sim::OrderEnvelope envelope);
using PlayerSubmit = std::function<bool(sim::OrderEnvelope)>;
using TargetLookup = std::function<std::optional<sim::ObjectRef>(std::uint32_t)>;
bool submitScriptOrder(sim::OrderEnvelope envelope, const PlayerSubmit& submit);
void bindQueue(sim::TacticalOrderQueue& queue, std::unique_ptr<sim::OrderAdmissionValidator> validator, PlayerSubmit submit, TargetLookup target);
bool prepareTick(bool due = true);
bool waitingForPeer();
std::uint64_t nextTick();
void stateHashes(std::uint64_t tick, const StateHash& values);
}
