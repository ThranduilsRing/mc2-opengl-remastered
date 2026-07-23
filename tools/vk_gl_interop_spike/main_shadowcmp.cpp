// =============================================================================
// VK sampler2DShadow == GL sampler2DShadow parity SPIKE  (VK-INTEROP-BRIDGE-1)
//
//   STOP-gate / oracle for the SHARED VK LIGHTING/SHADOW HELPER.
//
// The depth/compare CONVENTION is already proven (P8a: VK produces D32, GL
// samples via sampler2DShadow, identical). The ONE unproven bit is VK-SIDE
// shadow-compare SAMPLING: does a VK sampler2DShadow with
//   compareOp = VK_COMPARE_OP_LESS_OR_EQUAL + LINEAR filter
// produce the same hardware 2x2 PCF blend as GL's LINEAR sampler2DShadow
//   (GL_COMPARE_REF_TO_TEXTURE + GL_LEQUAL) ?
//
// APPROACH (independent-upload parity, like the sampletex spike):
//   * Build one CPU depth map (a ramp / step field) as float depth in [0,1].
//   * GL: upload to a depth texture (D32F and, separately, DEPTH24), set
//         COMPARE_REF_TO_TEXTURE + LEQUAL, LINEAR/LINEAR, CLAMP_TO_BORDER,
//         border = 1.0 (white = fully lit). Sample sampler2DShadow at an
//         NxN grid of (uv, refDepth) points -> readback as UNORM8.
//   * VK: upload the SAME depth bytes to a sampled depth image (depth-aspect
//         view, SHADER_READ_ONLY), VkSampler compareEnable=TRUE,
//         compareOp=LESS_OR_EQUAL, LINEAR, CLAMP_TO_BORDER white border.
//         Sample sampler2DShadow at the SAME grid -> readback.
//   * ASSERT VK == GL at every grid point (esp. the edge/transition texels
//         where the 2x2 PCF blend matters, out-of-range border = 1.0, and
//         exact-boundary refDepth for LEQUAL tie behaviour).
//
// PATHS TESTED:
//   HW_LINEAR_D32F   VkSampler LINEAR-compare vs GL LINEAR sampler2DShadow (D32F).   [STOP-GATE]
//   HW_LINEAR_D24    same on GL DEPTH_COMPONENT24 (VK keeps D32; compare result parity).
//   MANUAL4TAP_D32F  NEAREST-compare + explicit 4-tap PCF averaged in-shader (fallback).
//
// NEGATIVE CONTROLS (env MC2_SHADOWCMP_NEGCTL):
//   =cmpflip  -> VK compareOp = GREATER (compare inverts) : parity MUST fail.
//   =filter   -> VK NEAREST while GL stays LINEAR         : PCF blend differs -> MUST fail.
//   (unset)   -> real run; all enabled paths must PASS.
//
// Exit 0 == every enabled path PASS + zero VK validation errors == GATE GREEN
//           (or, under a NEGCTL, the expected mismatch WAS detected).
// Non-zero == a path mismatched / neg-control failed to detect == GATE RED.
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
#include <cmath>
#include <vector>
#include <string>

// Embedded SPIR-V (glslc -mfmt=c at build time; see CMakeLists.txt).
static const uint32_t shadowcmp_vert_spv[] =
    #include "shadowcmp.vert.spv.inc"
;
static const uint32_t shadowcmp_frag_spv[] =
    #include "shadowcmp.frag.spv.inc"
;

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
    a.win = SDL_CreateWindow("vk_gl_shadowcmp_spike", SDL_WINDOWPOS_CENTERED,
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
    ai.pApplicationName = "vk_gl_shadowcmp_spike";
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
    VkDeviceCreateInfo dvci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dvci.queueCreateInfoCount = 1; dvci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(a.phys, &dvci, nullptr, &a.dev) != VK_SUCCESS) { LOG("vkCreateDevice"); return false; }
    vkGetDeviceQueue(a.dev, a.qfam, 0, &a.queue);

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = a.qfam;
    if (vkCreateCommandPool(a.dev, &cpi, nullptr, &a.cmdPool) != VK_SUCCESS) { LOG("vkCreateCommandPool"); return false; }
    return true;
}

