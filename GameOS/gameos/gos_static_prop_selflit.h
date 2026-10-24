// Which static-prop types are self-lit cards (drawn double-sided). Header-only, so it is unit-tested.
#pragma once
#include <cstddef>
#include <cstdint>

namespace mc2_prop {

// A stock self-lit card (the power generator's spinning Coil) tags every vertex 0xffffffff
// ("other light, add as authored"). An imported override (GLB/FBX, or the load placeholder) also
// defaults to 0xffffffff when it has no authored colours, so imports are never cards.
template <class Vertex>
inline bool isSelfLitTwoSidedCard(bool importedMesh, const Vertex* verts, std::size_t count)
{
    if (importedMesh || !verts || count == 0)
        return false;
    for (std::size_t v = 0; v < count; ++v)
        if (verts[v].aRGBLight != 0xffffffffu)
            return false;
    return true;
}

} // namespace mc2_prop
