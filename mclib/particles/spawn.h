//==========================================================================//
// File:    spawn.h                                                          //
// Contents: mc2::particles::Spawn - polymorphic spawn dispatcher routing    //
//           by gosFX spec ClassID to the per-primitive SpawnCard /          //
//           SpawnCardCloud / SpawnPoint / SpawnShard / SpawnTube entry      //
//           points.                                                         //
//                                                                           //
//           Per integrated plan v6 §5.4 B1 Stage 2' C8.                     //
//                                                                           //
//           Consumer (C8): the four primitive gosFX subclass Start()        //
//           methods (Card / PointCloud / ShardCloud / Tube) call Spawn()    //
//           after Effect::Start has resolved m_seed / m_localToWorld,       //
//           under the MC2_GPU_PARTICLES=1 gate. This catches both top-      //
//           level direct spawns AND children-inside-composites (an          //
//           EffectCloud parent iterates and calls each child Start, which   //
//           lands here). The C7 EffectAdapter-at-MakeEffect boundary was    //
//           retired because composite parents bypass MakeEffect for their   //
//           children, leaving the new pipeline starved.                     //
//                                                                           //
//           Pert / Shape / Debris / EffectCloud are NOT dispatched today    //
//           (no Stage 2' Spawn* implementation); Spawn() returns false for  //
//           those ClassIDs. Stage 0' recon shows these primitives never     //
//           spawn in tier1 stock content, so the visual gap is acknowledged //
//           and the slots are filed as B2 polish debt.                      //
//===========================================================================//

#pragma once

#include "gosfx/effect.hpp"

#include "../math/glm_config.h"

namespace mc2 {
namespace particles {

// Polymorphic dispatcher. Routes by spec->GetClassID() to one of the four
// implemented per-primitive entry points; returns false (no-op) for the
// four deferred primitive classes.
//
// Inputs:
//   spec           - non-null gosFX::Effect::Specification*. Class is read
//                    via GetClassID() (which Check_Object's the spec).
//   parentToWorld  - parent transform; passed straight through. May be null
//                    only if the underlying SpawnX permits it.
//   spawnSeed      - per-instance random seed in [0..1]. Producers without
//                    a seed should pass 0.5f (median).
//   callerAge      - VFX-AGE-SAMPLE-1: the effect's real normalized age
//                    (gosFX Effect::m_age, 0..1) at the Draw call. Used to
//                    sample spec curves only when MC2_VFX_AGE_SAMPLE=1;
//                    otherwise (and for invalid/out-of-range age) the fixed
//                    0.5 midpoint is used. Defaults to 0.5f for callers that
//                    do not supply an age (byte-identical legacy behavior).
//
// Returns:
//   true  - spec was dispatched to a Spawn* entry point (Card / PointCloud /
//           ShardCloud / Tube).
//   false - spec is null OR its ClassID is one of Pert / Shape / Debris /
//           EffectCloud (B2 deferred) OR an unknown subclass.
bool Spawn(gosFX::Effect::Specification* spec,
           const glm::mat4*  parentToWorld,
           float                          spawnSeed,
           float                          callerAge = 0.5f);

// VFX-AGE-SAMPLE-1: resolve the curve-sample age for a spawn. Returns 0.5f
// when MC2_VFX_AGE_SAMPLE is OFF (default) or when callerAge is invalid
// (NaN / sentinel -1 / outside [0,1]); otherwise returns callerAge. Single
// source of truth shared by every Spawn* primitive so gate-OFF is exactly the
// pre-slice fixed-midpoint behavior. Look-only — no gameplay/timing effect.
float resolveSampleAge(float callerAge);

}  // namespace particles
}  // namespace mc2
