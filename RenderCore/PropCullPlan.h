#pragma once

// Static-prop compaction decision (vulkan_color_prop_island.cpp). With HZB off every instance survives, so the compaction dispatch is an
// identity copy: the draw can read the instance slot straight from gl_InstanceIndex and the dispatch and both barriers go away. The indirect
// commands are cached per parity slot and an HZB dispatch overwrites their instanceCounts in place, so a slot that last ran a dispatch must
// re-upload the original commands before its next direct frame.
namespace PropList {
struct CullDecision { bool dispatch; bool reupload; bool slotDirty; };

// hzbOn: this view's HZB visibility is in use. slotDirty: the slot's command buffer was last written by a dispatch. Returns the new slotDirty.
constexpr CullDecision decideCull(bool hzbOn, bool slotDirty) {
    if (hzbOn) return {true, false, true};
    return {false, slotDirty, false};
}
}
