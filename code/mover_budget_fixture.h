// M6 synthetic mover budget fixture (dev only). MC2_MOVER_FIXTURE=N tops the mission up to N movers on two teams
// (alternating mech / ground vehicle clones of the mission's own units), then keeps them busy with move and attack
// orders every 2 sim seconds. Run it through tools/mover_budget.py, which reads the [SIM_TICK_COST v1] row.
#pragma once
#include <cstdio>
#include <cmath>
#include "prefs.h"
#include "mc2fmt.h"
extern CPrefs prefs;

inline void mc2MoverBudgetFixtureTick(unsigned turn)
{
    static const long target = mc2::gate::get<"MC2_MOVER_FIXTURE">();
    if (target <= 0 || !ObjectManager || !mission || !Team::home) return;
    static bool spawned = false;
    static unsigned nextOrders = 0;
    if (turn < 3) return;
    if (!spawned) {
        spawned = true;
        MoverPtr tmpl[2] = {nullptr, nullptr};   // [0] mech, [1] ground vehicle
        for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
            MoverPtr m = ObjectManager->getMover(i);
            if (!m) continue;
            if (!tmpl[0] && m->getObjectClass() == BATTLEMECH) tmpl[0] = m;
            if (!tmpl[1] && m->getObjectClass() == GROUNDVEHICLE) tmpl[1] = m;
        }
        if (!tmpl[0]) { mc2::fmt::print_out("[MOVER_FIXTURE v1] result=no_mech_template\n"); return; }
        if (!tmpl[1]) tmpl[1] = tmpl[0];
        long enemyTeam = -1;
        for (long t = 0; t < MAX_TEAMS; t++)
            if (Team::teams[t] && t != Team::home->getId()) { enemyTeam = t; break; }
        if (enemyTeam < 0) enemyTeam = Team::home->getId();
        const glm::vec3 origin = tmpl[0]->getPosition();
        const long start = ObjectManager->getNumMovers();
        long added = 0;
        for (long n = start; n < target; n++) {
            const int side = static_cast<int>(n & 1), cls = static_cast<int>((n >> 1) & 1);
            MoverInitData data; memset(&data, 0, sizeof(data));
            strcpy(data.pilotFileName, "pmp_flash");
            strcpy(data.brainFileName, "pbrain");
            data.rosterIndex = 255; data.gestureID = 2;
            data.active = true; data.exists = true; data.capturable = false;
            data.variant = (cls == 0) ? static_cast<DWORD>(static_cast<BattleMechPtr>(tmpl[0])->variantID) : 0;
            data.baseColor = prefs.baseColor; data.highlightColor1 = data.highlightColor2 = prefs.highlightColor;
            data.commanderID = static_cast<char>(side ? Team::teams[enemyTeam]->getId() : (Commander::home ? Commander::home->getId() : 0));
            data.teamID = static_cast<char>(side ? enemyTeam : Team::home->getId());
            data.objNumber = tmpl[cls]->getTypeHandle();
            data.controlType = CONTROL_AI;
            // Two blocks of 12-wide grids, 60 m apart in x between sides, 40 m pitch.
            const long k = n / 2;
            glm::vec3 pos = origin;
            pos.x += (side ? 300.0f : -300.0f) + static_cast<float>(k % 12) * 40.0f * (side ? 1.0f : -1.0f);
            pos.y += static_cast<float>(k / 12) * 40.0f - 200.0f;
            pos.z = land ? land->getTerrainElevation(pos) : 0.0f;
            data.position = pos; data.rotation = 0;
            const long h = mission->addMover(&data);
            if (h <= 0) break;
            ++added;
        }
        mc2::fmt::print_out("[MOVER_FIXTURE v1] result=spawned requested={:d} base={:d} added={:d} movers={:d}\n",
            mc2::fmt::as_long(target), mc2::fmt::as_long(start), mc2::fmt::as_long(added), mc2::fmt::as_long(static_cast<long>(ObjectManager->getNumMovers())));
        std::fflush(stdout);
        nextOrders = turn + 1;
    }
    if (!nextOrders || turn < nextOrders) return;
    nextOrders = turn + 60;
    const long count = ObjectManager->getNumMovers();
    for (long i = 0; i < count; i++) {
        MoverPtr m = ObjectManager->getMover(i);
        if (!m || m->isDisabled() || m->isDestroyed() || !m->getPilot()) continue;
        MoverPtr best = nullptr; float bestD = 1e30f;
        for (long j = 0; j < count; j++) {
            MoverPtr o = ObjectManager->getMover(j);
            if (!o || o->getTeamId() == m->getTeamId() || o->isDestroyed()) continue;
            const float d = m->distanceFrom(o->getPosition());
            if (d < bestD) { bestD = d; best = o; }
        }
        if (!best) continue;
        if (i & 1) {
            m->getPilot()->orderAttackObject(false, ORDER_ORIGIN_COMMANDER, best, ATTACK_TO_DESTROY,
                ATTACKMETHOD_RANGED, FIRERANGE_OPTIMAL, -1, -1, TACORDER_PARAM_PURSUE);
        } else {
            m->getPilot()->orderMoveToPoint(false, true, ORDER_ORIGIN_COMMANDER, best->getPosition());
        }
    }
}
