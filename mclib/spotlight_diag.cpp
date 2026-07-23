// T1.16 — (E)-owned spotlight slot tagging registry implementation.
//
// See spotlight_diag.h. File-scope unordered_map<long, uint8_t> mapping
// slotId -> SourceClass. Cheap O(1) lookups in Camera::updateLights; only
// queried when MC2_SPOT_DIAG is set (callers check is_enabled() first to
// avoid the lookup entirely in the default-off case).

#include "spotlight_diag.h"

#include <cstdlib>
#include <unordered_map>

namespace mc2_spotlight_diag {

namespace {

std::unordered_map<long, uint8_t>& slot_map()
{
    static std::unordered_map<long, uint8_t> s_map;
    return s_map;
}

bool s_enabled_cached = (std::getenv("MC2_SPOT_DIAG") != nullptr);

} // namespace

void tag_slot(long slot, SourceClass src)
{
    slot_map()[slot] = static_cast<uint8_t>(src);
}

void untag_slot(long slot)
{
    slot_map().erase(slot);
}

bool is_e_slot(long slot, SourceClass* outSrc)
{
    auto& m = slot_map();
    auto it = m.find(slot);
    if (it == m.end()) return false;
    if (outSrc) *outSrc = static_cast<SourceClass>(it->second);
    return true;
}

void reset()
{
    slot_map().clear();
}

bool is_enabled()
{
    return s_enabled_cached;
}

// MOVER-LIGHT-HALOGEN-1: see header. Parsed once; malformed hex -> default.
unsigned long mover_light_argb()
{
    static const unsigned long s_argb = []() -> unsigned long {
        unsigned long rgb = 0xFFEED1ul;   // warm halogen default
        const char* v = std::getenv("MC2_LIGHT_POOL_MOVER_COLOR");
        if (v && v[0]) {
            char* end = nullptr;
            unsigned long parsed = std::strtoul(v, &end, 16);
            if (end && end != v && *end == '\0' && parsed <= 0xFFFFFFul)
                rgb = parsed;
        }
        return 0xFF000000ul | rgb;
    }();
    return s_argb;
}

float mover_light_fwd_offset()
{
    static const float s_fwd = []() -> float {
        float f = 24.0f;   // MOVER-LIGHT-FORWARD-1 default
        const char* v = std::getenv("MC2_LIGHT_POOL_MOVER_FWD");
        if (v && v[0]) f = (float)std::atof(v);
        if (f < 0.0f) f = 0.0f;
        if (f > 120.0f) f = 120.0f;
        return f;
    }();
    return s_fwd;
}

} // namespace mc2_spotlight_diag
