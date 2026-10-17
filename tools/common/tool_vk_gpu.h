// tool_vk_gpu -- the small offscreen-render + texture layer the viewport tools build on (asset_viewer's 3D previews).
//
// WHAT: textures (RGBA8 / BC7, sRGB or linear, optional generated mips) addressed by an opaque uint32 id (0 = none, like the GL names they
//   replace), offscreen color+depth targets that ImGui can show, host-visible vertex/index buffers, and a one-draw-per-descriptor-set pipeline
//   for "1 UBO + N sampled textures" shaders. Rendering is a one-shot command buffer per preview pass followed by a queue wait: it is a tool,
//   not the game, so there is no frame-in-flight machinery.
// ORIENTATION: targets render with a flipped (negative-height) viewport, so a GL-style projection that keeps GL's Y-up clip space produces an
//   upright image in memory: show a target with UV (0,0)-(1,1). Depth range is Vulkan's [0,1]: use tvk-style perspective (see perspectiveVk).
#pragma once
#include "tool_vk_shell.h"
#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

namespace toolvk {

enum class TexFormat { Rgba8Unorm, Rgba8Srgb, Bc7Unorm, Bc7Srgb };

struct MipLevel { size_t offset; size_t size; };

// Makes the GPU layer use `s` (call once after toolvk::init succeeded; pass nullptr before toolvk::shutdown to release everything).
bool gpuInit(Shell* s);
void gpuShutdown();
bool gpuReady();

// ---- textures ----
// `data` holds all levels back to back; `levels` names them (empty = one level at offset 0 sized w*h*4). genMips: RGBA8 only, single source
// level -> full chain by blit. Returns 0 on failure (reason on stderr).
uint32_t createTexture(int w, int h, TexFormat fmt, const void* data, const std::vector<MipLevel>& levels, bool genMips, bool repeat);
void destroyTexture(uint32_t id);
// ImGui texture id (a VkDescriptorSet) for ImGui::Image; 0 for an unknown id.
unsigned long long imguiTex(uint32_t id);
bool hasBc7();

// ---- offscreen targets ----
struct Target {
    uint32_t colorTex = 0;   // texture id (RGBA8 unorm, sampled)
    int w = 0, h = 0;
    void* impl = nullptr;
};
bool createTarget(Target& t, int w, int h);
void destroyTarget(Target& t);

// ---- buffers ----
struct Buffer {
    unsigned long long buf = 0, mem = 0;   // VkBuffer / VkDeviceMemory
    size_t size = 0;
};
bool createBuffer(Buffer& b, const void* data, size_t bytes, bool index);
void destroyBuffer(Buffer& b);

// ---- pipelines ----
struct VertexAttr { uint32_t location; uint32_t format /*VkFormat*/; uint32_t offset; };
struct PipelineDesc {
    std::string vertSpv, fragSpv;          // file names under shaderDir()
    uint32_t stride = 0;
    std::vector<VertexAttr> attrs;
    uint32_t samplers = 0;                 // combined image samplers at bindings 1..samplers; binding 0 is the UBO
    bool cullBack = false;
};
struct Pipeline { void* impl = nullptr; };
bool createPipeline(Pipeline& p, const PipelineDesc& d);
void destroyPipeline(Pipeline& p);

// Directory holding the compiled .spv files (MC2_TOOL_SHADER_DIR, else <exe dir>/shaders_vk/).
std::string shaderDir();

// ---- one offscreen pass ----
class Pass {
public:
    // Clears color + depth and begins recording. false on failure.
    bool begin(Target& t, const float clear[4]);
    void bind(Pipeline& p);
    // One draw with its own UBO contents and textures (id 0 = a 1x1 white texel). indexed: 32-bit indices.
    void draw(const void* ubo, size_t uboBytes, std::initializer_list<uint32_t> texIds, const Buffer& vb, const Buffer& ib, uint32_t indexCount);
    // Submits and waits; the target's color texture is then ready for ImGui.
    void end();
private:
    void* cb_ = nullptr;
    Pipeline* cur_ = nullptr;
    bool open_ = false;
};

// GL-convention perspective with Vulkan's [0,1] depth (column-major, like the previews' matrices).
void perspectiveVk(float fovyRad, float aspect, float zn, float zf, float m[16]);

} // namespace toolvk
