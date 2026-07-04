// =============================================================================
// VK-sampled == GL-sampled parity SPIKE  (VK-INTEROP-BRIDGE-1)
//
//   STOP-gate / oracle for the GL->VK SOURCE-TEXTURE seam: VK regions will
//   sample GL source textures (colormap / material / props) via a VK-owned
//   copy-once (staging buffer -> vkCmdCopyBufferToImage -> sampled VK image +
//   VkSampler). Before wiring that into the engine we must PROVE that VK
//   sampling of an uploaded texture matches GL sampling of the SAME source
//   bytes, across every format axis MC2 uses.
//
// For each case we upload IDENTICAL source bytes to BOTH:
//   * a GL texture, sampled by a fullscreen-quad GL draw -> glReadPixels.
//   * a VK sampled image (staging -> copy -> sampled by a VK draw) -> readback.
// then ASSERT VK-sampled == GL-sampled within tolerance.
//
// NOTE: This is NOT a zero-copy interop test — the other spikes prove shared
// images/semaphores. This proves the SAMPLING math (channel order, sRGB decode,
// BC7 decode, array-layer indexing, bilinear taps) agrees between the two APIs
// when each uploads the same bytes independently. That is exactly the contract
// the copy-once source-texture path relies on.
//
// COVERAGE MATRIX (all must pass):
//   2D_RGBA8            colormap/cement/overlay
//   2D_RGBA8_BGRA      upload BGRA-in-memory (GL format=GL_BGRA); R/B NOT flipped
//   2D_ARRAY_RGBA8     >=2 layers; correct layer at float layer index (props)
//   BC7_UNORM          known BC7 block; blocks copied verbatim, decoded equal
//   BC7_SRGB           sRGB->linear on sample matches GL (gamma trap)
//   LINEAR_FILTER      bilinear tap at non-integer UV within tolerance
// NEGATIVE CONTROL (env MC2_SAMPLETEX_NEGCTL=rbswap): VK uploads R/B-swapped
//   bytes so the BGRA case MUST mismatch -> proves the harness detects it.
//
// Exit 0 == every enabled case PASS + zero VK validation errors == GATE GREEN.
// Non-zero == which case/axis failed is printed == GATE RED (scopes the wiring).
// =============================================================================

#include <windows.h>

#include <GL/glew.h>
#include <GL/wglew.h>

#include <SDL.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <cmath>
#include <vector>
#include <string>

// Embedded SPIR-V (glslc -mfmt=c at build time; see CMakeLists.txt).
static const uint32_t sampletex_vert_spv[] =
    #include "sampletex.vert.spv.inc"
;
static const uint32_t sampletex_frag_spv[] =
    #include "sampletex.frag.spv.inc"
;
static const uint32_t sampletex_array_frag_spv[] =
    #include "sampletex_array.frag.spv.inc"
;

#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM        0x8E8C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM  0x8E8D
#endif

// ---- logging + verdict tracking --------------------------------------------
static int  g_valErrors = 0;
static bool g_glDebugSawError = false;
#define LOG(...)  do { std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while(0)

static VKAPI_ATTR VkBool32 VKAPI_CALL vkDbg(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* d, void*)
{
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG("[VK-VALIDATION %s] %s",
            (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" : "WARN",
            d->pMessage ? d->pMessage : "(null)");
        if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) g_valErrors++;
    }
    return VK_FALSE;
}
static void APIENTRY glDbg(GLenum, GLenum type, GLuint, GLenum sev,
                           GLsizei, const GLchar* msg, const void*) {
    if (type == GL_DEBUG_TYPE_ERROR || sev == GL_DEBUG_SEVERITY_HIGH) {
        LOG("[GL-DEBUG ERROR] %s", msg ? msg : "(null)");
        g_glDebugSawError = true;
    }
}
static bool glCheck(const char* where) {
    GLenum e; bool bad = false;
    while ((e = glGetError()) != GL_NO_ERROR) { LOG("[GL-ERROR 0x%04X] at %s", e, where); bad = true; }
    return !bad;
}

// -----------------------------------------------------------------------------
struct App {
    SDL_Window*   win = nullptr;
    SDL_GLContext glctx = nullptr;
    VkInstance       inst = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice         dev  = VK_NULL_HANDLE;
    uint32_t         qfam = 0;
    VkQueue          queue = VK_NULL_HANDLE;
    VkCommandPool    cmdPool = VK_NULL_HANDLE;
    PFN_vkCreateDebugUtilsMessengerEXT  pCreateDbg = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT pDestroyDbg = nullptr;
};

static bool findMemType(App& a, uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t* out) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(a.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { *out = i; return true; }
    return false;
}

