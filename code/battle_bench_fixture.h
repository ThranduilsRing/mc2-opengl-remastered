// Real attack orders and world counters for the versioned battle fixture.
#pragma once
#include "../mclib/battle_bench_sink.h"
#include "bldng.h"
#include "turret.h"
#include <unordered_set>

inline void mc2BattleBenchObserve() {
    using namespace mc2::battlebench;
    if (!output || !ObjectManager) return;
    static float nextSample = -1, nextAttack = -1;
    static std::unordered_set<long> destroyed;
    if (attack && scenarioTime >= nextAttack && Team::home) {
        nextAttack = scenarioTime + 2.0f;
        int issued = 0;
        for (long i = 0; i < ObjectManager->getNumMovers(); ++i) {
            auto* m = ObjectManager->getMover(i);
            if (!m || m->getTeamId() != Team::home->id || m->isDisabled() || m->isDestroyed() || !m->getPilot()) continue;
            GameObjectPtr target = nullptr;
            float best = 1e30f;
            const auto consider = [&](GameObjectPtr o) {
                if (!o || !o->getExists() || o->isDestroyed() || o->getTeamId() == Team::home->id || o->getTeamId() < 0) return;
                const float d = m->distanceFrom(o->getPosition());
                if (d < best) { best = d; target = o; }
            };
            for (long j = 0; j < ObjectManager->getNumMovers(); ++j) consider(ObjectManager->getMover(j));
            for (long j = 0; j < ObjectManager->getNumTurrets(); ++j) consider(ObjectManager->getTurret(j));
            // Every fourth mech attacks a destructible base building, exercising damage and rebaking.
            if (i % 4 == 0) {
                target = nullptr; best = 1e30f;
                for (long j = 0; j < ObjectManager->getNumBuildings(); ++j) consider(ObjectManager->getBuilding(j));
            }
            if (target) {
                m->getPilot()->orderAttackObject(false, ORDER_ORIGIN_COMMANDER, target,
                    ATTACK_TO_DESTROY, ATTACKMETHOD_RANGED, FIRERANGE_OPTIMAL, -1, -1, TACORDER_PARAM_PURSUE);
                ++issued;
            }
        }
        std::fprintf(output, "{\"type\":\"orders\",\"sim_s\":%.6f,\"attacks\":%d}\n", double(scenarioTime), issued);
    }
    if (scenarioTime < nextSample) return;
    nextSample = scenarioTime + 0.5f;
    int alive = 0, mechs = 0, active = 0;   // active = alive and moving (speed > 0); "firing" is not exposed on the mover
    const auto count = [&](GameObjectPtr o) {
        if (o && o->isDestroyed()) destroyed.insert(o->getPartId());
    };
    for (long i = 0; i < ObjectManager->getNumMovers(); ++i) {
        auto* m = ObjectManager->getMover(i); count(m);
        if (m && m->getExists() && !m->isDestroyed() && !m->isDisabled()) ++alive;
        if (m && m->getObjectClass() == BATTLEMECH) ++mechs;
        if (m && m->getExists() && !m->isDestroyed() && !m->isDisabled() && m->getSpeed() > 0.0f) ++active;
    }
    for (long i = 0; i < ObjectManager->getNumBuildings(); ++i) count(ObjectManager->getBuilding(i));
    for (long i = 0; i < ObjectManager->getNumTurrets(); ++i) count(ObjectManager->getTurret(i));
    for (long i = 0; i < ObjectManager->getNumTerrainObjects(); ++i) count(ObjectManager->getTerrainObject(i));
    std::fprintf(output, "{\"type\":\"world\",\"frame\":%llu,\"sim_s\":%.6f,\"movers\":%ld,\"units_alive\":%d,\"mechs\":%d,\"active\":%d,\"destroyed\":%zu}\n",
        static_cast<unsigned long long>(frame), double(scenarioTime), ObjectManager->getNumMovers(), alive, mechs, active, destroyed.size());
}
