#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include "Stuff/Stuff.hpp"
#include "tgl.h"
#include "msl.h"

// Per-instance shader-visible struct.
// Layout mirror of the GLSL std430 struct in shaders/static_prop.vert.
// CHANGING THIS STRUCT REQUIRES CHANGING THE SHADER IN LOCKSTEP.
struct alignas(16) GpuStaticPropInstance {
    float    modelMatrix[16];   // shape-to-world, row-major (uploaded GL_FALSE)
    uint32_t typeID;
    uint32_t firstColorOffset;  // into per-frame color SSBO
    uint32_t flags;             // bit 0: lightsOut, bit 1: isWindow, bit 2: isSpotlight
    uint32_t _pad0;
    float    aRGBHighlight[4];
    float    fogRGB[4];
};

// Layout: 64 (mat4) + 16 (4 x uint32) + 16 (vec4) + 16 (vec4) = 112 bytes.
static_assert(sizeof(GpuStaticPropInstance) == 112,
              "GpuStaticPropInstance size must match std430 GLSL struct");
static_assert(offsetof(GpuStaticPropInstance, modelMatrix)      ==  0, "modelMatrix offset");
static_assert(offsetof(GpuStaticPropInstance, typeID)           == 64, "typeID offset");
static_assert(offsetof(GpuStaticPropInstance, firstColorOffset) == 68, "firstColorOffset offset");
static_assert(offsetof(GpuStaticPropInstance, flags)            == 72, "flags offset");
static_assert(offsetof(GpuStaticPropInstance, _pad0)            == 76, "_pad0 offset");
static_assert(offsetof(GpuStaticPropInstance, aRGBHighlight)    == 80, "aRGBHighlight offset");
static_assert(offsetof(GpuStaticPropInstance, fogRGB)           == 96, "fogRGB offset");

// Packet descriptor (CPU-side only -- not uploaded as an SSBO).
struct GpuStaticPropPacket {
    uint32_t firstIndex;     // into shared IBO
    uint32_t indexCount;
    int32_t  baseVertex;     // into shared VBO
    uint32_t textureSlot;    // index into owning TG_TypeShape::listOfTextures.
                             // Resolved at draw time because MC2 mutates the
                             // handle each frame via SetTextureHandle (see
                             // msl.cpp:1321 TransformMultiShape).
    uint32_t materialFlags;  // bit 0: ALPHA_TEST_BIT
    uint32_t owningTypeID;
};

constexpr uint32_t STATIC_PROP_FLAG_ALPHA_TEST = 1u << 0;

// Per-type descriptor: range of packets + vertex count (for color block sizing).
struct GpuStaticPropType {
    uint32_t firstPacket;
    uint32_t packetCount;
    uint32_t vertexCount;    // number of vertices in the owning TG_TypeShape
    const TG_TypeShape* source;
};

class GpuStaticPropBatcher {
public:
    static GpuStaticPropBatcher& instance();

    // Called from gameosmain at map load / unload.
    void onMapLoad();
    void onMapUnload();

    // Register one TG_TypeShape (idempotent). Builds packet table entries
    // and appends geometry to the in-progress VBO/IBO staging.
    // Called during onMapLoad for every static-prop type + its damage variants.
    void registerType(TG_TypeShape* typeShape);

    // Convenience wrapper: iterate a multishape's listOfTypeShapes and call
    // registerType on each SHAPE_NODE leaf. Safe to call with NULL (no-op).
    void registerMultiShape(TG_TypeMultiShape* multiShape);

    // Called at end of registration to upload the immutable VBO/IBO.
    void finalizeGeometry();

    // Per-frame submission. shape->listOfColors must be fresh (set by
    // TransformMultiShape earlier in the frame).
    // Returns true if the instance was accepted. Returns false (Layer B
    // safety net) when the type was never registered; in that case the
    // caller MUST render the shape via the old CPU path this frame.
    [[nodiscard]] bool submit(TG_Shape* shape,
                              const Stuff::Matrix4D& shapeToWorld,
                              uint32_t highlightARGB,
                              uint32_t fogARGB,
                              uint32_t flags);

    // Iterate a multishape's children and submit each SHAPE_NODE leaf using
    // the child's own listOfShapes[i].shapeToWorld. Per-child highlight/fog/
    // flags are pulled from the TG_Shape node itself. Returns false if ANY
    // child fails registration — caller MUST CPU-fallback the whole
    // multishape for this frame to keep the visual self-consistent.
    [[nodiscard]] bool submitMultiShape(TG_MultiShape* multi);

    // Per-frame dispatch.
    void flush();         // main color pass
    void flushShadow();   // depth-only into dynamic shadow FBO

    // Debug: color-address validation mode. 0=off, 1=gradient, 2=hash.
    void setDebugAddrMode(int mode);
    int  getDebugAddrMode() const { return debugAddrMode_; }

private:
    GpuStaticPropBatcher() = default;

    // Declared here so the whole batcher state is visible for review;
    // implementation details live in .cpp.
    struct Impl;
    // State is file-static in .cpp to keep this header light; singleton
    // method bodies there forward to those statics.
    int debugAddrMode_ = 0;
};
