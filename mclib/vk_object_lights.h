// CPU object-light rows consumed by the native Vulkan shared light feed.
// Static rows survive frame resets; dynamic rows and dedup caches reset together.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
struct TG_HWLightsData;
namespace mc2::vkLights {
struct Packet;
// Retains the exact CPU version used by a native draw, including its row stamps.
struct Feed {
    std::shared_ptr<const Packet> owner;
    const void* data = nullptr;
    uint64_t bytes = 0;
    std::span<const uint64_t> generations;
    uint64_t revision = 0;
    uint64_t packetKey = 0;
};
Feed captureFeed();
void start();
void shutdown();
bool active();
        void resetLightData();

        // [LIGHTBAKE v2] persistent static-light table: write the baked
        // constant into the permanent CPU slot lightData_[recipeIndex]
        // (grow-preserving) + advance the static high-water S. Called
        // once per recipe at bake / invalidate re-bake, NOT per frame.
        void bakeStaticLightSlot(int32_t recipeIndex, const struct TG_HWLightsData& baked);

        // Diagnostic accessors for the static-prop registry's flush trace:
        // read-only views of the per-frame light dedup table.
        // LightSlotPeek includes the first light's type (lightDir[0].w) and
        // color (lightColor[0].rgb) so registry::flush() can identify the
        // H2 hypothesis: slot has numLights=1 but lightColor=(0,0,0) → black.
        struct LightSlotPeek {
            int   numLights;    // -1 if out of range
            int   firstType;    // -1 if numLights==0; else int(lightDir[0].w)
            float firstColorR;
            float firstColorG;
            float firstColorB;
        };
        uint32_t getLightStructCount();
        LightSlotPeek peekLightSlot(uint32_t idx);
        // [LIGHTBAKE-PROOF v1] copy a light slot's full struct (parity trace).
        // Returns false if idx is beyond the allocated count.
        bool copyLightSlot(uint32_t idx, TG_HWLightsData& out);
        // VK-MECH-FEED-NATIVE-1: read-only view of the CPU LightsData array
        // (the byte source of the GL SSBO upload). Defined in vk_object_lights.cpp
        // (needs complete TG_HWLightsData for sizeof).
uint32_t addLightDataStructure(TG_HWLightsData* lightData);
uint32_t addLightDataStructureWithPerActorColor(TG_HWLightsData* lightData);
}