// ============================ init GL / VK ===================================
static bool initGL(App& a) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { LOG("SDL_Init: %s", SDL_GetError()); return false; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    a.win = SDL_CreateWindow("vk_gl_sampletex_spike", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!a.win) { LOG("SDL_CreateWindow: %s", SDL_GetError()); return false; }
    a.glctx = SDL_GL_CreateContext(a.win);
    if (!a.glctx) { LOG("SDL_GL_CreateContext (need GL4.5 core): %s", SDL_GetError()); return false; }
    glewExperimental = GL_TRUE;
    GLenum ge = glewInit();
    if (ge != GLEW_OK) { LOG("glewInit: %s", (const char*)glewGetErrorString(ge)); return false; }
    glGetError();
    LOG("[GL] vendor=%s",   (const char*)glGetString(GL_VENDOR));
    LOG("[GL] renderer=%s", (const char*)glGetString(GL_RENDERER));
    LOG("[GL] version=%s",  (const char*)glGetString(GL_VERSION));
    if (GLEW_KHR_debug || GLEW_ARB_debug_output) {
        glEnable(GL_DEBUG_OUTPUT); glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDbg, nullptr);
    }
    if (!GLEW_ARB_texture_compression_bptc && !GLEW_EXT_texture_compression_bptc)
        LOG("[GL] WARNING: BPTC/BC7 extension not advertised (may still be core in 4.5)");
    return glCheck("initGL");
}