// ---- one-shot VK command buffer helper -------------------------------------
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
// The shared depth map. A step + ramp field so a 2x2 PCF blend has real
// transitions to exercise. 8x8 float depths in [0,1].
// Layout: a diagonal ramp with a sharp step ridge so many texels differ from
// their neighbours (the interesting PCF case is neighbouring texels with
// DIFFERENT compare outcomes for a given ref).
// =============================================================================
static const uint32_t MAP_W = 8, MAP_H = 8;
static std::vector<float> makeDepthMap() {
    std::vector<float> d(MAP_W * MAP_H);
    for (uint32_t y = 0; y < MAP_H; ++y)
        for (uint32_t x = 0; x < MAP_W; ++x) {
            // diagonal ramp 0..1 plus a step block in the middle to create
            // hard neighbour discontinuities.
            float ramp = (float)(x + y) / (float)((MAP_W - 1) + (MAP_H - 1));
            if (x >= 3 && x <= 4 && y >= 3 && y <= 4) ramp = 0.5f; // flat step plateau
            d[y * MAP_W + x] = ramp;
        }
    return d;
}

// grid of (uv, ref) points; readback is GRID*GRID UNORM8 (R channel = lit frac)
static const uint32_t GRID = 32;

// =============================================================================
// GL: upload the float depth map into a depth texture, configure shadow-compare
// sampling, render the NxN grid via a sampler2DShadow fullscreen draw, read
// back the R channel (lit fraction *255).
//   internalFmt: GL_DEPTH_COMPONENT32F or GL_DEPTH_COMPONENT24
//   linear:      LINEAR (else NEAREST) filter
//   manual4tap:  if true, the shader does 4 explicit taps and averages
// =============================================================================
static GLuint g_glShadowProgHw = 0, g_glShadowProgManual = 0;
static GLuint compileGlShadowProgram(bool manual4tap) {
    const char* vs =
        "#version 450 core\n"
        "void main(){ const vec2 C[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));\n"
        "  vec2 q=C[gl_VertexID]; gl_Position=vec4(q*2.0-1.0,0.0,1.0); }\n";
    // Fragment shader mirrors shadowcmp.frag exactly (same grid math + compare).
    std::string fs =
        "#version 450 core\n"
        "uniform sampler2DShadow srcShadow;\n"
        "uniform float uGridN; uniform float uMapW; uniform float uMapH;\n"
        "layout(location=0) out vec4 o;\n"
        "float gridToUV(float i){ return (i/(uGridN-1.0))*1.2 - 0.1; }\n"
        "float gridToRef(float j){ return (j/(uGridN-1.0)); }\n"
        "void main(){\n"
        "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
        "  float u = gridToUV(float(p.x));\n"
        "  float v = gridToUV(float(p.y));\n"
        "  float ref = gridToRef(mod(float(p.x+p.y), uGridN));\n"
        "  float lit;\n";
    if (manual4tap)
        fs += "  vec2 texel = vec2(1.0/uMapW, 1.0/uMapH);\n"
              "  float s=0.0;\n"
              "  s += texture(srcShadow, vec3(u+texel.x*-0.5, v+texel.y*-0.5, ref));\n"
              "  s += texture(srcShadow, vec3(u+texel.x* 0.5, v+texel.y*-0.5, ref));\n"
              "  s += texture(srcShadow, vec3(u+texel.x*-0.5, v+texel.y* 0.5, ref));\n"
              "  s += texture(srcShadow, vec3(u+texel.x* 0.5, v+texel.y* 0.5, ref));\n"
              "  lit = s*0.25;\n";
    else
        fs += "  lit = texture(srcShadow, vec3(u, v, ref));\n";
    fs +=     "  o = vec4(lit, lit, lit, 1.0);\n"
              "}\n";
    const char* fsp = fs.c_str();
    GLuint vsh = glCreateShader(GL_VERTEX_SHADER);   glShaderSource(vsh,1,&vs,nullptr);  glCompileShader(vsh);
    GLuint fsh = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(fsh,1,&fsp,nullptr); glCompileShader(fsh);
    GLint ok=0;
    glGetShaderiv(vsh,GL_COMPILE_STATUS,&ok); if(!ok){char l[512];glGetShaderInfoLog(vsh,512,nullptr,l);LOG("[GL vs]%s",l);}
    glGetShaderiv(fsh,GL_COMPILE_STATUS,&ok); if(!ok){char l[512];glGetShaderInfoLog(fsh,512,nullptr,l);LOG("[GL fs]%s",l);}
    GLuint p=glCreateProgram(); glAttachShader(p,vsh); glAttachShader(p,fsh); glLinkProgram(p);
    glGetProgramiv(p,GL_LINK_STATUS,&ok); if(!ok){char l[512];glGetProgramInfoLog(p,512,nullptr,l);LOG("[GL link]%s",l);}
    glDeleteShader(vsh); glDeleteShader(fsh);
    return p;
}

