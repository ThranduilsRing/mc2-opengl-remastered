#pragma once
#include "mc2_czstring.h"
#include <cstdint>

namespace gpu_cull {

// Call once per emitted record, BEFORE substrate_submitDynamicActor.
// Compares the record's worldCenter against the actor's position.
// Env-gated: MC2_GPU_CULL_AABB_PARITY=1.
// Parameters:
//   actorId   — actor handle (for log)
//   catName   — short string: "Mech", "GV", "Gate", "Turret", "Other"
//   recCenter — worldCenter[3] from the GpuActorRecord (raw MC2 world coords)
//   posX, posY, posZ — actor's position (MC2 world coords, same space as recCenter)
void parity_checkRecord(uint32_t actorId, mc2::czstring catName,
                        const float recCenter[3],
                        float posX, float posY, float posZ);

// Emit 600-frame summary. Call from substrate_flushUpload or a dedicated site.
void parity_flushSummary();

// Returns true if parity checking is enabled.
bool parity_isEnabled();

} // namespace gpu_cull
