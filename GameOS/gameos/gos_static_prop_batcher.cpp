#include "gos_static_prop_batcher.h"
#include "gos_profiler.h"
#include "gameos.hpp"
#include <GL/glew.h>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t RING_FRAMES = 3;
constexpr size_t   INITIAL_INSTANCES_PER_FRAME = 4096;
constexpr size_t   INITIAL_COLORS_PER_FRAME    = 1'000'000;  // uint32 ARGB entries

// Immutable per-map geometry.
GLuint s_sharedVbo = 0;
GLuint s_sharedIbo = 0;
GLuint s_sharedVao = 0;

// Per-frame persistent-mapped rings.
GLuint   s_instanceSsbo = 0;
GLuint   s_colorSsbo    = 0;
void*    s_instanceMap  = nullptr;
void*    s_colorMap     = nullptr;
GLsync   s_fence[RING_FRAMES] = {0};
uint32_t s_frameSlot = 0;

// CPU staging for the current frame.
// Instances are staged in per-type buckets (not a flat list) so that
// flush() can write each type's instances into a contiguous SSBO region.
// Binding that region via glBindBufferRange means gl_InstanceID in the
// shader is 0..N-1 within the bucket -- NOT dependent on gl_BaseInstance
// and NOT requiring any extension.
struct PerTypeBucket {
    std::vector<GpuStaticPropInstance> instances;
    std::vector<uint32_t>              colors;  // concatenated per-instance color blocks
};
std::unordered_map<uint32_t, PerTypeBucket> s_bucketsByType;

// Populated at flush time: per-type contiguous byte offset into the
// ring-slot SSBO (instance + color), used to bind exactly that range.
struct TypeRangeSsbo {
    size_t instanceByteOffset;
    size_t instanceByteSize;
    size_t colorByteOffset;
    size_t colorByteSize;
    uint32_t instanceCount;
};

// Geometry table (immutable after finalizeGeometry).
std::vector<GpuStaticPropPacket>                   s_packets;
std::vector<GpuStaticPropType>                     s_types;
std::unordered_map<const TG_TypeShape*, uint32_t>  s_typeIndex;

// CPU-side staging during registration (cleared after finalizeGeometry).
std::vector<uint8_t>  s_stagingVbo;
std::vector<uint32_t> s_stagingIbo;

bool s_geometryFinalized = false;
bool s_fatalRegistrationFailure = false;

// Layer B fallback: types we failed to register (logged once, fall back to CPU path).
std::unordered_map<const TG_TypeShape*, bool> s_failedTypes;

} // namespace

GpuStaticPropBatcher& GpuStaticPropBatcher::instance() {
    static GpuStaticPropBatcher s;
    return s;
}

void GpuStaticPropBatcher::onMapLoad() {
    // Reset everything; called at every map boundary.
    s_packets.clear();
    s_types.clear();
    s_typeIndex.clear();
    s_stagingVbo.clear();
    s_stagingIbo.clear();
    s_failedTypes.clear();
    s_geometryFinalized = false;
    s_fatalRegistrationFailure = false;
}

void GpuStaticPropBatcher::onMapUnload() {
    if (s_sharedVbo) { glDeleteBuffers(1, &s_sharedVbo); s_sharedVbo = 0; }
    if (s_sharedIbo) { glDeleteBuffers(1, &s_sharedIbo); s_sharedIbo = 0; }
    if (s_sharedVao) { glDeleteVertexArrays(1, &s_sharedVao); s_sharedVao = 0; }
    // Ring buffers are kept across maps (sized to map's worst case -- grow on demand).
}

void GpuStaticPropBatcher::registerType(TG_TypeShape* /*typeShape*/) {
    // Filled in Task 6.
}

void GpuStaticPropBatcher::finalizeGeometry() {
    // Filled in Task 7.
    s_geometryFinalized = true;
}

bool GpuStaticPropBatcher::submit(TG_Shape* /*shape*/,
                                  const Stuff::Matrix4D& /*shapeToWorld*/,
                                  uint32_t /*highlightARGB*/,
                                  uint32_t /*fogARGB*/,
                                  uint32_t /*flags*/) {
    // Filled in Task 8.
    return false;
}

void GpuStaticPropBatcher::flush() {
    // Filled in Task 10.
    s_bucketsByType.clear();
}

void GpuStaticPropBatcher::flushShadow() {
    // Filled in Task 13.
}

void GpuStaticPropBatcher::setDebugAddrMode(int mode) { debugAddrMode_ = mode; }
