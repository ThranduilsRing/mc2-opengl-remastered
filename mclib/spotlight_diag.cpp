// T1.16 — (E)-owned spotlight slot tagging registry implementation.
//
// Camera identity scopes slot numbers, including the shipped mover classification.

#include "spotlight_diag.h"

#include <cstdlib>
#include <unordered_map>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"

namespace mc2_spotlight_diag {

namespace {

struct SlotTag { uint8_t src; const void* owner; };

using CameraTags = std::unordered_map<const void*, std::unordered_map<long, SlotTag>>;
CameraTags* live_map = nullptr;
CameraTags& slot_map()
{
    struct Registry : CameraTags {
        Registry() { live_map = this; }
        ~Registry() { live_map = nullptr; }
    };
    static Registry s_map;
    return s_map;
}

bool s_enabled_cached = false;
uint64_t s_registration_revision = 0;

} // namespace

void tag_slot(const void* camera, long slot, SourceClass src, const void* owner)
{
    ++s_registration_revision;
    SlotTag t;
    t.src = static_cast<uint8_t>(src);
    t.owner = owner;
    slot_map()[camera][slot] = t;
}

const void* slot_owner(const void* camera, long slot)
{
    const auto cam = slot_map().find(camera);
    if (cam == slot_map().end()) return nullptr;
    const auto& [identity, m] = *cam;
    auto it = m.find(slot);
    if (it == m.end()) return nullptr;
    const auto& [index, tag] = *it;
    return tag.owner;
}

void untag_slot(const void* camera, long slot)
{
    ++s_registration_revision;
    auto& cameras = slot_map();
    const auto cam = cameras.find(camera);
    if (cam == cameras.end()) return;
    auto& [identity, tags] = *cam;
    tags.erase(slot);
    if (tags.empty()) cameras.erase(cam);
}

std::optional<SourceClass> is_e_slot(const void* camera, long slot)
{
    const auto cam = slot_map().find(camera);
    if (cam == slot_map().end()) return std::nullopt;
    const auto& [identity, m] = *cam;
    auto it = m.find(slot);
    if (it == m.end()) return std::nullopt;
    const auto& [index, tag] = *it;
    return static_cast<SourceClass>(tag.src);
}

void reset()
{
    ++s_registration_revision;
    if (live_map) live_map->clear();
}

void reset(const void* camera)
{
    ++s_registration_revision;
    // missionCamera can outlive the lazily constructed registry at process exit.
    if (camera && live_map) live_map->erase(camera);
}

uint64_t registration_revision()
{
    return s_registration_revision;
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
        const std::string colorStr = mc2::gate::str<"MC2_LIGHT_POOL_MOVER_COLOR">();
        const char* v = colorStr.c_str();
        if (v[0]) {
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
    static const float s_fwd = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_FWD">();
    return s_fwd;
}

} // namespace mc2_spotlight_diag
