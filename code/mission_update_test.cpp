#include "mission_internal.h"
#include "../GameOS/gameos/diagnostic_trace.h"
// Dev mission test: reports cached objective states and submits normal player orders.
#include "player_unit_intent.h"
#include "brain_special_dispatch.h"
#include "brain_execution_policy.h"

namespace {
long applyPlayerIntent(Mission& mission, const mc2::player::UnitIntent& intent, long captureTarget = 0) {
    auto* object = ObjectManager->getByWatchID(intent.unitWatchId);
    if (!object || !object->isMover()) return 1;
    auto* mover = static_cast<Mover*>(object);
    const bool cancelSupport = mover->refitBuddyWID || mover->recoverBuddyWID;
    const bool powerUp = object->getStatus() == OBJECT_STATUS_SHUTDOWN;
    TacticalOrder order;
    order.init(ORDER_ORIGIN_PLAYER, cancelSupport ? TACTICAL_ORDER_STOP
        : powerUp ? TACTICAL_ORDER_POWERUP : captureTarget ? TACTICAL_ORDER_CAPTURE
        : intent.goal == mc2::player::Goal::Attack ? TACTICAL_ORDER_ATTACK_OBJECT
        : intent.goal == mc2::player::Goal::Repair ? TACTICAL_ORDER_REFIT
        : intent.goal == mc2::player::Goal::Recover ? TACTICAL_ORDER_RECOVER : TACTICAL_ORDER_MOVETO_POINT);
    if (cancelSupport || powerUp) {
        // Service owns shutdown; resume normal commands only after native cancellation/power-up.
    } else if (captureTarget) {
        order.targetWID = captureTarget;
        order.attackParams.type = ATTACK_NONE;
        order.attackParams.method = ATTACKMETHOD_RAMMING;
        order.attackParams.pursue = true;
    } else if (intent.goal == mc2::player::Goal::Recover) {
        order.targetWID = intent.targetWatchId;
        order.attackParams.type = ATTACK_NONE;
        order.attackParams.method = ATTACKMETHOD_RAMMING;
        order.attackParams.pursue = true;
    } else if (intent.goal == mc2::player::Goal::Repair) {
        order.targetWID = intent.targetWatchId;
        order.selectionIndex = -1;
        order.moveParams.faceObject = true;
        order.moveParams.fromArea = -1;
    } else if (intent.goal == mc2::player::Goal::Attack) {
        order.targetWID = intent.targetWatchId;
        order.attackParams.type = ATTACK_TO_DESTROY;
        order.attackParams.method = ATTACKMETHOD_RANGED;
        order.attackParams.range = intent.longRange ? FIRERANGE_LONG : FIRERANGE_OPTIMAL;
        order.attackParams.pursue = intent.posture == mc2::player::Posture::Engage
            && !static_cast<Mover*>(object)->isGuardTower();
    } else {
        order.moveParams.wayPath.numPoints = 1;
        order.moveParams.wayPath.curPoint = 0;
        glm::vec3 location;
        // Give native routing the destination, including detours around blocked terrain.
        if (MoverGroup::calcMoveGoals(intent.location, 1, &location) != 1) return 1;
        order.moveParams.wayPath.points[0] = location.x;
        order.moveParams.wayPath.points[1] = location.y;
        order.moveParams.wayPath.points[2] = location.z;
    }
    order.moveParams.wayPath.mode[0] = TRAVEL_MODE_FAST;
    order.moveParams.wait = false;
    auto* pilot = static_cast<Mover*>(object)->getPilot();
    auto* current = pilot->getCurTacOrder();
    if (current && current->origin == ORDER_ORIGIN_PLAYER && current->code == order.code) {
        if (order.code == TACTICAL_ORDER_MOVETO_POINT) {
            const auto* path = pilot->getMovePath();
            if ((pilot->getMovePathRequest() || (path && path->numSteps > 0))
                && current->moveParams.wayPath.numPoints > 0
                && current->moveParams.wayPath.mode[0] == order.moveParams.wayPath.mode[0]) {
                const glm::vec3 previous{current->moveParams.wayPath.points[0], current->moveParams.wayPath.points[1], 0};
                int oldRow, oldCol, newRow, newCol;
                land->worldToCell(previous, oldRow, oldCol);
                const glm::vec3 requested{order.moveParams.wayPath.points[0], order.moveParams.wayPath.points[1], 0};
                land->worldToCell(requested, newRow, newCol);
                if (oldRow == newRow && oldCol == newCol) return 0;
            }
        } else if (current->targetWID == order.targetWID && (order.code != TACTICAL_ORDER_ATTACK_OBJECT
            || (current->attackParams.method == order.attackParams.method
                && current->attackParams.range == order.attackParams.range
                && current->attackParams.pursue == order.attackParams.pursue))) {
            return 0; // Preserve pursuit, aim and action progress for an unchanged order.
        }
    }
    const long result = mission.submitPlayerOrder(static_cast<Mover*>(object), order, false);
    if (!result && captureTarget)
        mc2::fmt::print_err("[PLAYER_BRAIN] wid={} event=capture target={} x={} y={}\n",
            intent.unitWatchId, captureTarget, object->getPosition().x, object->getPosition().y);
    return result;
}
const char* missionTestStatus(int state) {
    return state == OS_SUCCESSFUL ? "PASS" : state == OS_FAILED ? "FAIL" : "PENDING";
}
}

