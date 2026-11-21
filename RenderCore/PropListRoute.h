#pragma once
#include <cstdint>

// Static-prop colour island draw lists: the single source of the eight-list layout (vulkan_color_prop_island.cpp,
// color_prop_cull.comp read the same slots). Command-buffer order is opaque, core, fringe, late; each stage has a
// single-sided list followed by a double-sided one (+kDouble: cull none, back-face lighting).
namespace PropList {
constexpr uint32_t kOpaque = 0, kCore = 2, kFringe = 4, kLate = 6, kDouble = 1, kCount = 8;
constexpr uint32_t kOpaqueDoubleSided = kOpaque + kDouble;
constexpr uint32_t kNoList = ~0u;

struct CoreRoute { uint32_t list; bool alphaTestable; bool opaquePrefix; bool doubleSidedSeen; };

// Core-pass list of one packet. Routed per packet by its baked alpha-test bit (not by the type's alpha class, which
// is true for every packet of an alpha-mixed type, trunks included). A packet stays in an opaque list only while every
// earlier packet of its type was opaque, so a coplanar later packet still wins the GEQUAL tie against an earlier one.
// Single-sided packets take the back-face-culling opaque pipeline; double-sided ones (tree materials) cull nothing.
// kOpaque draws before kOpaqueDoubleSided, so a single-sided packet after a double-sided opaque one ends the prefix
// (it goes to kCore) to keep the type's draw order monotonic: kOpaque -> kOpaqueDoubleSided -> kCore.
// A packet in a core list keeps its own double-sided half (+kDouble).
constexpr CoreRoute routeCore(bool opaquePrefix, int alphaTest, bool doubleSided, bool doubleSidedSeen = false) {
    const bool testable = alphaTest != 0;
    const bool prefix = opaquePrefix && !testable && (doubleSided || !doubleSidedSeen);
    return {(prefix ? kOpaque : kCore) + (doubleSided ? kDouble : 0u), testable, prefix,
            doubleSidedSeen || (prefix && doubleSided)};
}

// Tier 0 is the full-detail mesh and keeps its soft fringe; coarser override-tree tiers take the hard cutout only.
constexpr bool tierSkipsFringe(uint32_t tier) { return tier >= 1; }

// Soft-fringe re-draw list of one packet, or kNoList. Only packets the fragment shader can alpha-test (the per-packet
// bit) get one: class 1 -> kFringe, class 2 -> kLate. An opaque packet of an alpha-ON type (a trunk) gets none; a
// coarse tier (lodSkipsLate) takes the hard cutout only.
constexpr uint32_t routeFringe(int alphaTest, bool doubleSided, bool lodSkipsLate) {
    if (alphaTest == 0 || lodSkipsLate) return kNoList;
    return (alphaTest == 2 ? kLate : kFringe) + (doubleSided ? kDouble : 0u);
}
}
