// code/screen_layer_order.h -- which logistics screens may draw behind the current one.
// The front end composites in two layers: legacy GameOS HUD draws first, ImGui (defs pages) after.
// A predecessor in the ImGui layer therefore always lands ABOVE a legacy current screen, whatever the
// submit order: the pilot select page covered the (legacy) loading doors. Standard library only.
#pragma once

namespace mc2::ui {

// True when drawing `other` beneath `current` cannot end up on top of it.
constexpr bool predecessorMayDrawBehind(bool currentInImGuiLayer, bool otherInImGuiLayer) {
    return currentInImGuiLayer || !otherInImGuiLayer;
}

}  // namespace mc2::ui
