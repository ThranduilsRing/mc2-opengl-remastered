// code/impact_presentation.h -- the bridge between simulation impacts and the impact decal renderer.
//
// Simulation side (onWeaponImpact / onExplosion): called where the sim resolves a shot or an explosion. They only push a
// tick-tagged event into a bounded presentation queue; nothing in the simulation ever reads it back, and no RNG is drawn.
// Presentation side (frame / reset): once per rendered frame the queue is drained into the decal pool (anchors resolved
// read-only through the object manager), aged, and handed to the Vulkan pass. See docs/impact-damage-design.md.
#pragma once

#include "math/glm_config.h"

namespace impact_presentation {

// A bolt reached its target or the ground. masterId = the firing weapon's master component; targetWID = 0 for a miss.
void onWeaponImpact(long masterId, bool isBeam, bool areaEffect, float damage,
                    const glm::vec3& from, const glm::vec3& hit, unsigned long targetWID);

// An explosion that is not a weapon bolt's own impact (mines, vehicle/mech/building deaths, airstrikes).
void onExplosion(long effectId, const glm::vec3& pos, float radius);

// Once per rendered frame, before the render lists are flushed.
void frame();

// Mission start / load: drop every decal and pending event.
void reset();

}  // namespace impact_presentation
