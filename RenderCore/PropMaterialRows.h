#pragma once
#include "PropMaterialGpu.h"
#include <cstddef>
#include <span>

namespace RenderCore {
// Per-object rows index the per-object texture table. When that table is not resident (one slot on Low/Potato, or more maps than the
// device has slots) those indices mean nothing to the shader: drop the flag and the indices so the row falls back to class scalars.
// Returns the number of rows changed.
inline std::size_t dropPerTextureRows(std::span<PropMaterialGpu> rows) {
    std::size_t changed = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto& m = rows[i].material;
        if (!(m.flags & MaterialFlags::kPerTexture)) continue;
        m.flags &= ~(MaterialFlags::kPerTexture | MaterialFlags::kNormalMap | MaterialFlags::kMetallicRoughness);
        m.albedoTex = m.normalTex = m.metallicRoughnessTex = m.emissiveTex = kMaterialTexAbsent;
        rows[i].reserved[1] = 0;
        ++changed;
    }
    return changed;
}
}