void Mission::updateMissionTest(bool report) {
    if (!mc2::gate::get<"MC2_PLAY_MISSION">() || MPlayer || !Team::home || !Commander::home) return;
    auto& test = missionTest;
    if (test.finished) return;
    if (!report) {
        if (mc2::gate::get<"MC2_PLAY_MISSION_INVULNERABLE">())
            Team::noPain[Team::home->getId()] = true;
        if (mc2::gate::get<"MC2_PLAY_MISSION_SKIP_CINEMATICS">() && eye && eye->inMovieMode)
            eye->forceMovieToEnd();
        // Dev-only: send the home lance to a world position ("x,y"), re-ordered every ~30 s until it is near.
        // Used to settle "do the enemies react when the player is inside their radius".
        const std::string gotoSpec = mc2::gate::str<"MC2_PLAY_MISSION_GOTO">();
        static int gotoTick = 0;
        float gx = 0.0f, gy = 0.0f;
        if (!gotoSpec.empty() && eye && !eye->inMovieMode && std::sscanf(gotoSpec.c_str(), "%f,%f", &gx, &gy) == 2 &&
            (gotoTick++ % 900) == 0) {
            for (int mi = 0; mi < ObjectManager->numMechs; ++mi) {
                BattleMech* pM = ObjectManager->getMech(mi);
                if (!pM || pM->isDisabled() || pM->isDestroyed() || pM->getTeamId() != Team::home->id) continue;
                MechWarriorPtr pilot = pM->getPilot();
                if (!pilot) continue;
                glm::vec3 goal(gx, gy, 0.0f);
                goal.z = land->getTerrainElevation(goal);
                const glm::vec3 pos = pM->getPosition();
                if (std::hypot(pos.x - gx, pos.y - gy) < 60.0f) continue;
                pilot->orderMoveToPoint(false, true, ORDER_ORIGIN_COMMANDER, goal, -1, TACORDER_PARAM_RUN);
                if (mc2_diag::tagRequested("MISSION_GOTO")) mc2::fmt::print_err("[MISSION_GOTO v1] mech={} from={:.0f},{:.0f} to={:.0f},{:.0f}\n",
                    mi, pos.x, pos.y, gx, gy);
            }
        }
        return;
    }

    int id = 0, passed = 0, failed = 0, pending = 0;
    CObjective* destination = nullptr;
    for (auto* objective : Team::home->objectives) {
        const int state = objective->ReportedStatus();
        if (static_cast<int>(test.objectives.size()) <= id) test.objectives.push_back(-1);
        if (test.objectives[id] != state) {
            mc2::fmt::print_err("[MISSION_OBJECTIVES v1] id={} text_key={} state={} primary={} hidden={} x={} y={}\n",
                id, objective->TitleUseResourceString() ? std::to_string(objective->TitleResourceStringID()) : std::format("fit:{}", id),
                missionTestStatus(state), objective->Priority() == 1,
                objective->IsHiddenTrigger(), objective->MarkerX(), objective->MarkerY());
            test.objectives[id] = state;
        }
        // Hidden triggers are mission scripting machinery, not player objectives.
        if (!objective->IsHiddenTrigger()) {
            passed += state == OS_SUCCESSFUL;
            failed += state == OS_FAILED;
            pending += state == OS_UNDETERMINED;
            if (state == OS_UNDETERMINED && objective->IsActive()
                && (!destination || (objective->Priority() == 1 && destination->Priority() != 1)))
                destination = objective;
        }
        ++id;
    }

    for (long i = 0; i < ObjectManager->getNumMovers(); ++i) {
        auto* mover = ObjectManager->getMover(i);
        if (!mover || !mover->getTeam() || Team::home->isFriendly(mover->getTeam())) continue;
        const long wid = mover->getWatchID();
        auto found = std::ranges::find(test.enemies, wid, &MissionTestState::Enemy::wid);
        if (found == test.enemies.end()) {
            test.enemies.push_back({wid, mover->getPosition()});
            found = std::prev(test.enemies.end());
            mc2::fmt::print_err("[MISSION_ENEMIES v1] event=spawn wid={} count={}\n", wid, test.enemies.size());
        }
        if (!found->moved && glm::distance(found->position, mover->getPosition()) > 5.0f) {
            found->moved = true;
            mc2::fmt::print_err("[MISSION_ENEMIES v1] event=react_move wid={}\n", wid);
        }
        if (!found->dead && (mover->isDestroyed() || mover->isDisabled())) {
            found->dead = true;
            mc2::fmt::print_err("[MISSION_ENEMIES v1] event=disabled_or_dead wid={}\n", wid);
        }
        float health = 0;
        for (int loc = 0; loc < mover->numArmorLocations; ++loc) health += mover->armor[loc].curArmor;
        for (int loc = 0; loc < mover->numBodyLocations; ++loc) health += mover->body[loc].curInternalStructure;
        if (!found->damaged && found->health >= 0 && health < found->health) {
            found->damaged = true;
            mc2::fmt::print_err("[MISSION_ENEMIES v1] event=damage wid={}\n", wid);
        }
        found->health = health;
        if (!found->destroyed && mover->isDestroyed()) {
            found->destroyed = true;
            mc2::fmt::print_err("[MISSION_ENEMIES v1] event=destroyed wid={}\n", wid);
        }
    }
    const bool timeout = scenarioTime >= mc2::gate::get<"MC2_PLAY_MISSION_SECONDS">();
    if (terminationCounterStarted || scenarioResult != mis_PLAYING || terminationResult == 9999 || timeout) {
        const bool ended = terminationCounterStarted || scenarioResult != mis_PLAYING || terminationResult == 9999;
        const bool won = ended && (terminationResult == mis_PLAYER_WIN_BIG || terminationResult == mis_PLAYER_WIN_SMALL
            || scenarioResult == mis_PLAYER_WIN_BIG || scenarioResult == mis_PLAYER_WIN_SMALL);
        int losses = 0, survivors = 0;
        for (long i = 0; i < Team::home->getRosterSize(); ++i) {
            auto* unit = Team::home->getMover(i);
            if (!unit || unit->getCommanderId() != Commander::home->getId()) continue;
            if (!unit->getExists() || unit->isDisabled() || unit->isDestroyed()) ++losses;
            else ++survivors;
        }
        mc2::fmt::print_err("[MISSION_VERDICT v1] verdict={} passed={} failed={} pending={} time={:.2f} enemies={} reacted={} disabled_or_dead={} damaged={} destroyed={} losses={} survivors={}\n",
            won ? "PASS" : timeout ? "TIMEOUT" : "FAIL", passed, failed, pending, scenarioTime, test.enemies.size(),
            std::ranges::count(test.enemies, true, &MissionTestState::Enemy::moved),
            std::ranges::count(test.enemies, true, &MissionTestState::Enemy::dead),
            std::ranges::count(test.enemies, true, &MissionTestState::Enemy::damaged),
            std::ranges::count(test.enemies, true, &MissionTestState::Enemy::destroyed), losses, survivors);
        test.finished = true;
        return;
    }
    if (!destination || scenarioTime < test.nextOrder) return;
    test.nextOrder = scenarioTime + 2.0f;
    glm::vec3 location{destination->MarkerX(), destination->MarkerY(), 0.0f};
    GameObjectPtr objectiveTarget = nullptr;
    bool captureObjective = false;
    bool extracting = false;
    for (auto* condition : destination->m_conditionList) {
        if (condition->Status() == OS_SUCCESSFUL) continue;
        auto point = condition->GetObjectivePosition();
        if (condition->Species() == DESTROY_ALL_ENEMY_UNITS
            || condition->Species() == DESTROY_NUMBER_OF_ENEMY_UNITS) {
            // These conditions have no marker or specific unit ID to navigate to.
            for (long i = 0; i < ObjectManager->getNumMovers(); ++i) {
                auto* enemy = ObjectManager->getMover(i);
                if (!enemy || !enemy->getTeam() || Team::home->isFriendly(enemy->getTeam())
                    || !enemy->getExists() || enemy->isDisabled() || enemy->isDestroyed()) continue;
                objectiveTarget = enemy;
                location = enemy->getPosition();
                break;
            }
            break;
        }
        // Unmarked unit objectives have no navigation marker (often 0,0).
        if (auto* specific = dynamic_cast<CSpecificUnitObjectiveCondition*>(condition)) {
            if (!objectiveTarget && (condition->Species() == DESTROY_SPECIFIC_ENEMY_UNIT
                || condition->Species() == CAPTURE_UNIT)) {
                objectiveTarget = ObjectManager->getByWatchID(specific->UnitWatchID());
                captureObjective = condition->Species() == CAPTURE_UNIT;
                if (objectiveTarget && (captureObjective || !destination->DisplayMarker()))
                    location = objectiveTarget->getPosition();
            }
            continue;
        }
        // Work on the first unfinished condition with a position, rather than
        // repeatedly walking to the last condition or an already visited marker.
        if (point.x <= -999000.0f || point.y <= -999000.0f) continue;
        location = point;
        extracting = condition->Species() == MOVE_ALL_SURVIVING_MECHS_TO_AREA
            || condition->Species() == MOVE_ALL_SURVIVING_UNITS_TO_AREA
            || condition->Species() == MOVE_ALL_UNITS_TO_AREA;
        captureObjective = condition->Species() == CAPTURE_STRUCTURE;
        if (condition->Species() == DESTROY_SPECIFIC_STRUCTURE || captureObjective) {
            for (long i = 0; i < ObjectManager->getNumBuildings(); ++i) {
                auto* building = ObjectManager->getBuilding(i);
                if (building && building->getExists() && !building->isDestroyed()
                    && glm::distance(building->getPosition(), point) < 1.0f)
                    objectiveTarget = building;
            }
        }
        break;
    }
    // Stable four-unit squads survive casualties; support units follow the nearest squad.
    std::vector<MoverPtr> units;
    bool hasRepair = false, needsRepair = false;
    for (long i = 0; i < Team::home->getRosterSize(); ++i) {
        auto* mover = Team::home->getMover(i);
        if (!mover || mover->getCommanderId() != Commander::home->getId() || !mover->getExists()
            || mover->isDisabled() || mover->isDestroyed() || !mover->getPilot()) continue;
        units.push_back(mover);
        hasRepair |= mover->isRefit() && mover->getRefitPoints() > 0;
        float armor = 0, total = 0;
        for (int loc = 0; loc < mover->numArmorLocations; ++loc) {
            armor += mover->armor[loc].curArmor;
            total += mover->armor[loc].maxArmor;
        }
        needsRepair |= mover->getObjectClass() == BATTLEMECH && mover->needsRefit()
            && (total <= 0 || armor < total * 0.85f);
        if (!mover->isRefit() && !mover->isRecover() && !mover->isGuardTower()
            && std::ranges::find(test.squadMembers, mover->getWatchID()) == test.squadMembers.end())
            test.squadMembers.push_back(mover->getWatchID());
    }
    std::vector<mc2::player::SquadIntent> squads(std::max<std::size_t>(1, (test.squadMembers.size() + 3) / 4));
    std::vector<int> members(squads.size());
    const auto squadIndex = [&](MoverPtr mover) {
        const auto found = std::ranges::find(test.squadMembers, mover->getWatchID());
        return found == test.squadMembers.end() ? std::size_t{0}
            : static_cast<std::size_t>(found - test.squadMembers.begin()) / 4;
    };
    for (auto* mover : units) {
        if (mover->isRefit() || mover->isRecover() || mover->isGuardTower()) continue;
        const auto index = squadIndex(mover);
        auto& squad = squads[index];
        squad.id = static_cast<unsigned>(index);
        if (!squad.leaderWatchId) squad.leaderWatchId = mover->getWatchID();
        squad.rally += mover->getPosition();
        ++members[index];
    }
    for (std::size_t i = 0; i < squads.size(); ++i) {
        auto& squad = squads[i];
        squad.extracting = extracting;
        if (members[i]) squad.rally /= static_cast<float>(members[i]);
        float best = -1;
        for (auto* mover : units) {
            if (squadIndex(mover) != i || mover->isRefit() || mover->isRecover()) continue;
            std::array<int, MAX_CONTACTS_PER_TEAM> contacts{};
            const long count = mover->getContacts(contacts.data(), (1 | 64), CONTACT_SORT_DISTANCE);
            for (long c = 0; c < count; ++c) {
                auto* enemy = ObjectManager->get(contacts[c]);
                if (!enemy || !enemy->getExists() || enemy->isDisabled() || enemy->isDestroyed()
                    || (captureObjective && enemy == objectiveTarget)
                    || !mover->isEnemy(enemy->getTeam()) || !mover->lineOfSight(enemy)) continue;
                const float distance = mover->distanceFrom(enemy->getPosition());
                if (distance > std::max(300.0f, mover->getMaxFireRange())) continue;
                const auto route = location - mover->getPosition();
                const float fraction = std::clamp(glm::dot(enemy->getPosition() - mover->getPosition(), route)
                    / std::max(1.0f, glm::dot(route, route)), 0.0f, 1.0f);
                if (extracting) {
                    if (glm::distance(enemy->getPosition(), location) >= MaxExtractUnitDistance) continue;
                } else if (enemy != objectiveTarget && (distance > std::max(200.0f, mover->getMaxFireRange())
                    || glm::distance(enemy->getPosition(), mover->getPosition() + fraction * route) > 400)) continue;
                float remaining = 0, total = 0;
                if (enemy->isMover()) {
                    auto* target = static_cast<Mover*>(enemy);
                    for (int loc = 0; loc < target->numBodyLocations; ++loc) {
                        remaining += target->body[loc].curInternalStructure;
                        total += target->body[loc].maxInternalStructure;
                    }
                }
                const float condition = total > 0 ? remaining / total : 1.0f;
                const float score = (1.0f + enemy->getThreatRating())
                    / ((100.0f + distance) * (0.5f + 0.5f * condition))
                    + (enemy == objectiveTarget && !captureObjective ? 1000.0f : 0.0f);
                if (score > best) { best = score; squad.targetWatchId = enemy->getWatchID(); }
            }
        }
        mc2::fmt::print_err("[PLAYER_SQUAD v1] time={:.2f} squad={} leader={} members={} focus={} x={:.1f} y={:.1f}\n",
            scenarioTime, i, squad.leaderWatchId, members[i], squad.targetWatchId, squad.rally.x, squad.rally.y);
    }
    if (!extracting && !units.empty() && scenarioTime >= test.nextSupport) {
        auto* gui = MissionInterfaceManager::instance();
        auto position = units.front()->getPosition();
        float ownStrength = 0, enemyStrength = 0;
        for (auto* ally : units)
            if (!ally->isRefit() && !ally->isRecover() && ally->distanceFrom(position) < 600)
                ownStrength += std::max(1.0f, static_cast<float>(ally->getThreatRating()));
        for (const auto& contact : test.enemies) {
            auto* enemy = ObjectManager->getByWatchID(contact.wid);
            if (enemy && enemy->getExists() && !enemy->isDisabled() && !enemy->isDestroyed()
                && enemy->distanceFrom(position) < 600 && Team::home->teamLineOfSight(enemy->getPosition(), 0.0f))
                enemyStrength += std::max(1.0f, static_cast<float>(enemy->getThreatRating()));
        }
        const bool outmatched = enemyStrength > ownStrength * 0.75f;
        const auto safe = [&](const glm::vec3& point, float radius) {
            for (const auto& contact : test.enemies) {
                auto* enemy = ObjectManager->getByWatchID(contact.wid);
                if (enemy && enemy->getExists() && !enemy->isDisabled() && !enemy->isDestroyed()
                    && Team::home->teamLineOfSight(enemy->getPosition(), 0.0f)
                    && enemy->distanceFrom(point) < radius) return false;
            }
            return true;
        };
        bool purchased = false;
        if (gui && (needsRepair || scenarioTime < 10) && !hasRepair && safe(position, 250))
            purchased = gui->requestPlayerSupport(ControlGui::REPAIR_VEHICLE, position);
        const bool lostUnit = std::ranges::any_of(test.squadMembers, [](long wid) {
            auto* unit = ObjectManager->getByWatchID(wid);
            return !unit || !unit->getExists() || unit->isDisabled() || unit->isDestroyed();
        });
        if (gui && !purchased && lostUnit) {
            for (long i = 0; i < ObjectManager->getNumMovers(); ++i) {
                auto* wreck = ObjectManager->getMover(i);
                if (!wreck || !wreck->getExists() || !wreck->isDisabled() || wreck->isDestroyed()
                    || wreck->getObjectClass() != BATTLEMECH || wreck->recoverBuddyWID
                    || !safe(wreck->getPosition(), 300)) continue;
                if (gui->requestPlayerSupport(ControlGui::RECOVERY_TEAM, wreck->getPosition(), wreck)) {
                    purchased = true;
                    break;
                }
            }
        }
        if (gui && !purchased && outmatched) {
            for (const auto& squad : squads) {
                auto* target = ObjectManager->getByWatchID(squad.targetWatchId);
                if (!target) continue;
                bool clear = true;
                for (long i = 0; i < ObjectManager->getNumMovers(); ++i) {
                    auto* ally = ObjectManager->getMover(i);
                    if (ally && ally->getTeam() && Team::home->isFriendly(ally->getTeam())
                        && ally->getExists() && !ally->isDestroyed()
                        && ally->distanceFrom(target->getPosition()) < 150) clear = false;
                }
                if (clear && gui->requestPlayerSupport(ControlGui::LARGE_AIRSTRIKE, target->getPosition())) {
                    purchased = true;
                    test.airstrikePosition = target->getPosition();
                    test.airstrikeUntil = scenarioTime + 15.0f;
                    break;
                }
            }
        }
        if (gui && !purchased && outmatched && !test.artilleryOrdered && safe(position, 100)) {
            purchased = gui->requestPlayerSupport(ControlGui::GUARD_TOWER, position);
            test.artilleryOrdered = purchased;
        }
        mc2::fmt::print_err("[PLAYER_RESOURCES v1] time={:.2f} rp={} repair_present={} repair_needed={} purchased={} allow_repair={} allow_salvage={} allow_air={} allow_arty={}\n",
            scenarioTime, LogisticsData::instance->getResourcePoints(), hasRepair, needsRepair, purchased,
            LogisticsData::instance->canHaveRepairTruck(), LogisticsData::instance->canHaveSalavageCraft(),
            LogisticsData::instance->canHaveAirStrike(), LogisticsData::instance->canHaveArtilleryPiece());
        test.nextSupport = scenarioTime + (purchased ? 20.0f : 6.0f);
    }
    int issued = 0, rejected = 0, retreating = 0;
    long focus = 0;
    for (long i = 0; i < Team::home->getRosterSize(); ++i) {
        auto* mover = Team::home->getMover(i);
        if (!mover || mover->getCommanderId() != Commander::home->getId() || mover->isDisabled()
            || !mover->getExists() || !mover->getPilot()) continue;
        if (!extracting && (mover->refitBuddyWID || mover->recoverBuddyWID)) {
            auto* buddy = ObjectManager->getByWatchID(mover->refitBuddyWID
                ? mover->refitBuddyWID : mover->recoverBuddyWID);
            auto* service = mover->isRefit() || mover->isRecover() ? mover
                : buddy && buddy->isMover() ? static_cast<Mover*>(buddy) : nullptr;
            const auto* order = service && service->getPilot() ? service->getPilot()->getCurTacOrder() : nullptr;
            if (buddy && buddy->getExists() && !buddy->isDestroyed() && order
                && (order->code == TACTICAL_ORDER_RECOVER
                    || (order->code == TACTICAL_ORDER_REFIT && !buddy->isDisabled()))) continue;
        }
        auto index = squadIndex(mover);
        if (mover->isRefit() || mover->isRecover()) {
            float nearest = 1e30f;
            for (std::size_t j = 0; j < squads.size(); ++j) {
                const float distance = mover->distanceFrom(squads[j].rally);
                if (members[j] && distance < nearest) { nearest = distance; index = j; }
            }
        }
        auto intent = producePlayerIntent(mover->getPilot(), location,
            extracting ? squads[index].targetWatchId
                : objectiveTarget && !captureObjective ? objectiveTarget->getWatchID() : 0, squads[index], scenarioTime);
        if (captureObjective && objectiveTarget && intent.targetWatchId == objectiveTarget->getWatchID()) {
            intent = {static_cast<long>(mover->getWatchID()), mc2::player::Goal::Move, location};
            intent.posture = mc2::player::Posture::Advance;
        }
        // Keep the force outside its own pending strike, including moving/capturing units.
        if (scenarioTime < test.airstrikeUntil && !mover->isRefit() && !mover->isRecover()
            && !mover->isGuardTower()) {
            const auto away = mover->getPosition() - test.airstrikePosition;
            intent = {static_cast<long>(mover->getWatchID()), mc2::player::Goal::Move, mover->getPosition()};
            intent.posture = mc2::player::Posture::Retreat;
            if (mover->distanceFrom(test.airstrikePosition) < 150 && glm::length(away) > 0)
                intent.location += glm::normalize(away) * 1200.0f;
        }
        focus = squads[index].targetWatchId;
        float armor = 0, maxArmor = 0, structure = 0, maxStructure = 0;
        for (int loc = 0; loc < mover->numArmorLocations; ++loc) {
            armor += mover->armor[loc].curArmor;
            maxArmor += mover->armor[loc].maxArmor;
        }
        for (int loc = 0; loc < mover->numBodyLocations; ++loc) {
            structure += mover->body[loc].curInternalStructure;
            maxStructure += mover->body[loc].maxInternalStructure;
        }
        auto* chosen = ObjectManager->getByWatchID(intent.targetWatchId);
        mc2::fmt::print_err("[PLAYER_PILOT v1] time={:.2f} wid={} goal={} posture={} target={} distance={:.1f} range={:.1f} armor={:.2f} structure={:.2f} refit={} points={} buddy={} rp={} x={:.1f} y={:.1f} squad={} leader={}\n",
            scenarioTime, mover->getWatchID(), static_cast<int>(intent.goal), static_cast<int>(intent.posture),
            intent.targetWatchId, chosen ? mover->distanceFrom(chosen->getPosition()) : -1.0f,
            mover->getMaxFireRange(), maxArmor > 0 ? armor / maxArmor : 1.0f,
            maxStructure > 0 ? structure / maxStructure : 1.0f, mover->isRefit(),
            mover->getRefitPoints(), mover->refitBuddyWID, LogisticsData::instance->getResourcePoints(),
            mover->getPosition().x, mover->getPosition().y, index, squads[index].leaderWatchId);
        auto* runtime = mover->getPilot()->getBrainRuntime();
        const bool recovering = runtime->playerRecoverUntil > scenarioTime;
        auto* current = mover->getPilot()->getCurTacOrder();
        const bool healthRetreat = intent.posture == mc2::player::Posture::Retreat && runtime->retreatEffectApplied;
        if (captureObjective && objectiveTarget && !recovering && !healthRetreat
            && scenarioTime >= test.airstrikeUntil
            && current && current->code == TACTICAL_ORDER_CAPTURE && current->targetWID == objectiveTarget->getWatchID()) {
            ++issued; // Let the normal capture action finish before taking another side fight.
            continue;
        }
        retreating += intent.posture == mc2::player::Posture::Retreat;
        const long capture = captureObjective && objectiveTarget
            && intent.goal == mc2::player::Goal::Move && intent.posture == mc2::player::Posture::Advance
            && !mover->isRefit() && !mover->isRecover() && !mover->isGuardTower()
            && !recovering && glm::distance(mover->getPosition(), location) < 400 ? objectiveTarget->getWatchID() : 0;
        const long result = applyPlayerIntent(*this, intent, capture);
        if (result) ++rejected;
        else ++issued;
        mc2::fmt::print_err("[PLAYER_ORDER v1] time={:.2f} wid={} result={} long_range={} capture={}\n",
            scenarioTime, mover->getWatchID(), result, intent.longRange, capture);
    }
    mc2::fmt::print_err("[MISSION_COMMANDER v1] producer=TechBrain time={:.2f} issued={} rejected={} retreating={} focus={} goal_target={} x={} y={}\n",
        scenarioTime, issued, rejected, retreating, focus, objectiveTarget ? objectiveTarget->getWatchID() : 0, location.x, location.y);
}