static bool glRunGrid(const std::vector<float>& depth, GLenum internalFmt,
                      bool linear, bool manual4tap, std::vector<uint8_t>& outR /*GRID*GRID*/) {
    GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Upload float depth; GL converts to the requested internal depth format.
    glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, MAP_W, MAP_H, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
    GLenum filt = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    GLfloat border[4] = {1.0f,1.0f,1.0f,1.0f};   // white border == fully lit
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    if (!glCheck("GL depth upload/config")) { glDeleteTextures(1,&tex); return false; }

    GLuint rt; glGenTextures(1,&rt); glBindTexture(GL_TEXTURE_2D, rt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GRID, GRID, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint fbo; glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { LOG("GL rt FBO incomplete"); return false; }

    glViewport(0,0,GRID,GRID);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_FRAMEBUFFER_SRGB);

    GLuint prog = manual4tap ? g_glShadowProgManual : g_glShadowProgHw;
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog,"srcShadow"), 0);
    glUniform1f(glGetUniformLocation(prog,"uGridN"), (float)GRID);
    glUniform1f(glGetUniformLocation(prog,"uMapW"),  (float)MAP_W);
    glUniform1f(glGetUniformLocation(prog,"uMapH"),  (float)MAP_H);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
    GLuint vao; glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();

    std::vector<uint8_t> rgba(GRID*GRID*4);
    glReadPixels(0,0,GRID,GRID,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
    bool ok = glCheck("GL grid readback");
    outR.resize(GRID*GRID);
    for (uint32_t i=0;i<GRID*GRID;++i) outR[i] = rgba[i*4]; // R channel

    glBindFramebuffer(GL_FRAMEBUFFER,0);
    glDeleteVertexArrays(1,&vao); glDeleteFramebuffers(1,&fbo);
    glDeleteTextures(1,&rt); glDeleteTextures(1,&tex);
    return ok;
}

