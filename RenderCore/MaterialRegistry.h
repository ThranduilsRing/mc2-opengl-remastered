// RenderCore/MaterialRegistry.h -- material identity (shared-foundations P4a).
//
// A material is KEYED by what makes it render differently: the mesh asset, the material slot of that mesh, the four texture handles (albedo, normal,
// metallic-roughness, emissive), the flag / alpha / pass class and the paint variant. Equal keys are ONE MaterialHandle (deduplication); the handle's index
// is the row of the one MaterialGpu table, whose texture fields carry MaterialTextureSemantic::DescriptorIndex (the target semantic of the unified table,
// declared here so no consumer infers it).
//
// A MaterialSet is the materials of one mesh instance, one per mesh material slot, referenced by a single base index. Two instances with the same
// materials (the same list of handles) share one set; a repainted instance gets different texture handles, so a different material per painted slot and a
// different set, while the mesh handle stays one.
//
// Pure standard C++ (doctest-driven). Nothing in the render path reads this yet: the consumers arrive with the batchers (P6a/P6b).
#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "Handle.h"
#include "MaterialGpu.h"

namespace RenderCore {

constexpr MaterialTextureSemantic kRegistryTextureSemantic = MaterialTextureSemantic::DescriptorIndex;

struct MaterialKey {
    uint64_t      meshKey = 0;        // hash of the mesh asset key (cooked asset id, LOD, variant)
    uint32_t      slot = 0;           // material slot within the mesh
    TextureHandle albedo, normal, metallicRoughness, emissive;
    uint32_t      flags = 0;          // MaterialFlags
    uint32_t      passClass = 0;      // opaque / alpha-test / blended
    uint32_t      paintVariant = 0;   // paint scheme hash (0 = unpainted)
    bool operator==(const MaterialKey& o) const {
        return meshKey == o.meshKey && slot == o.slot && albedo == o.albedo && normal == o.normal && metallicRoughness == o.metallicRoughness &&
               emissive == o.emissive && flags == o.flags && passClass == o.passClass && paintVariant == o.paintVariant;
    }
};

struct MaterialKeyHash {
    size_t operator()(const MaterialKey& k) const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
        mix(k.meshKey); mix(k.slot);
        mix(k.albedo.raw()); mix(k.normal.raw()); mix(k.metallicRoughness.raw()); mix(k.emissive.raw());
        mix(k.flags); mix(k.passClass); mix(k.paintVariant);
        return static_cast<size_t>(h);
    }
};

struct MaterialSetRef {
    uint32_t base = 0xFFFFFFFFu;   // index of the first slot in the set table
    uint32_t count = 0;
    bool valid() const { return base != 0xFFFFFFFFu; }
    bool operator==(const MaterialSetRef& o) const { return base == o.base && count == o.count; }
};

class MaterialRegistry {
public:
    // The handle of `key`: new (a row index of the MaterialGpu table) or the existing one.
    MaterialHandle acquire(const MaterialKey& key) {
        auto it = byKey_.find(key);
        if (it != byKey_.end()) return MaterialHandle::make(it->second, 1u);
        const uint32_t idx = static_cast<uint32_t>(keys_.size());
        keys_.push_back(key);
        byKey_.emplace(key, idx);
        return MaterialHandle::make(idx, 1u);
    }
    const MaterialKey* keyOf(MaterialHandle h) const { return h.isValid() && h.index() < keys_.size() ? &keys_[h.index()] : nullptr; }

    // The set of these materials (one per mesh material slot, in slot order): an existing set with the same handles, or a new contiguous range.
    MaterialSetRef acquireSet(const std::vector<MaterialHandle>& slots) {
        auto it = bySet_.find(slots);
        if (it != bySet_.end()) return it->second;
        MaterialSetRef r;
        r.base = static_cast<uint32_t>(setSlots_.size());
        r.count = static_cast<uint32_t>(slots.size());
        setSlots_.insert(setSlots_.end(), slots.begin(), slots.end());
        bySet_.emplace(slots, r);
        return r;
    }
    MaterialHandle materialOfSetSlot(MaterialSetRef set, uint32_t slot) const {
        if (!set.valid() || slot >= set.count) return MaterialHandle::invalid();
        return setSlots_[set.base + slot];
    }

    uint32_t materialCount() const { return static_cast<uint32_t>(keys_.size()); }
    uint32_t setCount() const { return static_cast<uint32_t>(bySet_.size()); }

private:
    struct SlotsLess {
        bool operator()(const std::vector<MaterialHandle>& a, const std::vector<MaterialHandle>& b) const {
            if (a.size() != b.size()) return a.size() < b.size();
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].raw() != b[i].raw()) return a[i].raw() < b[i].raw();
            return false;
        }
    };
    std::vector<MaterialKey> keys_;
    std::unordered_map<MaterialKey, uint32_t, MaterialKeyHash> byKey_;
    std::vector<MaterialHandle> setSlots_;
    std::map<std::vector<MaterialHandle>, MaterialSetRef, SlotsLess> bySet_;
};

}  // namespace RenderCore