static bool initVk(App& a) {
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool haveValidation = false;
    for (auto& l : layers) if (!std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) haveValidation = true;
    LOG("[VK] VK_LAYER_KHRONOS_validation available = %d", haveValidation ? 1 : 0);

    std::vector<const char*> instExts = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    std::vector<const char*> instLayers;
    if (haveValidation) instLayers.push_back("VK_LAYER_KHRONOS_validation");

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "vk_gl_sampletex_spike";
    ai.apiVersion = VK_API_VERSION_1_2;

    VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dci.pfnUserCallback = vkDbg;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = &dci;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = (uint32_t)instExts.size();
    ici.ppEnabledExtensionNames = instExts.data();
    ici.enabledLayerCount = (uint32_t)instLayers.size();
    ici.ppEnabledLayerNames = instLayers.data();
    if (vkCreateInstance(&ici, nullptr, &a.inst) != VK_SUCCESS) { LOG("vkCreateInstance"); return false; }

    a.pCreateDbg  = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(a.inst, "vkCreateDebugUtilsMessengerEXT");
    a.pDestroyDbg = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(a.inst, "vkDestroyDebugUtilsMessengerEXT");
    if (a.pCreateDbg) a.pCreateDbg(a.inst, &dci, nullptr, &a.messenger);

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(a.inst, &pdCount, nullptr);
    if (pdCount == 0) { LOG("no Vulkan physical devices"); return false; }
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(a.inst, &pdCount, pds.data());
    a.phys = pds[0];
    for (auto pd : pds) {
        VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(pd, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) a.phys = pd;
    }
    { VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(a.phys, &p);
      LOG("[VK] using device: %s", p.deviceName); }

    // Require textureCompressionBC for BC7.
    VkPhysicalDeviceFeatures feats{}; vkGetPhysicalDeviceFeatures(a.phys, &feats);
    LOG("[VK] textureCompressionBC = %d", feats.textureCompressionBC ? 1 : 0);

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(a.phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(a.phys, &qfCount, qfs.data());
    bool found = false;
    for (uint32_t i = 0; i < qfCount; ++i)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { a.qfam = i; found = true; break; }
    if (!found) { LOG("no graphics queue family"); return false; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = a.qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures enableFeats{};
    enableFeats.textureCompressionBC = feats.textureCompressionBC;
    VkDeviceCreateInfo dvci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dvci.queueCreateInfoCount = 1; dvci.pQueueCreateInfos = &qci;
    dvci.pEnabledFeatures = &enableFeats;
    if (vkCreateDevice(a.phys, &dvci, nullptr, &a.dev) != VK_SUCCESS) { LOG("vkCreateDevice"); return false; }
    vkGetDeviceQueue(a.dev, a.qfam, 0, &a.queue);

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = a.qfam;
    if (vkCreateCommandPool(a.dev, &cpi, nullptr, &a.cmdPool) != VK_SUCCESS) { LOG("vkCreateCommandPool"); return false; }
    return true;
}

// =============================================================================
// A one-shot VK command buffer helper.
// =============================================================================
static VkCommandBuffer beginOneShot(App& a) {
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = a.cmdPool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb; vkAllocateCommandBuffers(a.dev, &cbi, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}
static void endOneShot(App& a, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(a.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(a.queue);
    vkFreeCommandBuffers(a.dev, a.cmdPool, 1, &cb);
}

// =============================================================================
// Case descriptor. Each case names its source bytes + the GL/VK formats + the
// sampler filter + the source dimensions/layers.
// =============================================================================
struct Case {
    const char* name;
    uint32_t    srcW, srcH, layers;     // source texture geometry
    // GL upload
    GLenum      glInternal;             // e.g. GL_RGBA8 / GL_SRGB8_ALPHA8 / GL_COMPRESSED_RGBA_BPTC_UNORM
    GLenum      glFormat;               // GL_RGBA / GL_BGRA (ignored for compressed)
    GLenum      glType;                 // GL_UNSIGNED_BYTE (ignored for compressed)
    bool        compressed;            // BC7?
    bool        arrayTex;              // 2D_ARRAY?
    bool        linear;                // LINEAR filter (else NEAREST)?
    // VK
    VkFormat    vkFormat;
    // sample geometry: render target size, and the UV set (nearest = center-of-texel)
    uint32_t    dstW, dstH;
    float       sampleLayer;           // float layer index used by the shader (array case)
    // the raw source bytes (already in the memory order both APIs receive)
    std::vector<uint8_t> bytes;        // for uncompressed: srcW*srcH*layers*4 ; for BC7: blocks
    bool        rbSwapForVkNeg;        // negative control: VK uploads R/B-swapped bytes
};

// -------- sRGB reference (for logging only) ----------------------------------
static float srgbToLinear(float c) {
    return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// =============================================================================
// GL: upload source, render a fullscreen quad sampling it into an RGBA8 FBO,
// read back the dstW*dstH result as 8-bit RGBA.
// =============================================================================
static GLuint g_glSampleProg2D = 0, g_glSampleProgArr = 0;
static GLuint compileGlSampleProgram(bool arrayTex) {
    const char* vs =
        "#version 450 core\n"
        "out vec2 vUV;\n"
        "void main(){ const vec2 C[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));\n"
        "  vec2 q=C[gl_VertexID]; vUV=q; gl_Position=vec4(q*2.0-1.0,0.0,1.0); }\n";
    std::string fs =
        "#version 450 core\n";
    if (arrayTex)
        fs += "uniform sampler2DArray srcTex; uniform float uLayer;\n"
              "in vec2 vUV; layout(location=0) out vec4 o;\n"
              "void main(){ o=texture(srcTex, vec3(vUV,uLayer)); }\n";
    else
        fs += "uniform sampler2D srcTex; uniform float uLayer;\n"
              "in vec2 vUV; layout(location=0) out vec4 o;\n"
              "void main(){ o=texture(srcTex, vUV); }\n";
    const char* fsp = fs.c_str();
    GLuint v = glCreateShader(GL_VERTEX_SHADER); glShaderSource(v,1,&vs,nullptr); glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f,1,&fsp,nullptr); glCompileShader(f);
    GLint ok=0;
    glGetShaderiv(v,GL_COMPILE_STATUS,&ok); if(!ok){char l[512];glGetShaderInfoLog(v,512,nullptr,l);LOG("[GL vs]%s",l);}
    glGetShaderiv(f,GL_COMPILE_STATUS,&ok); if(!ok){char l[512];glGetShaderInfoLog(f,512,nullptr,l);LOG("[GL fs]%s",l);}
    GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

static bool glRunCase(const Case& c, std::vector<uint8_t>& outRGBA8 /*dstW*dstH*4*/) {
    GLenum target = c.arrayTex ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    GLuint tex; glGenTextures(1, &tex); glBindTexture(target, tex);
    GLenum filt = c.linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (c.compressed) {
        // BC7: 4x4 blocks, 16 bytes each.
        GLsizei imgSize = (GLsizei)c.bytes.size();
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, c.glInternal, c.srcW, c.srcH, 0, imgSize, c.bytes.data());
    } else if (c.arrayTex) {
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, c.glInternal, c.srcW, c.srcH, c.layers, 0,
                     c.glFormat, c.glType, c.bytes.data());
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, c.glInternal, c.srcW, c.srcH, 0,
                     c.glFormat, c.glType, c.bytes.data());
    }
    if (!glCheck("GL upload")) { glDeleteTextures(1,&tex); return false; }

    // RGBA8 render target.
    GLuint rt; glGenTextures(1,&rt); glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, c.dstW, c.dstH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint fbo; glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { LOG("GL rt FBO incomplete"); return false; }

    glViewport(0,0,c.dstW,c.dstH);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    // IMPORTANT: sampling an sRGB texture decodes sRGB->linear regardless of
    // GL_FRAMEBUFFER_SRGB; we must NOT re-encode on write, so keep it disabled
    // so the RGBA8 target stores the linear sample value directly (matches VK,
    // which writes the sampled linear value into an UNORM target).
    glDisable(GL_FRAMEBUFFER_SRGB);

    GLuint prog = c.arrayTex ? g_glSampleProgArr : g_glSampleProg2D;
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog,"srcTex"), 0);
    glUniform1f(glGetUniformLocation(prog,"uLayer"), c.sampleLayer);
    glActiveTexture(GL_TEXTURE0); glBindTexture(target, tex);
    GLuint vao; glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();

    outRGBA8.resize(c.dstW*c.dstH*4);
    glReadPixels(0,0,c.dstW,c.dstH,GL_RGBA,GL_UNSIGNED_BYTE,outRGBA8.data());
    bool ok = glCheck("GL sample+readback");

    glBindFramebuffer(GL_FRAMEBUFFER,0);
    glDeleteVertexArrays(1,&vao); glDeleteFramebuffers(1,&fbo);
    glDeleteTextures(1,&rt); glDeleteTextures(1,&tex);
    return ok;
}