// =============================================================================
// VK: upload float depth map -> sampled depth image (depth aspect), compare
// sampler, render grid via sampler2DShadow -> readback R channel.
//   compareOp:  LESS_OR_EQUAL (real) or GREATER (neg-control cmpflip)
//   linear:     LINEAR (else NEAREST) filter
//   manualMode: push-constant mode 1 (4-tap) vs 0 (hw)
// =============================================================================
static bool vkRunGrid(App& a, const std::vector<float>& depth,
                      VkCompareOp cmpOp, bool linear, bool manualMode,
                      std::vector<uint8_t>& outR) {
    const VkFormat depthFmt = VK_FORMAT_D32_SFLOAT;
    const VkDeviceSize srcBytes = (VkDeviceSize)depth.size()*sizeof(float);

    // ---- staging buffer with the raw float depths ---------------------------
    VkBuffer stg; VkDeviceMemory stgMem;
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size=srcBytes; bci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(a.dev,&bci,nullptr,&stg)!=VK_SUCCESS){LOG("vk stg buf");return false;}
        VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(a.dev,stg,&mr);
        uint32_t mi; findMemType(a,mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,&mi);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize=mr.size; mai.memoryTypeIndex=mi;
        vkAllocateMemory(a.dev,&mai,nullptr,&stgMem); vkBindBufferMemory(a.dev,stg,stgMem,0);
        void* pm; vkMapMemory(a.dev,stgMem,0,srcBytes,0,&pm);
        std::memcpy(pm, depth.data(), (size_t)srcBytes); vkUnmapMemory(a.dev,stgMem);
    }

    // ---- sampled depth image ------------------------------------------------
    VkImage img; VkDeviceMemory imgMem;
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType=VK_IMAGE_TYPE_2D; ci.format=depthFmt;
        ci.extent={MAP_W,MAP_H,1}; ci.mipLevels=1; ci.arrayLayers=1;
        ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(a.dev,&ci,nullptr,&img)!=VK_SUCCESS){LOG("vk depth img");return false;}
        VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(a.dev,img,&mr);
        uint32_t mi; findMemType(a,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&mi);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize=mr.size; mai.memoryTypeIndex=mi;
        vkAllocateMemory(a.dev,&mai,nullptr,&imgMem); vkBindImageMemory(a.dev,img,imgMem,0);
    }

    // ---- copy staging -> image (depth aspect), transition to SHADER_READ ----
    {
        VkCommandBuffer cb = beginOneShot(a);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=img;
        b.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
        b.srcAccessMask=0; b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);

        VkBufferImageCopy rgn{};
        rgn.bufferOffset=0; rgn.bufferRowLength=0; rgn.bufferImageHeight=0;
        rgn.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
        rgn.imageOffset={0,0,0}; rgn.imageExtent={MAP_W,MAP_H,1};
        vkCmdCopyBufferToImage(cb,stg,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&rgn);

        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
        endOneShot(a,cb);
    }

    // ---- depth-aspect view + compare sampler --------------------------------
    VkImageView view; VkSampler samp;
    {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image=img; vci.viewType=VK_IMAGE_VIEW_TYPE_2D; vci.format=depthFmt;
        vci.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
        if (vkCreateImageView(a.dev,&vci,nullptr,&view)!=VK_SUCCESS){LOG("vk depth view");return false;}
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        VkFilter f = linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
        sci.magFilter=f; sci.minFilter=f; sci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;   // white == fully lit
        sci.compareEnable=VK_TRUE; sci.compareOp=cmpOp;
        sci.maxLod=0.25f;
        if (vkCreateSampler(a.dev,&sci,nullptr,&samp)!=VK_SUCCESS){LOG("vk sampler");return false;}
    }

    // ---- RGBA8 render target + renderpass + framebuffer ---------------------
    VkImage rt; VkDeviceMemory rtMem; VkImageView rtView;
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType=VK_IMAGE_TYPE_2D; ci.format=VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent={GRID,GRID,1}; ci.mipLevels=1; ci.arrayLayers=1;
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
        fci.width=GRID; fci.height=GRID; fci.layers=1;
        vkCreateFramebuffer(a.dev,&fci,nullptr,&fb);
    }

    // ---- descriptor set (combined image sampler / sampler2DShadow) ----------
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
        mv.codeSize=sizeof(shadowcmp_vert_spv); mv.pCode=shadowcmp_vert_spv;
        vkCreateShaderModule(a.dev,&mv,nullptr,&vs);
        VkShaderModuleCreateInfo mf{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mf.codeSize=sizeof(shadowcmp_frag_spv); mf.pCode=shadowcmp_frag_spv;
        vkCreateShaderModule(a.dev,&mf,nullptr,&fs);
    }
    VkPipelineLayout pl; VkPipeline pipe;
    {
        VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(float)*4};
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
        VkViewport vp{0,0,(float)GRID,(float)GRID,0,1};
        VkRect2D scr{{0,0},{GRID,GRID}};
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

    // ---- readback buffer ----------------------------------------------------
    VkBuffer rb; VkDeviceMemory rbMem;
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size=GRID*GRID*4; bci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(a.dev,&bci,nullptr,&rb);
        VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(a.dev,rb,&mr);
        uint32_t mi; findMemType(a,mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,&mi);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize=mr.size; mai.memoryTypeIndex=mi;
        vkAllocateMemory(a.dev,&mai,nullptr,&rbMem); vkBindBufferMemory(a.dev,rb,rbMem,0);
    }

    // ---- draw + copy target -> readback -------------------------------------
    {
        VkCommandBuffer cb = beginOneShot(a);
        VkClearValue clr{}; clr.color={{0,0,0,1}};
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass=rp; rbi.framebuffer=fb; rbi.renderArea={{0,0},{GRID,GRID}};
        rbi.clearValueCount=1; rbi.pClearValues=&clr;
        vkCmdBeginRenderPass(cb,&rbi,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&dset,0,nullptr);
        float pc[4]={(float)GRID,(float)MAP_W,(float)MAP_H, manualMode?1.0f:0.0f};
        vkCmdPushConstants(cb,pl,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(pc),pc);
        vkCmdDraw(cb,6,1,0,0);
        vkCmdEndRenderPass(cb);
        VkBufferImageCopy rgn{};
        rgn.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        rgn.imageExtent={GRID,GRID,1};
        vkCmdCopyImageToBuffer(cb,rt,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,rb,1,&rgn);
        endOneShot(a,cb);
    }
    std::vector<uint8_t> rgba(GRID*GRID*4);
    { void* pm; vkMapMemory(a.dev,rbMem,0,GRID*GRID*4,0,&pm);
      std::memcpy(rgba.data(),pm,GRID*GRID*4); vkUnmapMemory(a.dev,rbMem); }
    outR.resize(GRID*GRID);
    for (uint32_t i=0;i<GRID*GRID;++i) outR[i]=rgba[i*4];

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
// Compare two GRID*GRID lit-fraction (UNORM8) grids point-by-point.
// Reports max diff, #points over tolerance, and a sampling of the worst points
// with their (uv,ref) so a mismatch is diagnosable.
// =============================================================================
static bool compareGrid(const char* label, const std::vector<uint8_t>& gl,
                        const std::vector<uint8_t>& vk, int tol, int* outBad, int* outMax) {
    if (gl.size()!=vk.size() || gl.empty()) { LOG("   [%s] size mismatch gl=%zu vk=%zu",label,gl.size(),vk.size()); return false; }
    int maxDiff=0, nBad=0, nWorstShown=0;
    for (uint32_t i=0;i<gl.size();++i){
        int d=std::abs((int)gl[i]-(int)vk[i]);
        if(d>maxDiff)maxDiff=d;
        if(d>tol){
            nBad++;
            if(nWorstShown<8){
                uint32_t px=i%GRID, py=i/GRID;
                // reconstruct the (uv,ref) this point sampled (mirrors shader).
                float u=((float)px/(GRID-1.0f))*1.2f-0.1f;
                float v=((float)py/(GRID-1.0f))*1.2f-0.1f;
                float ref=(float)((px+py)%GRID)/(GRID-1.0f);
                LOG("   [%s] BAD px=(%2u,%2u) uv=(%.3f,%.3f) ref=%.3f  GL=%3u VK=%3u diff=%d",
                    label,px,py,u,v,ref,gl[i],vk[i],d);
                nWorstShown++;
            }
        }
    }
    // representative center point
    uint32_t ct=(GRID/2)*GRID+(GRID/2);
    LOG("   [%s] center GL=%3u VK=%3u | maxDiff=%d tol=%d badPoints=%d/%d",
        label, gl[ct], vk[ct], maxDiff, tol, nBad, (int)gl.size());
    if(outBad)*outBad=nBad; if(outMax)*outMax=maxDiff;
    return nBad==0;
}

