#pragma once

#include "dobjclass.h"
#include <span>

namespace mc2 {
// Mission movers exist before terrain objects. Resolve initial tree contacts
// after terrain priming, including overlaps across an object-block boundary.
// Contact is the stock tree collision handler, not a second destruction path.
template<class Mover, class Tree, size_t Extent, class Contact>
unsigned clearSpawnTrees(Mover& mover, std::span<Tree*, Extent> objects, Contact contact)
{
    if (mover.getObjectClass() != BATTLEMECH || !mover.getExists() ||
        !mover.getTangible() || mover.getMoveLevel() != 0 || !mover.pathLocks)
        return 0;
    unsigned cleared = 0;
    const auto position = mover.getPosition();
    for (auto* tree : objects) {
        if (!tree || tree->getObjectClass() != TREE || !tree->getExists() || !tree->getTangible())
            continue;
        const auto p = tree->getPosition();
        const float dx = p.x - position.x, dy = p.y - position.y;
        const float radius = mover.getExtentRadius() + tree->getExtentRadius();
        if (radius > 0.0f && dx * dx + dy * dy < radius * radius) {
            contact(tree, &mover);
            ++cleared;
        }
    }
    return cleared;
}
}