// =============================================================================
// VK: staging -> copy -> sampled image; render fullscreen quad into RGBA8
// target; copy target -> host buffer; read back dstW*dstH RGBA8.
// =============================================================================
static bool vkRunCase(App& a, const Case& c, std::vector<uint8_t>& outRGBA8) {
    // ---- source bytes (optionally R/B swapped for the negative control) -----
    std::vector<uint8_t> src = c.bytes;
    if (c.rbSwapForVkNeg && !c.compressed) {
        for (size_t i = 0; i + 3 < src.size(); i += 4) std::swap(src[i], src[i+2]);
    }

    // ---- staging buffer -----------------------------------------------------
    VkBuffer stg; VkDeviceMemory stgMem;
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = src.size(); bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(a.dev,&bci,nullptr,&stg)!=VK_SUCCESS){LOG("vk stg buf");return false;}
        VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(a.dev,stg,&mr);
        uint32_t mi; findMemType(a,mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,&mi);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize=mr.size; mai.memoryTypeIndex=mi;
        vkAllocateMemory(a.dev,&mai,nullptr,&stgMem); vkBindBufferMemory(a.dev,stg,stgMem,0);
        void* pm; vkMapMemory(a.dev,stgMem,0,src.size(),0,&pm);
        std::memcpy(pm, src.data(), src.size()); vkUnmapMemory(a.dev,stgMem);
    }

    // ---- sampled image ------------------------------------------------------
    VkImage img; VkDeviceMemory imgMem;
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType=VK_IMAGE_TYPE_2D; ci.format=c.vkFormat;
        ci.extent={c.srcW,c.srcH,1}; ci.mipLevels=1; ci.arrayLayers=c.layers;
        ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(a.dev,&ci,nullptr,&img)!=VK_SUCCESS){LOG("vk img");return false;}
        VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(a.dev,img,&mr);
        uint32_t mi; findMemType(a,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&mi);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize=mr.size; mai.memoryTypeIndex=mi;
        vkAllocateMemory(a.dev,&mai,nullptr,&imgMem); vkBindImageMemory(a.dev,img,imgMem,0);
    }

    // ---- copy staging -> image (all layers), transition to SHADER_READ ------
    {
        VkCommandBuffer cb = beginOneShot(a);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=img;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,c.layers};
        b.srcAccessMask=0; b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);

        VkBufferImageCopy rgn{};
        rgn.bufferOffset=0; rgn.bufferRowLength=0; rgn.bufferImageHeight=0;
        rgn.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,c.layers};
        rgn.imageOffset={0,0,0}; rgn.imageExtent={c.srcW,c.srcH,1};
        vkCmdCopyBufferToImage(cb,stg,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&rgn);

        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
        endOneShot(a,cb);
    }

    // ---- image view + sampler ----------------------------------------------
    VkImageView view; VkSampler samp;
    {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image=img; vci.viewType=c.arrayTex?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
        vci.format=c.vkFormat;
        vci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,c.layers};
        if (vkCreateImageView(a.dev,&vci,nullptr,&view)!=VK_SUCCESS){LOG("vk view");return false;}
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        VkFilter f = c.linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        sci.magFilter=f; sci.minFilter=f; sci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod=0.25f;
        vkCreateSampler(a.dev,&sci,nullptr,&samp);
    }

    // ---- RGBA8 render target + framebuffer + renderpass ---------------------
    VkImage rt; VkDeviceMemory rtMem; VkImageView rtView;
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType=VK_IMAGE_TYPE_2D; ci.format=VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent={c.dstW,c.dstH,1}; ci.mipLevels=1; ci.arrayLayers=1;
        ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(a.dev,&ci,nullptr,&rt);
        VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(a.dev,rt,&mr);
        uint32_t mi; findMemType(a,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&mi);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize=mr.size; mai.memoryTypeIndex=mi;
        vkAllocateMemory(a.dev,&mai,nullptr,&rtMem); vkBindImageMemory(a.dev,rt,rtMem,0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image=rt; vci.viewType=VK_IMAGE_VIEW_TYPE_2D; vci.format=VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCreateImageView(a.dev,&vci,nullptr,&rtView);
    }
    VkRenderPass rp;
    {
        VkAttachmentDescription att{}; att.format=VK_FORMAT_R8G8B8A8_UNORM; att.samples=VK_SAMPLE_COUNT_1_BIT;
        att.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount=1; sub.pColorAttachments=&ref;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount=1; rpci.pAttachments=&att; rpci.subpassCount=1; rpci.pSubpasses=&sub;
        vkCreateRenderPass(a.dev,&rpci,nullptr,&rp);
    }
    VkFramebuffer fb;
    {
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass=rp; fci.attachmentCount=1; fci.pAttachments=&rtView;
        fci.width=c.dstW; fci.height=c.dstH; fci.layers=1;
        vkCreateFramebuffer(a.dev,&fci,nullptr,&fb);
    }

    // ---- descriptor set (combined image sampler at binding 0) ---------------
    VkDescriptorSetLayout dsl; VkDescriptorPool dpool; VkDescriptorSet dset;
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding=0; b.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount=1; b.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount=1; lci.pBindings=&b;
        vkCreateDescriptorSetLayout(a.dev,&lci,nullptr,&dsl);
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets=1; pci.poolSizeCount=1; pci.pPoolSizes=&ps;
        vkCreateDescriptorPool(a.dev,&pci,nullptr,&dpool);
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool=dpool; dai.descriptorSetCount=1; dai.pSetLayouts=&dsl;
        vkAllocateDescriptorSets(a.dev,&dai,&dset);
        VkDescriptorImageInfo dii{samp,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet=dset; w.dstBinding=0; w.descriptorCount=1;
        w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo=&dii;
        vkUpdateDescriptorSets(a.dev,1,&w,0,nullptr);
    }

    // ---- pipeline -----------------------------------------------------------
    VkShaderModule vs, fs;
    {
        VkShaderModuleCreateInfo mv{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mv.codeSize=sizeof(sampletex_vert_spv); mv.pCode=sampletex_vert_spv;
        vkCreateShaderModule(a.dev,&mv,nullptr,&vs);
        const uint32_t* fcode = c.arrayTex ? sampletex_array_frag_spv : sampletex_frag_spv;
        size_t fsz = c.arrayTex ? sizeof(sampletex_array_frag_spv) : sizeof(sampletex_frag_spv);
        VkShaderModuleCreateInfo mf{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mf.codeSize=fsz; mf.pCode=fcode;
        vkCreateShaderModule(a.dev,&mf,nullptr,&fs);
    }
    VkPipelineLayout pl; VkPipeline pipe;
    {
        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(float)*4};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount=1; plci.pSetLayouts=&dsl;
        plci.pushConstantRangeCount=1; plci.pPushConstantRanges=&pcr;
        vkCreatePipelineLayout(a.dev,&plci,nullptr,&pl);

        VkPipelineShaderStageCreateInfo st[2]{};
        st[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        st[0].stage=VK_SHADER_STAGE_VERTEX_BIT; st[0].module=vs; st[0].pName="main";
        st[1]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        st[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module=fs; st[1].pName="main";
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0,0,(float)c.dstW,(float)c.dstH,0,1};
        VkRect2D scr{{0,0},{c.dstW,c.dstH}};
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount=1; vps.pViewports=&vp; vps.scissorCount=1; vps.pScissors=&scr;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.lineWidth=1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask=0xF; cba.blendEnable=VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount=1; cb.pAttachments=&cba;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount=2; gp.pStages=st; gp.pVertexInputState=&vin; gp.pInputAssemblyState=&ia;
        gp.pViewportState=&vps; gp.pRasterizationState=&rs; gp.pMultisampleState=&ms;
        gp.pDepthStencilState=&ds; gp.pColorBlendState=&cb;
        gp.layout=pl; gp.renderPass=rp; gp.subpass=0;
        if (vkCreateGraphicsPipelines(a.dev,VK_NULL_HANDLE,1,&gp,nullptr,&pipe)!=VK_SUCCESS){LOG("vk pipeline");return false;}
    }

    // ---- host-readable readback buffer --------------------------------------
    VkBuffer rb; VkDeviceMemory rbMem;
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size=c.dstW*c.dstH*4; bci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(a.dev,&bci,nullptr,&rb);
        VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(a.dev,rb,&mr);
        uint32_t mi; findMemType(a,mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,&mi);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize=mr.size; mai.memoryTypeIndex=mi;
        vkAllocateMemory(a.dev,&mai,nullptr,&rbMem); vkBindBufferMemory(a.dev,rb,rbMem,0);
    }

    // ---- draw + copy target -> readback buffer ------------------------------
    {
        VkCommandBuffer cb = beginOneShot(a);
        VkClearValue clr{}; clr.color={{0,0,0,1}};
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass=rp; rbi.framebuffer=fb; rbi.renderArea={{0,0},{c.dstW,c.dstH}};
        rbi.clearValueCount=1; rbi.pClearValues=&clr;
        vkCmdBeginRenderPass(cb,&rbi,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&dset,0,nullptr);
        float pc[4]={c.sampleLayer,0,0,0};
        vkCmdPushConstants(cb,pl,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(pc),pc);
        vkCmdDraw(cb,6,1,0,0);
        vkCmdEndRenderPass(cb);
        // renderpass finalLayout already TRANSFER_SRC_OPTIMAL.
        VkBufferImageCopy rgn{};
        rgn.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        rgn.imageExtent={c.dstW,c.dstH,1};
        vkCmdCopyImageToBuffer(cb,rt,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,rb,1,&rgn);
        endOneShot(a,cb);
    }
    outRGBA8.resize(c.dstW*c.dstH*4);
    { void* pm; vkMapMemory(a.dev,rbMem,0,c.dstW*c.dstH*4,0,&pm);
      std::memcpy(outRGBA8.data(),pm,c.dstW*c.dstH*4); vkUnmapMemory(a.dev,rbMem); }

    // ---- teardown -----------------------------------------------------------
    vkDeviceWaitIdle(a.dev);
    vkDestroyPipeline(a.dev,pipe,nullptr); vkDestroyPipelineLayout(a.dev,pl,nullptr);
    vkDestroyShaderModule(a.dev,vs,nullptr); vkDestroyShaderModule(a.dev,fs,nullptr);
    vkDestroyDescriptorPool(a.dev,dpool,nullptr); vkDestroyDescriptorSetLayout(a.dev,dsl,nullptr);
    vkDestroyFramebuffer(a.dev,fb,nullptr); vkDestroyRenderPass(a.dev,rp,nullptr);
    vkDestroyImageView(a.dev,rtView,nullptr); vkDestroyImage(a.dev,rt,nullptr); vkFreeMemory(a.dev,rtMem,nullptr);
    vkDestroySampler(a.dev,samp,nullptr); vkDestroyImageView(a.dev,view,nullptr);
    vkDestroyImage(a.dev,img,nullptr); vkFreeMemory(a.dev,imgMem,nullptr);
    vkDestroyBuffer(a.dev,stg,nullptr); vkFreeMemory(a.dev,stgMem,nullptr);
    vkDestroyBuffer(a.dev,rb,nullptr); vkFreeMemory(a.dev,rbMem,nullptr);
    return true;
}