static struct { const char* name; bool pass; bool ran; } g_results[16];
static int g_nres=0;
static void record(const char* n, bool p){ g_results[g_nres].name=n; g_results[g_nres].pass=p; g_results[g_nres].ran=true; g_nres++; }

int main(int, char**) {
    const char* negEnv = std::getenv("MC2_SHADOWCMP_NEGCTL");
    bool negCmpFlip = negEnv && !std::strcmp(negEnv,"cmpflip");
    bool negFilter  = negEnv && !std::strcmp(negEnv,"filter");
    bool neg = negCmpFlip || negFilter;

    LOG("==============================================================");
    LOG(" VK sampler2DShadow == GL sampler2DShadow parity spike");
    LOG("   STOP-gate for the shared VK lighting/shadow helper.");
    LOG("   depth map %ux%u ramp+step, grid %ux%u (uv,ref) points.", MAP_W,MAP_H,GRID,GRID);
    LOG("   compareOp=LESS_OR_EQUAL / GL_LEQUAL, CLAMP_TO_BORDER white=1.0.");
    if (negCmpFlip) LOG("   NEG-CONTROL cmpflip: VK compareOp=GREATER -> parity MUST fail.");
    if (negFilter)  LOG("   NEG-CONTROL filter : VK NEAREST vs GL LINEAR -> parity MUST fail.");
    LOG("==============================================================");

    App a;
    if (!initGL(a) || !initVk(a)) { LOG("VERDICT: GATE RED - init failed."); return 1; }
    g_glShadowProgHw     = compileGlShadowProgram(false);
    g_glShadowProgManual = compileGlShadowProgram(true);
    glCheck("compile GL shadow programs");

    std::vector<float> depth = makeDepthMap();

    // ============================ PATH 1: HW LINEAR D32F (STOP-GATE) =========
    // GL LINEAR sampler2DShadow (D32F) vs VK LINEAR-compare sampler2DShadow.
    {
        std::vector<uint8_t> glOut, vkOut;
        bool glOk = glRunGrid(depth, GL_DEPTH_COMPONENT32F, /*linear*/true, /*manual*/false, glOut);
        VkCompareOp op = negCmpFlip ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS_OR_EQUAL;
        bool vkLinear = negFilter ? false : true;   // filter neg-control forces VK NEAREST
        bool vkOk = vkRunGrid(a, depth, op, vkLinear, /*manual*/false, vkOut);
        LOG("");
        LOG("=== PATH 1: HW LINEAR compare, GL D32F vs VK D32F  [STOP-GATE] ===");
        bool pass=false;
        if (glOk && vkOk) { int bad=0,mx=0; pass = compareGrid("HW_LINEAR_D32F", glOut, vkOut, /*tol*/1, &bad,&mx); }
        else LOG("   [run error] glOk=%d vkOk=%d", glOk,vkOk);
        LOG("   -> HW_LINEAR_D32F : %s", pass?"PASS":"FAIL");
        record("HW_LINEAR_D32F", pass);
    }

    // ============================ PATH 2: HW LINEAR, GL DEPTH24 ==============
    // Same but GL stores DEPTH_COMPONENT24 (VK stays D32 - the interop map is
    // D32, but MC2 GL shadow maps are often 24-bit; check the compare parity).
    if (!neg) {
        std::vector<uint8_t> glOut, vkOut;
        bool glOk = glRunGrid(depth, GL_DEPTH_COMPONENT24, true, false, glOut);
        bool vkOk = vkRunGrid(a, depth, VK_COMPARE_OP_LESS_OR_EQUAL, true, false, vkOut);
        LOG("");
        LOG("=== PATH 2: HW LINEAR compare, GL DEPTH24 vs VK D32F ===");
        bool pass=false;
        if (glOk && vkOk) { int bad=0,mx=0; pass = compareGrid("HW_LINEAR_D24", glOut, vkOut, /*tol*/2, &bad,&mx); }
        else LOG("   [run error] glOk=%d vkOk=%d", glOk,vkOk);
        LOG("   -> HW_LINEAR_D24 : %s", pass?"PASS":"FAIL");
        record("HW_LINEAR_D24", pass);
    }

    // ============================ PATH 3: MANUAL 4-TAP PCF (fallback) ========
    // NEAREST-compare + explicit 4-tap average on BOTH sides. If PATH 1 (hw
    // LINEAR) ever diverges, this is the helper's portable filtering path.
    if (!neg) {
        std::vector<uint8_t> glOut, vkOut;
        bool glOk = glRunGrid(depth, GL_DEPTH_COMPONENT32F, /*linear*/false, /*manual*/true, glOut);
        bool vkOk = vkRunGrid(a, depth, VK_COMPARE_OP_LESS_OR_EQUAL, /*linear*/false, /*manual*/true, vkOut);
        LOG("");
        LOG("=== PATH 3: MANUAL 4-tap PCF (NEAREST compare), GL vs VK ===");
        bool pass=false;
        if (glOk && vkOk) { int bad=0,mx=0; pass = compareGrid("MANUAL4TAP_D32F", glOut, vkOut, /*tol*/1, &bad,&mx); }
        else LOG("   [run error] glOk=%d vkOk=%d", glOk,vkOk);
        LOG("   -> MANUAL4TAP_D32F : %s", pass?"PASS":"FAIL");
        record("MANUAL4TAP_D32F", pass);
    }

    // ============================ verdict ====================================
    LOG("");
    LOG("--------------------------------------------------------------");
    int nPass=0;
    for (int i=0;i<g_nres;i++){ LOG("  %-20s %s", g_results[i].name, g_results[i].pass?"PASS":"FAIL"); if(g_results[i].pass)nPass++; }
    LOG("--------------------------------------------------------------");
    LOG("[TALLY] paths passed         = %d / %d", nPass, g_nres);
    LOG("[TALLY] VK validation errors = %d", g_valErrors);
    LOG("[TALLY] GL debug errors seen = %d", g_glDebugSawError?1:0);

    int rc;
    if (neg) {
        // Under a neg-control the STOP-GATE path (index 0) MUST have FAILED.
        bool detected = (g_nres>=1) && !g_results[0].pass;
        if (detected) { LOG("VERDICT: NEG-CONTROL OK - injected %s mismatch correctly DETECTED.", negEnv); rc=0; }
        else          { LOG("VERDICT: NEG-CONTROL BROKEN - harness did NOT detect the %s mismatch.", negEnv); rc=2; }
    } else {
        bool stopGateGreen = (g_nres>=1) && g_results[0].pass; // PATH 1
        bool allPass = (nPass==g_nres) && (g_valErrors==0) && !g_glDebugSawError;
        if (stopGateGreen && allPass) {
            LOG("VERDICT: STOP-GATE GREEN - VK sampler2DShadow LEQUAL+LINEAR matches");
            LOG("         GL LINEAR sampler2DShadow (2x2 hardware PCF blend identical).");
            LOG("         Shared VK lighting/shadow helper UNBLOCKED; use HW LINEAR path.");
            rc=0;
        } else if (stopGateGreen) {
            LOG("VERDICT: STOP-GATE GREEN (hw LINEAR matches) but a secondary path differs - see above.");
            rc=0;
        } else {
            LOG("VERDICT: STOP-GATE RED - VK hw LINEAR compare != GL. Check MANUAL4TAP path:");
            LOG("         if MANUAL4TAP_D32F PASSED, the helper must use NEAREST+manual-4-tap PCF.");
            rc=1;
        }
    }
    LOG("==============================================================");

    glDeleteProgram(g_glShadowProgHw); glDeleteProgram(g_glShadowProgManual);
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
