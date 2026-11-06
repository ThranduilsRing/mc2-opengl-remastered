#pragma once
#include <cstdint>
#include <cstddef>

namespace gpu_cull {

// std430 layout. 16-byte aligned for SSBO array stride safety.
// Total size: 64 bytes.
struct alignas(16) GpuActorRecord {
    float       worldCenter[3];      // offset 0   (12 B) — raw MC2 world coords (x=east, y=north, z=elev)
    float       boundingRadius;      // offset 12  (4 B)
    float       worldAabbMin[3];     // offset 16  (12 B)
    uint32_t    category;            // offset 28  (4 B)
    float       worldAabbMax[3];     // offset 32  (12 B)
    uint32_t    flags;               // offset 44  (4 B)
    uint32_t    actorId;             // offset 48  (4 B)
    uint32_t    prevVisibilityBit;   // offset 52  (4 B)
    uint32_t    consumerFlags;       // offset 56  (4 B)
    uint32_t    blockIdx;           // offset 60  (4 B) — actor's terrain block index for C1-RB rollup
};

static_assert(sizeof(GpuActorRecord) == 64,
              "GpuActorRecord size must match std430 GLSL struct (64 B).");
static_assert(offsetof(GpuActorRecord, worldCenter)        ==  0, "worldCenter offset");
static_assert(offsetof(GpuActorRecord, boundingRadius)     == 12, "boundingRadius offset");
static_assert(offsetof(GpuActorRecord, worldAabbMin)       == 16, "worldAabbMin offset");
static_assert(offsetof(GpuActorRecord, category)           == 28, "category offset");
static_assert(offsetof(GpuActorRecord, worldAabbMax)       == 32, "worldAabbMax offset");
static_assert(offsetof(GpuActorRecord, flags)              == 44, "flags offset");
static_assert(offsetof(GpuActorRecord, actorId)            == 48, "actorId offset");
static_assert(offsetof(GpuActorRecord, prevVisibilityBit)  == 52, "prevVisibilityBit offset");
static_assert(offsetof(GpuActorRecord, consumerFlags)      == 56, "consumerFlags offset");
static_assert(offsetof(GpuActorRecord, blockIdx)           == 60, "blockIdx offset");

// Header at SSBO offset 0 (one per ring slot).
struct alignas(16) GpuActorRecordHeader {
    uint32_t recordCount;            // 0  — set by CPU at flushUpload()
    uint32_t recordCapacity;         // 4  — mission-load constant
    uint32_t visibleCount;           // 8  — written by compute (C1+); ignored in C0
    uint32_t _pad0;                  // 12
};
static_assert(sizeof(GpuActorRecordHeader) == 16, "Header must be 16 B std430");

enum GpuActorCategory : uint32_t {
    Cat_Other      = 0u,
    Cat_Mech       = 1u,
    Cat_GroundVeh  = 2u,
    Cat_Gate       = 3u,
    Cat_Turret     = 4u,
    Cat_StaticProp = 5u,
    CategoryMask   = 0xFu,
};

enum GpuActorFlags : uint32_t {
    Flag_None          = 0u,
    Flag_AlwaysVisible = 1u << 0,
    Flag_HasShadow     = 1u << 1,
    Flag_NeverShadow   = 1u << 2,
};

enum class GpuConsumerFlags : uint32_t {
    Consumer_None             = 0u,
    Consumer_AIGate           = 1u << 0,
    Consumer_WeaponSpawnNode  = 1u << 1,
    Consumer_LifecycleGate    = 1u << 2,
    Consumer_RenderGate       = 1u << 3,
};
using enum GpuConsumerFlags;

// Selftest: call once at startup. Returns number of failures (0 = pass).
int gpu_cull_record_selftest();

} // namespace gpu_cull
