#pragma once
// perf-terrain-walk: which non-resident terrain views the RenderObjects.Terrain walk may leave out. The extract still
// runs for every admitted object (selection reset, bar and dust capture); only the draw visit is dropped, and only
// when it would draw and capture nothing with this frame's presentation state.
#include "dobjclass.h"

namespace mc2 {

// TreeAppearance::render outside the view does one thing: capture a falling tree's moving shadow.
constexpr bool treeRenderIsNoOp(bool inView, bool shadowMoving) { return !inView && !shadowMoving; }

// TerrainObject::renderView (trees and terrain objects) draws bars and the dust effect besides render(); an object
// that shows either is always visited. objClass is the view's object class.
constexpr bool terrainWalkSkips(int objClass, bool hasBars, bool hasDust, bool renderIsNoOp)
{
	return (objClass == TREE || objClass == TERRAINOBJECT) && !hasBars && !hasDust && renderIsNoOp;
}

} // namespace mc2