// =============================================================================
// Compare GL vs VK RGBA8 readbacks with a per-channel LSB tolerance.
// =============================================================================
static bool compareCase(const Case& c, const std::vector<uint8_t>& gl,
                        const std::vector<uint8_t>& vk, int tol) {
    if (gl.size()!=vk.size() || gl.empty()) { LOG("   size mismatch gl=%zu vk=%zu", gl.size(), vk.size()); return false; }
    int maxDiff=0, nBad=0;
    for (size_t i=0;i<gl.size();++i){ int d=std::abs((int)gl[i]-(int)vk[i]); if(d>maxDiff)maxDiff=d; if(d>tol)nBad++; }
    // Log a representative texel (center).
    size_t ct=((c.dstH/2)*c.dstW+(c.dstW/2))*4;
    LOG("   center texel GL=(%3u,%3u,%3u,%3u)  VK=(%3u,%3u,%3u,%3u)  maxDiff=%d tol=%d badChans=%d",
        gl[ct],gl[ct+1],gl[ct+2],gl[ct+3], vk[ct],vk[ct+1],vk[ct+2],vk[ct+3], maxDiff, tol, nBad);
    return nBad==0;
}

// =============================================================================
// Build a solid-color BC7 mode-6 block (16 bytes). With both endpoints equal,
// the decoded output equals the endpoint color for every pixel regardless of
// index bits. This is a spec-valid BC7 block usable verbatim by GL and VK.
//   Mode 6 bit layout (LSB first):
//     [0]      mode marker : 1 bit set at bit 0
//     [1..7]   R0 (7b) [8..14] R1 [15..21] G0 [22..28] G1
//     [29..35] B0 [36..42] B1 [43..49] A0 [50..56] A1
//     [57] p0  [58] p1
//     [59..63.. up to 127] index bits (16 px * 4b, first is 3b) -> all zero = index 0 = endpoint0
//   Effective 8-bit channel value = (endpoint7 << 1) | pbit.
// =============================================================================
static void bc7SolidBlock(uint8_t r,uint8_t g,uint8_t b,uint8_t alpha, uint8_t out[16]) {
    auto putbits=[&](int start,int count,uint64_t val,uint8_t* dst){
        for(int i=0;i<count;i++){ int bit=start+i; if(val&(1ull<<i)) dst[bit>>3]|=(1u<<(bit&7)); }
    };
    std::memset(out,0,16);
    // We want decoded 8-bit == r/g/b/a. 8-bit value = (e7<<1)|p. Choose p=lsb, e7=val>>1.
    auto e7=[&](uint8_t v)->uint64_t{ return (uint64_t)(v>>1); };
    uint64_t pr = r&1, pg=g&1, pb=b&1, pa=alpha&1;
    // A single shared p-bit pair applies: p0 to endpoint0, p1 to endpoint1. Both
    // endpoints identical, both p-bits = we need one p per endpoint, not per chan.
    // Mode 6 has a UNIQUE p-bit per endpoint (2 total). To hit exact 8-bit values
    // for all channels we need channel p-bits, which mode6 lacks. So instead pick
    // colors whose low bit is identical across channels, OR accept (e7<<1)|p.
    // Simplify: set p0=p1=0 and use e7=val>>1 -> decoded = (val>>1)<<1 = val & 0xFE.
    // We therefore choose test colors that are even (low bit 0) so decode is exact.
    (void)pr;(void)pg;(void)pb;(void)pa;
    putbits(0,1,1,out);                 // mode 6 marker
    putbits(1,7,e7(r),out);  putbits(8,7,e7(r),out);   // R0=R1
    putbits(15,7,e7(g),out); putbits(22,7,e7(g),out);  // G0=G1
    putbits(29,7,e7(b),out); putbits(36,7,e7(b),out);  // B0=B1
    putbits(43,7,e7(alpha),out); putbits(50,7,e7(alpha),out); // A0=A1
    // p0=p1=0 (bits 57,58 left 0). Index bits all 0 -> pick endpoint0 everywhere.
}

