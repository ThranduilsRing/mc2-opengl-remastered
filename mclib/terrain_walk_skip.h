#pragma once
// perf-terrain-walk: which non-resident terrain views the RenderObjects.Terrain walk may leave out. The extract still
// runs for every admitted object (selection reset, bar and dust capture); only the draw visit is dropped, and only
// when it would draw and capture nothing with this frame's presentation state.
#include "dobjclass.h"

namespace mc2 {

// Resident draws skip extractRenderView, but picking and transient GUI flags still run every frame.
template <class Object>
void refreshResidentInteraction(Object& obj, long turn)
{
	if (obj.getAppearance()->canBeSeen()) obj.windowsVisible = turn;
	obj.setSelected(false);
	obj.setTargeted(false);
}

// TreeAppearance::render outside the view does one thing: capture a falling tree's moving shadow.
constexpr bool treeRenderIsNoOp(bool inView, bool shadowMoving) { return !inView && !shadowMoving; }

// TerrainObject::renderView (trees and terrain objects) draws bars and the dust effect besides render(); an object
// that shows either is always visited. objClass is the view's object class.
constexpr bool terrainWalkSkips(int objClass, bool hasBars, bool hasDust, bool renderIsNoOp)
{
	return (objClass == TREE || objClass == TERRAINOBJECT) && !hasBars && !hasDust && renderIsNoOp;
}

// perf-building-resident: BldgAppearance::render outside the view draws nothing; it still marks a world marker's nodes
// NoShadow and captures a moving (mover-owned or not static-eligible) shadow, so either keeps the visit. The terrain
// object's renderView also calls setVisibility(true, true), so the visit is a no-op only once both are already set
// (the FOW flags night lights read in BldgAppearance::update).
constexpr bool bldgRenderIsNoOp(bool inView, bool marker, bool capturesMoverShadow, bool visibleAndSeen)
{
	return !inView && !marker && !capturesMoverShadow && visibleAndSeen;
}

// perf-building-resident: Building, Turret and Gate::renderView return at once when their extract left the Visible
// flag clear (appearance not inView); bars and the name are only captured under that flag, so the visit draws nothing.
constexpr bool visibleFlagWalkSkips(int objClass, bool visibleFlag)
{
	return (objClass == BUILDING || objClass == TURRET || objClass == GATE) && !visibleFlag;
}

} // namespace mc2
