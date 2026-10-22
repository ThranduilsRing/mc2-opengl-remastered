#include "combat_sim_hash.h"
#include "objmgr_internal.h"
#include "save_archive.h"

std::uint64_t combatSimulationHash(GameObjectManager& manager)
{
    mc2::save::Archive state;
    for (long i = 1; i <= manager.getMaxObjects(); ++i) {
        auto* object = manager.get(i);
        if (!object || !object->getExists()) continue;
        auto id = object->getPartId();
        auto kind = static_cast<std::uint32_t>(object->getObjectClass());
        auto status = object->getStatus();
        auto position = object->getPosition();
        auto rotation = object->getRotation();
        auto damage = object->getDamage();
        auto tangible = object->getTangible();
        auto falling = object->getFlag(OBJECT_FLAG_FALLING);
        state(i, id, kind, status, position.x, position.y, position.z, rotation, damage, tangible, falling);
        if (auto* bolt = dynamic_cast<WeaponBolt*>(object)) {
            auto hash = bolt->simulationHash(); state(i, hash);
        } else if (auto* explosion = dynamic_cast<Carnage*>(object)) {
            auto tangible = explosion->getTangible();
            state(i, explosion->carnageType, explosion->ownerWID,
                  explosion->info.explosion.timer, explosion->info.explosion.radius,
                  explosion->info.explosion.chunkSize, tangible);
        } else if (auto* artillery = dynamic_cast<Artillery*>(object)) {
            auto boom = artillery->getFlag(OBJECT_FLAG_BOOM);
            state(i, artillery->artilleryType, artillery->teamId, artillery->commanderId,
                  artillery->info.strike.timeToImpact, artillery->info.strike.timeToLaunch,
                  artillery->info.strike.timeToBlind, artillery->info.strike.sensorRange, boom);
        }
    }
    std::uint64_t hash = 14695981039346656037ull;
    for (auto byte : state.bytes()) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}
