#pragma once
#include "lockstep.h"
namespace mc2::net {
struct SessionStopped {};
int exitStatus();
// Wire sequence counts every agreed command; queue sequence counts admissions.
std::expected<sim::OrderReceipt, sim::OrderError> admitAgreedOrder(sim::TacticalOrderQueue&,
    sim::OrderEnvelope, const sim::OrderAdmissionValidator&);
void configure(int argc, char** argv);
void redirectLogs();
bool active();
std::uint32_t localSlot();
void beginMission(std::string_view mission, std::uint32_t seed);
void setRoster(std::vector<sim::ObjectRef> roster);
bool owns(sim::ObjectRef ref, std::uint32_t slot);
bool submitLocal(sim::OrderEnvelope envelope);
void bindQueue(sim::TacticalOrderQueue& queue, std::unique_ptr<sim::OrderAdmissionValidator> validator);
bool prepareTick();
std::uint64_t nextTick();
void stateHashes(std::uint64_t tick, const StateHash& values);
}
