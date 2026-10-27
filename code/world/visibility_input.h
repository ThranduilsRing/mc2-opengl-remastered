#pragma once
// S4 VisibilityInput: the one way sim code reads "did the camera see this object this turn".
// The values are produced by the presentation stage (GameObjectManager::updateAppearancesOnly stamps windowsVisible from
// recalcBounds, once per frame); sim consumers read them only through here, never by calling recalcBounds themselves.
#include "gameobj.h"

extern long turn;

namespace mc2::world {
inline bool visibleThisTurn(GameObject* obj) { return obj->getWindowsVisible() == turn; }
}
