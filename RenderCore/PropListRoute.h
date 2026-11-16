#pragma once
#include <cstdint>

// Static-prop colour island draw lists. Command-buffer order is opaque, opaque double-sided, core, fringe, late;
// the indices are the on-GPU list slots (color_prop_cull.comp reads the same order).
namespace PropList {
constexpr uint32_t kOpaque = 0, kCore = 1, kFringe = 2, kLate = 3, kOpaqueDoubleSided = 4, kCount = 5;

struct CoreRoute { uint32_t list; bool alphaTestable; bool opaquePrefix; bool doubleSidedSeen; };

// Core-pass list of one packet. Routed per packet by its baked alpha-test bit (not by the type's alpha class, which
// is true for every packet of an alpha-mixed type, trunks included). A packet stays in an opaque list only while every
// earlier packet of its type was opaque, so a coplanar later packet still wins the GEQUAL tie against an earlier one.
// Single-sided packets take the back-face-culling opaque pipeline; double-sided ones (tree materials) cull nothing.
// kOpaque draws before kOpaqueDoubleSided, so a single-sided packet after a double-sided opaque one ends the prefix
// (it goes to kCore) to keep the type's draw order monotonic: kOpaque -> kOpaqueDoubleSided -> kCore.
constexpr CoreRoute routeCore(bool opaquePrefix, int alphaTest, bool doubleSided, bool doubleSidedSeen = false) {
    const bool testable = alphaTest != 0;
    const bool prefix = opaquePrefix && !testable && (doubleSided || !doubleSidedSeen);
    return {prefix ? (doubleSided ? kOpaqueDoubleSided : kOpaque) : kCore, testable, prefix,
            doubleSidedSeen || (prefix && doubleSided)};
}

// Tier 0 is the full-detail mesh and keeps its soft fringe; coarser override-tree tiers take the hard cutout only.
constexpr bool tierSkipsFringe(uint32_t tier) { return tier >= 1; }
}