// =============================================================================
static struct { const char* name; bool pass; bool ran; } g_results[16];
static int g_nres=0;

int main(int, char**) {
    const char* negEnv = std::getenv("MC2_SAMPLETEX_NEGCTL");
    bool neg = negEnv && !std::strcmp(negEnv,"rbswap");

    LOG("==============================================================");
    LOG(" VK-sampled == GL-sampled parity spike (source-texture STOP-gate)");
    LOG("   Each case: same source bytes -> GL sample & VK sample -> compare.");
    LOG("   negative control (rbswap) = %s", neg ? "ON (BGRA case MUST fail)" : "off");
    LOG("==============================================================");

    App a;
    if (!initGL(a) || !initVk(a)) { LOG("VERDICT: GATE RED — init failed."); return 1; }
    g_glSampleProg2D  = compileGlSampleProgram(false);
    g_glSampleProgArr = compileGlSampleProgram(true);
    glCheck("compile GL sample programs");

    std::vector<Case> cases;

    // ---- Case 1: 2D RGBA8 -----------------------------------------------------
    {
        Case c{}; c.name="2D_RGBA8"; c.srcW=4; c.srcH=4; c.layers=1;
        c.glInternal=GL_RGBA8; c.glFormat=GL_RGBA; c.glType=GL_UNSIGNED_BYTE;
        c.vkFormat=VK_FORMAT_R8G8B8A8_UNORM; c.dstW=4; c.dstH=4;
        c.bytes.resize(4*4*4);
        for(int i=0;i<16;i++){ c.bytes[i*4+0]=(uint8_t)(i*16); c.bytes[i*4+1]=(uint8_t)(255-i*16);
                               c.bytes[i*4+2]=(uint8_t)(i*8+7); c.bytes[i*4+3]=255; }
        cases.push_back(std::move(c));
    }
    // ---- Case 2: 2D RGBA8 uploaded as BGRA (GL format=GL_BGRA) ---------------
    // Source bytes are in BGRA memory order. GL is told format=GL_BGRA so it
    // reorders to RGBA internally. VK has no BGRA->RGBA on upload for an
    // R8G8B8A8 image, so we use VK_FORMAT_B8G8R8A8_UNORM to interpret the SAME
    // bytes as BGRA. Both must yield the same RGBA sample (R/B NOT flipped).
    {
        Case c{}; c.name="2D_RGBA8_BGRA_order"; c.srcW=4; c.srcH=4; c.layers=1;
        c.glInternal=GL_RGBA8; c.glFormat=GL_BGRA; c.glType=GL_UNSIGNED_BYTE;
        c.vkFormat=VK_FORMAT_B8G8R8A8_UNORM; c.dstW=4; c.dstH=4;
        c.bytes.resize(4*4*4);
        // memory order B,G,R,A. Pick a strongly R!=B color: R=200 G=40 B=10.
        for(int i=0;i<16;i++){ c.bytes[i*4+0]=10; c.bytes[i*4+1]=40; c.bytes[i*4+2]=200; c.bytes[i*4+3]=255; }
        c.rbSwapForVkNeg = neg;   // negative control corrupts only this case
        cases.push_back(std::move(c));
    }
    // ---- Case 3: 2D_ARRAY RGBA8, 3 layers; sample layer 1 -------------------
    {
        Case c{}; c.name="2D_ARRAY_RGBA8_layer1"; c.srcW=4; c.srcH=4; c.layers=3;
        c.glInternal=GL_RGBA8; c.glFormat=GL_RGBA; c.glType=GL_UNSIGNED_BYTE;
        c.vkFormat=VK_FORMAT_R8G8B8A8_UNORM; c.arrayTex=true; c.dstW=4; c.dstH=4;
        c.sampleLayer=1.0f;
        c.bytes.resize(4*4*4*3);
        const uint8_t layerColor[3][3]={{255,0,0},{0,255,0},{0,0,255}};
        for(uint32_t L=0;L<3;L++) for(int i=0;i<16;i++){
            size_t o=(L*16+i)*4; c.bytes[o]=layerColor[L][0]; c.bytes[o+1]=layerColor[L][1];
            c.bytes[o+2]=layerColor[L][2]; c.bytes[o+3]=255; }
        cases.push_back(std::move(c));   // expect GREEN (layer 1)
    }
    // ---- Case 4: BC7 UNORM (solid even-valued color) ------------------------
    {
        Case c{}; c.name="BC7_UNORM"; c.srcW=4; c.srcH=4; c.layers=1; c.compressed=true;
        c.glInternal=GL_COMPRESSED_RGBA_BPTC_UNORM;
        c.vkFormat=VK_FORMAT_BC7_UNORM_BLOCK; c.dstW=4; c.dstH=4;
        uint8_t blk[16]; bc7SolidBlock(120,60,200,254, blk);  // even-valued channels
        c.bytes.assign(blk,blk+16);
        cases.push_back(std::move(c));
    }
    // ---- Case 5: BC7 sRGB (solid even-valued color) -------------------------
    // Same block bytes but sRGB format: sample must sRGB->linear decode equally.
    {
        Case c{}; c.name="BC7_SRGB"; c.srcW=4; c.srcH=4; c.layers=1; c.compressed=true;
        c.glInternal=GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM;
        c.vkFormat=VK_FORMAT_BC7_SRGB_BLOCK; c.dstW=4; c.dstH=4;
        uint8_t blk[16]; bc7SolidBlock(120,60,200,254, blk);
        c.bytes.assign(blk,blk+16);
        // Log expected linear value for the R channel for context.
        LOG("[note] BC7_SRGB stored R=120/255 -> linear ~%.4f (expected in RGBA8 target ~%d)",
            srgbToLinear(120.0f/255.0f), (int)std::lround(srgbToLinear(120.0f/255.0f)*255.0f));
        cases.push_back(std::move(c));
    }
    // ---- Case 6: LINEAR filter at non-integer UV ----------------------------
    // 2x1 texture: left texel black, right texel white. Fullscreen quad over a
    // 4x1 target samples UV centers 0.125,0.375,0.625,0.875 -> bilinear ramp.
    {
        Case c{}; c.name="LINEAR_FILTER_bilinear"; c.srcW=2; c.srcH=1; c.layers=1;
        c.glInternal=GL_RGBA8; c.glFormat=GL_RGBA; c.glType=GL_UNSIGNED_BYTE;
        c.vkFormat=VK_FORMAT_R8G8B8A8_UNORM; c.linear=true; c.dstW=4; c.dstH=1;
        c.bytes.resize(2*4);
        c.bytes[0]=0;   c.bytes[1]=0;   c.bytes[2]=0;   c.bytes[3]=255; // left black
        c.bytes[4]=255; c.bytes[5]=255; c.bytes[6]=255; c.bytes[7]=255; // right white
        cases.push_back(std::move(c));
    }

    int nPass=0, nRun=0;
    for (const Case& c : cases) {
        LOG("");
        LOG("=== CASE %s  (src %ux%ux%u, filter=%s) ===",
            c.name, c.srcW, c.srcH, c.layers, c.linear?"LINEAR":"NEAREST");
        std::vector<uint8_t> glOut, vkOut;
        bool glOk = glRunCase(c, glOut);
        bool vkOk = vkRunCase(a, c, vkOut);
        bool pass=false;
        if (glOk && vkOk) {
            // Tolerance: exact for NEAREST/UNORM; a few LSB for LINEAR and sRGB decode.
            int tol = 1;
            if (c.linear) tol = 2;
            if (c.vkFormat==VK_FORMAT_BC7_SRGB_BLOCK) tol = 2;
            pass = compareCase(c, glOut, vkOut, tol);
        } else {
            LOG("   [run error] glOk=%d vkOk=%d", glOk?1:0, vkOk?1:0);
        }
        LOG("   -> %s : %s", c.name, pass ? "PASS" : "FAIL");
        g_results[g_nres].name=c.name; g_results[g_nres].pass=pass; g_results[g_nres].ran=true; g_nres++;
        nRun++; if(pass) nPass++;
    }

    LOG("");
    LOG("--------------------------------------------------------------");
    for (int i=0;i<g_nres;i++) LOG("  %-26s %s", g_results[i].name, g_results[i].pass?"PASS":"FAIL");
    LOG("--------------------------------------------------------------");
    LOG("[TALLY] cases passed              = %d / %d", nPass, nRun);
    LOG("[TALLY] VK validation errors      = %d", g_valErrors);
    LOG("[TALLY] GL debug errors seen      = %d", g_glDebugSawError?1:0);

    int rc;
    if (neg) {
        // Negative control: the BGRA case MUST fail (VK got R/B-swapped bytes).
        bool bgraFailed=false;
        for(int i=0;i<g_nres;i++) if(!std::strcmp(g_results[i].name,"2D_RGBA8_BGRA_order") && !g_results[i].pass) bgraFailed=true;
        if (bgraFailed) { LOG("VERDICT: NEG-CONTROL OK — injected R/B swap correctly DETECTED (BGRA case failed)."); rc=0; }
        else            { LOG("VERDICT: NEG-CONTROL BROKEN — harness did NOT detect the R/B swap."); rc=2; }
    } else {
        bool allPass = (nPass==nRun) && (g_valErrors==0) && !g_glDebugSawError;
        if (allPass) {
            LOG("VERDICT: STOP-GATE GREEN — VK-sampled == GL-sampled across");
            LOG("         2D / BGRA / 2D_ARRAY / BC7 / sRGB / LINEAR.");
            LOG("         GL->VK source-texture copy-once seam UNBLOCKED.");
            rc=0;
        } else {
            LOG("VERDICT: STOP-GATE RED — a format axis mismatched (see per-case above).");
            rc=1;
        }
    }
    LOG("==============================================================");

    glDeleteProgram(g_glSampleProg2D); glDeleteProgram(g_glSampleProgArr);
    vkDeviceWaitIdle(a.dev);
    vkDestroyCommandPool(a.dev,a.cmdPool,nullptr);
    vkDestroyDevice(a.dev,nullptr);
    if (a.messenger && a.pDestroyDbg) a.pDestroyDbg(a.inst,a.messenger,nullptr);
    vkDestroyInstance(a.inst,nullptr);
    if (a.glctx) SDL_GL_DeleteContext(a.glctx);
    if (a.win) SDL_DestroyWindow(a.win);
    SDL_Quit();
    return rc;
}
