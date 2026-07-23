// ============================================================================
// vk_gl_buffer_interop_probe — VK-P9a WATER-PERF STEP 0 de-risk gate.
//
// PROVES (or disproves) the ONE unknown that gates the shared-thin-buffer opt:
//
//   A VK-exportable BUFFER (VK_EXTERNAL_MEMORY, dedicated) can be:
//     (1) GL-imported  (glCreateMemoryObjectsEXT + glImportMemoryWin32HandleEXT
//                        + glBufferStorageMemEXT),
//     (2) WRITTEN by GL (a compute/SSBO write into the imported storage),
//     (3) READ by VK   GPU-side (a VK compute reads it, no CPU readback of the
//                        shared buffer), validation-clean,
//   with GL->VK synchronised by an exported GL->VK binary semaphore
//   (glSignalSemaphoreEXT -> VK submit waits it).
//
// This extends the PROVEN VkImageResource/GlInteropView IMAGE interop pattern
// (tools/vk_interop_transport_test, glTextureStorageMem2DEXT) to a BUFFER
// (glBufferStorageMemEXT). The prior STEP-0 note only ruled out importing an
// ALREADY-ALLOCATED GL buffer into VK; this probes the CORRECT direction for the
// opt: VK ALLOCATES the exportable buffer, GL imports+writes it, VK reads it.
//
// Exit 0 = PASS: import ok + GL wrote + VK read back the GL-written pattern
//                byte-exact + validation_errors==0 + no shared-buffer CPU readback.
// Non-zero = FAIL/STOP-0 (the buffer export/import wall) with the failing stage.
//
// Standalone: links only SDL2, GLEW, vulkan-1, opengl32. Headless.
// ============================================================================
#include <windows.h>
#include <GL/glew.h>
#include <GL/wglew.h>
#include <SDL.h>
#include <SDL_syswm.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <vector>
#include <string>

#include "vk_read_comp.spv.h"

static const uint32_t COUNT = 4096;                 // elements (uint)
static const VkDeviceSize BYTES = (VkDeviceSize)COUNT * 4;

static uint32_t expected(uint32_t i) { return i * 7u + 3u; }   // MUST match gl_write.comp

// ---------------------------------------------------------------------------
static FILE* g_logFile = nullptr;
static int   g_valErrors = 0;
static bool  g_glDebugSawError = false;
// Forbidden: any CPU readback of the SHARED buffer would defeat the whole point.
static unsigned g_sharedBufReadbacks = 0;

static void logLine(const char* fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    std::printf("%s\n", buf); std::fflush(stdout);
    if (g_logFile) { std::fprintf(g_logFile, "%s\n", buf); std::fflush(g_logFile); }
}
#define LOG(...) logLine(__VA_ARGS__)

static int g_failStage = 0;
static bool fail(int stage, const char* fmt, ...) {
    g_failStage = stage; char m[512]; va_list ap; va_start(ap, fmt);
    std::vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
    LOG("[FAIL stage %d] %s", stage, m); return false;
}
static const char* vkStr(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
        default: { static char b[32]; std::snprintf(b, 32, "VkResult(%d)", (int)r); return b; }
    }
}
#define VKCHECK(expr, stage, what) do { VkResult _r=(expr); \
    if (_r!=VK_SUCCESS) return fail((stage), "%s -> %s", (what), vkStr(_r)); } while(0)

static VKAPI_ATTR VkBool32 VKAPI_CALL vkDbg(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG("[VK-VALIDATION %s] %s",
            (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" : "WARN",
            d->pMessage ? d->pMessage : "(null)");
        if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) g_valErrors++;
    }
    return VK_FALSE;
}
static void APIENTRY glDbg(GLenum, GLenum type, GLuint, GLenum sev, GLsizei,
                           const GLchar* msg, const void*) {
    if (type == GL_DEBUG_TYPE_ERROR || sev == GL_DEBUG_SEVERITY_HIGH) {
        LOG("[GL-DEBUG ERROR] %s", msg ? msg : "(null)"); g_glDebugSawError = true;
    }
}
static bool glCheck(const char* where) {
    GLenum e; bool bad=false;
    while ((e=glGetError())!=GL_NO_ERROR){ LOG("[GL-ERROR 0x%04X] at %s", e, where); bad=true; }
    return !bad;
}

struct App {
    SDL_Window* win=nullptr; SDL_GLContext glctx=nullptr;
    VkInstance inst=VK_NULL_HANDLE; VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
    VkPhysicalDevice phys=VK_NULL_HANDLE; VkDevice dev=VK_NULL_HANDLE;
    uint32_t qfam=0; VkQueue queue=VK_NULL_HANDLE; VkCommandPool cmdPool=VK_NULL_HANDLE;
    uint8_t devLuid[VK_LUID_SIZE]={0}; bool haveLuid=false;
    PFN_vkGetMemoryWin32HandleKHR    pGetMemWin32=nullptr;
    PFN_vkGetSemaphoreWin32HandleKHR pGetSemWin32=nullptr;
    PFN_vkCreateDebugUtilsMessengerEXT  pCreateDbg=nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT pDestroyDbg=nullptr;
};

static bool findMemType(App& a, uint32_t bits, VkMemoryPropertyFlags want, uint32_t* out) {
    VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(a.phys,&mp);
    for (uint32_t i=0;i<mp.memoryTypeCount;++i)
        if ((bits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&want)==want){*out=i;return true;}
    return false;
}

// =============== Stage 1: GL context + interop exts =========================
static bool initGL(App& a) {
    if (SDL_Init(SDL_INIT_VIDEO)!=0) return fail(1,"SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,SDL_GL_CONTEXT_DEBUG_FLAG);
    a.win=SDL_CreateWindow("vk_gl_buffer_interop_probe",SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED,64,64,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if (!a.win) return fail(1,"SDL_CreateWindow: %s", SDL_GetError());
    a.glctx=SDL_GL_CreateContext(a.win);
    if (!a.glctx) return fail(1,"SDL_GL_CreateContext: %s", SDL_GetError());
    glewExperimental=GL_TRUE;
    GLenum ge=glewInit();
    if (ge!=GLEW_OK) return fail(1,"glewInit: %s",(const char*)glewGetErrorString(ge));
    glGetError();
    LOG("[GL] renderer=%s  version=%s",(const char*)glGetString(GL_RENDERER),
        (const char*)glGetString(GL_VERSION));
    if (GLEW_KHR_debug){ glEnable(GL_DEBUG_OUTPUT); glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
                         glDebugMessageCallback(glDbg,nullptr); }
    if (!GLEW_EXT_memory_object || !GLEW_EXT_memory_object_win32)
        return fail(1,"GL lacks GL_EXT_memory_object[_win32] -> buffer import impossible");
    if (!GLEW_EXT_semaphore || !GLEW_EXT_semaphore_win32)
        return fail(1,"GL lacks GL_EXT_semaphore[_win32] -> cross-API sync impossible");
    return glCheck("initGL");
}

// =============== Stage 2: VK instance + ext-enabled device ==================
static bool initVk(App& a) {
    uint32_t lc=0; vkEnumerateInstanceLayerProperties(&lc,nullptr);
    std::vector<VkLayerProperties> layers(lc); vkEnumerateInstanceLayerProperties(&lc,layers.data());
    bool haveVal=false; for (auto&l:layers) if (!std::strcmp(l.layerName,"VK_LAYER_KHRONOS_validation")) haveVal=true;
    LOG("[VK] validation layer available = %d", haveVal?1:0);
    std::vector<const char*> instExts={
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    std::vector<const char*> instLayers; if (haveVal) instLayers.push_back("VK_LAYER_KHRONOS_validation");
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName="vk_gl_buffer_interop_probe"; ai.apiVersion=VK_API_VERSION_1_2;
    VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dci.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dci.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dci.pfnUserCallback=vkDbg;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext=&dci; ici.pApplicationInfo=&ai;
    ici.enabledExtensionCount=(uint32_t)instExts.size(); ici.ppEnabledExtensionNames=instExts.data();
    ici.enabledLayerCount=(uint32_t)instLayers.size(); ici.ppEnabledLayerNames=instLayers.data();
    VKCHECK(vkCreateInstance(&ici,nullptr,&a.inst),2,"vkCreateInstance");
    a.pCreateDbg=(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(a.inst,"vkCreateDebugUtilsMessengerEXT");
    a.pDestroyDbg=(PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(a.inst,"vkDestroyDebugUtilsMessengerEXT");
    if (a.pCreateDbg) a.pCreateDbg(a.inst,&dci,nullptr,&a.messenger);

    uint32_t pdCount=0; vkEnumeratePhysicalDevices(a.inst,&pdCount,nullptr);
    if (!pdCount) return fail(2,"no Vulkan physical devices");
    std::vector<VkPhysicalDevice> pds(pdCount); vkEnumeratePhysicalDevices(a.inst,&pdCount,pds.data());
    a.phys=pds[0];
    for (auto pd:pds){ VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(pd,&p);
        if (p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) a.phys=pd; }
    { VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(a.phys,&p);
      LOG("[VK] using device: %s", p.deviceName);
      VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; p2.pNext=&idp;
      vkGetPhysicalDeviceProperties2(a.phys,&p2);
      if (idp.deviceLUIDValid){ std::memcpy(a.devLuid,idp.deviceLUID,VK_LUID_SIZE); a.haveLuid=true;
        std::string h; for (int i=0;i<VK_LUID_SIZE;++i){char b[4];std::snprintf(b,4,"%02x",idp.deviceLUID[i]);h+=b;}
        LOG("[VK] device LUID = %s", h.c_str()); } }
    if (a.haveLuid && GLEW_EXT_memory_object_win32) {
        GLubyte glLuid[GL_LUID_SIZE_EXT]={0}; glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT,glLuid);
        if (glCheck("glGetUnsignedBytevEXT(LUID)")) {
            bool match=std::memcmp(glLuid,a.devLuid,VK_LUID_SIZE)==0;
            LOG("[VK] GL<->VK LUID match = %d", match?1:0);
            if (!match && pdCount>1) return fail(2,"GL and VK are different GPUs (LUID mismatch)");
        }
    }
    uint32_t qfc=0; vkGetPhysicalDeviceQueueFamilyProperties(a.phys,&qfc,nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfc); vkGetPhysicalDeviceQueueFamilyProperties(a.phys,&qfc,qfs.data());
    bool found=false; for (uint32_t i=0;i<qfc;++i) if (qfs[i].queueFlags&VK_QUEUE_COMPUTE_BIT){a.qfam=i;found=true;break;}
    if (!found) return fail(2,"no compute queue family");
    std::vector<const char*> devExts={
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME };
    uint32_t dec=0; vkEnumerateDeviceExtensionProperties(a.phys,nullptr,&dec,nullptr);
    std::vector<VkExtensionProperties> des(dec); vkEnumerateDeviceExtensionProperties(a.phys,nullptr,&dec,des.data());
    for (const char* need:devExts){ bool ok=false; for (auto&e:des) if (!std::strcmp(e.extensionName,need)) ok=true;
        if (!ok) return fail(2,"device missing required extension: %s", need); }
    float prio=1.0f; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex=a.qfam; qci.queueCount=1; qci.pQueuePriorities=&prio;
    VkDeviceCreateInfo dvci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dvci.queueCreateInfoCount=1; dvci.pQueueCreateInfos=&qci;
    dvci.enabledExtensionCount=(uint32_t)devExts.size(); dvci.ppEnabledExtensionNames=devExts.data();
    VKCHECK(vkCreateDevice(a.phys,&dvci,nullptr,&a.dev),2,"vkCreateDevice");
    vkGetDeviceQueue(a.dev,a.qfam,0,&a.queue);
    a.pGetMemWin32=(PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(a.dev,"vkGetMemoryWin32HandleKHR");
    a.pGetSemWin32=(PFN_vkGetSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(a.dev,"vkGetSemaphoreWin32HandleKHR");
    if (!a.pGetMemWin32 || !a.pGetSemWin32) return fail(2,"win32 handle entry points null");
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex=a.qfam; cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKCHECK(vkCreateCommandPool(a.dev,&cpi,nullptr,&a.cmdPool),2,"vkCreateCommandPool");
    return true;
}

// Compile a GL compute program from GLSL source.
static GLuint glMakeCompute(const char* src) {
    GLuint sh=glCreateShader(GL_COMPUTE_SHADER); glShaderSource(sh,1,&src,nullptr); glCompileShader(sh);
    GLint ok=0; glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
    if (!ok){ char log[2048]; glGetShaderInfoLog(sh,2048,nullptr,log); LOG("[GL] compute compile fail: %s",log); return 0; }
    GLuint pr=glCreateProgram(); glAttachShader(pr,sh); glLinkProgram(pr);
    glGetProgramiv(pr,GL_LINK_STATUS,&ok);
    if (!ok){ char log[2048]; glGetProgramInfoLog(pr,2048,nullptr,log); LOG("[GL] link fail: %s",log); return 0; }
    glDeleteShader(sh); return pr;
}

static const char* GL_WRITE_SRC =
"#version 430\n"
"layout(local_size_x=64) in;\n"
"layout(std430, binding=0) buffer Shared { uint v[]; } sh;\n"
"uniform uint uCount;\n"
"void main(){ uint i=gl_GlobalInvocationID.x; if(i>=uCount) return; sh.v[i]=i*7u+3u; }\n";

int main(int, char**) {
    g_logFile=std::fopen("vk_gl_buffer_interop_probe-result.log","w");
    App a{};
    if (!initGL(a)) return 10+g_failStage;
    if (!initVk(a)) return 10+g_failStage;

    // -------- Stage 3: VK creates the EXPORTABLE shared buffer ---------------
    VkExternalMemoryBufferCreateInfo embci{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    embci.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext=&embci; bci.size=BYTES;
    bci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer shBuf=VK_NULL_HANDLE;
    VKCHECK(vkCreateBuffer(a.dev,&bci,nullptr,&shBuf),3,"vkCreateBuffer(shared exportable)");
    VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(a.dev,shBuf,&mr);
    uint32_t memIdx;
    if (!findMemType(a,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&memIdx))
        return fail(3,"no DEVICE_LOCAL memtype for shared buffer")?0:13;
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; ded.buffer=shBuf;
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.pNext=&ded; exp.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext=&exp; mai.allocationSize=mr.size; mai.memoryTypeIndex=memIdx;
    VkDeviceMemory shMem=VK_NULL_HANDLE;
    VKCHECK(vkAllocateMemory(a.dev,&mai,nullptr,&shMem),3,"vkAllocateMemory(shared exportable)");
    VKCHECK(vkBindBufferMemory(a.dev,shBuf,shMem,0),3,"vkBindBufferMemory(shared)");
    HANDLE shWin32=nullptr;
    { VkMemoryGetWin32HandleInfoKHR gh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
      gh.memory=shMem; gh.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
      VKCHECK(a.pGetMemWin32(a.dev,&gh,&shWin32),3,"vkGetMemoryWin32HandleKHR(shared)"); }
    LOG("[3] exportable shared buffer created: size=%llu memWin32=%p",
        (unsigned long long)mr.size, shWin32);

    // Exportable GL->VK binary semaphore (GL signals, VK submit waits).
    VkSemaphore semGlToVk=VK_NULL_HANDLE; HANDLE semWin32=nullptr;
    { VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
      esci.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
      VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sci.pNext=&esci;
      VKCHECK(vkCreateSemaphore(a.dev,&sci,nullptr,&semGlToVk),3,"vkCreateSemaphore(GL->VK)");
      VkSemaphoreGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
      gi.semaphore=semGlToVk; gi.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
      VKCHECK(a.pGetSemWin32(a.dev,&gi,&semWin32),3,"vkGetSemaphoreWin32HandleKHR(GL->VK)"); }

    // -------- Stage 4: GL imports the buffer (glBufferStorageMemEXT) --------
    GLuint glMemObj=0; glCreateMemoryObjectsEXT(1,&glMemObj);
    GLint dedicated=GL_TRUE; glMemoryObjectParameterivEXT(glMemObj,GL_DEDICATED_MEMORY_OBJECT_EXT,&dedicated);
    glImportMemoryWin32HandleEXT(glMemObj,mr.size,GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,shWin32);
    if (!glCheck("glImportMemoryWin32HandleEXT")) return 14;
    GLuint glBuf=0; glCreateBuffers(1,&glBuf);
    glNamedBufferStorageMemEXT(glBuf,(GLsizeiptr)mr.size,glMemObj,0);
    if (!glCheck("glNamedBufferStorageMemEXT")) { LOG("[STOP-0] GL cannot back a buffer with imported VK memory"); return 14; }
    LOG("[4] GL imported the exportable VK buffer via glNamedBufferStorageMemEXT (glBuf=%u)", glBuf);

    // Import the GL->VK semaphore into GL.
    GLuint glSem=0; glGenSemaphoresEXT(1,&glSem);
    glImportSemaphoreWin32HandleEXT(glSem,GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,semWin32);
    if (!glCheck("glImportSemaphoreWin32HandleEXT")) return 14;

    // -------- Stage 5: GL compute WRITES into the shared buffer -------------
    GLuint glProg=glMakeCompute(GL_WRITE_SRC);
    if (!glProg) return 15;
    glUseProgram(glProg);
    glUniform1ui(glGetUniformLocation(glProg,"uCount"), COUNT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,glBuf);
    glDispatchCompute((COUNT+63)/64,1,1);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    if (!glCheck("GL compute write")) return 15;
    // GL signals the semaphore, handing the buffer to VK (GPU-side; the buffer
    // is listed so its writes are made available to the VK consumer).
    GLenum srcLayout = GL_NONE; // buffer has no layout
    glSignalSemaphoreEXT(glSem, 1, &glBuf, 0, nullptr, &srcLayout);
    glFlush();
    if (!glCheck("glSignalSemaphoreEXT")) return 15;
    LOG("[5] GL compute wrote %u elements + signalled GL->VK semaphore", COUNT);

    // -------- Stage 6: VK plain readback buffer (host-visible) --------------
    VkBuffer rbBuf=VK_NULL_HANDLE; VkDeviceMemory rbMem=VK_NULL_HANDLE;
    { VkBufferCreateInfo r{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      r.size=BYTES; r.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; r.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
      VKCHECK(vkCreateBuffer(a.dev,&r,nullptr,&rbBuf),6,"vkCreateBuffer(readback)");
      VkMemoryRequirements rr{}; vkGetBufferMemoryRequirements(a.dev,rbBuf,&rr);
      uint32_t idx; if (!findMemType(a,rr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,&idx))
          return fail(6,"no host-visible memtype")?0:16;
      VkMemoryAllocateInfo m{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; m.allocationSize=rr.size; m.memoryTypeIndex=idx;
      VKCHECK(vkAllocateMemory(a.dev,&m,nullptr,&rbMem),6,"vkAllocateMemory(readback)");
      VKCHECK(vkBindBufferMemory(a.dev,rbBuf,rbMem,0),6,"vkBindBufferMemory(readback)"); }

    // VK compute pipeline: reads shared (binding0) -> writes readback (binding1).
    VkDescriptorSetLayoutBinding lb[2]{};
    lb[0].binding=0; lb[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; lb[0].descriptorCount=1; lb[0].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    lb[1].binding=1; lb[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; lb[1].descriptorCount=1; lb[1].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dslc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dslc.bindingCount=2; dslc.pBindings=lb;
    VkDescriptorSetLayout dsl; VKCHECK(vkCreateDescriptorSetLayout(a.dev,&dslc,nullptr,&dsl),6,"dsl");
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
    VkPipelineLayoutCreateInfo plc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plc.setLayoutCount=1; plc.pSetLayouts=&dsl; plc.pushConstantRangeCount=1; plc.pPushConstantRanges=&pcr;
    VkPipelineLayout playout; VKCHECK(vkCreatePipelineLayout(a.dev,&plc,nullptr,&playout),6,"playout");
    VkShaderModuleCreateInfo smc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smc.codeSize=sizeof(vk_read_comp_spv); smc.pCode=vk_read_comp_spv;
    VkShaderModule sm; VKCHECK(vkCreateShaderModule(a.dev,&smc,nullptr,&sm),6,"shadermodule");
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module=sm; cpci.stage.pName="main";
    cpci.layout=playout;
    VkPipeline pipe; VKCHECK(vkCreateComputePipelines(a.dev,VK_NULL_HANDLE,1,&cpci,nullptr,&pipe),6,"pipe");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
    VkDescriptorPoolCreateInfo dpc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpc.maxSets=1; dpc.poolSizeCount=1; dpc.pPoolSizes=&ps;
    VkDescriptorPool dpool; VKCHECK(vkCreateDescriptorPool(a.dev,&dpc,nullptr,&dpool),6,"dpool");
    VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dsa.descriptorPool=dpool; dsa.descriptorSetCount=1; dsa.pSetLayouts=&dsl;
    VkDescriptorSet dset; VKCHECK(vkAllocateDescriptorSets(a.dev,&dsa,&dset),6,"dset");
    VkDescriptorBufferInfo b0{shBuf,0,BYTES}, b1{rbBuf,0,BYTES};
    VkWriteDescriptorSet w[2]{};
    w[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet=dset; w[0].dstBinding=0; w[0].descriptorCount=1; w[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo=&b0;
    w[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet=dset; w[1].dstBinding=1; w[1].descriptorCount=1; w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo=&b1;
    vkUpdateDescriptorSets(a.dev,2,w,0,nullptr);

    // Record: VK compute reads shared -> writes readback.
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool=a.cmdPool; cbai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount=1;
    VkCommandBuffer cb; VKCHECK(vkAllocateCommandBuffers(a.dev,&cbai,&cb),6,"alloc cb");
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cbbi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(cb,&cbbi),6,"begin cb");
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,playout,0,1,&dset,0,nullptr);
    vkCmdPushConstants(cb,playout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&COUNT);
    vkCmdDispatch(cb,(COUNT+63)/64,1,1);
    VKCHECK(vkEndCommandBuffer(cb),6,"end cb");

    // Submit: WAIT the GL->VK semaphore (GL's compute write must be visible first).
    VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount=1; si.pWaitSemaphores=&semGlToVk; si.pWaitDstStageMask=&waitStage;
    si.commandBufferCount=1; si.pCommandBuffers=&cb;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; vkCreateFence(a.dev,&fci,nullptr,&fence);
    VKCHECK(vkQueueSubmit(a.queue,1,&si,fence),6,"vkQueueSubmit(VK read)");
    VKCHECK(vkWaitForFences(a.dev,1,&fence,VK_TRUE,5ull*1000*1000*1000),6,"vkWaitForFences");
    LOG("[6] VK compute read shared buffer GPU-side -> readback buffer (fence retired)");

    // -------- Stage 7: verify (readback buffer only; shared buffer NEVER
    //          CPU-read on either API) ----------------------------------------
    void* mp=nullptr; VKCHECK(vkMapMemory(a.dev,rbMem,0,BYTES,0,&mp),7,"map readback");
    const uint32_t* got=(const uint32_t*)mp;
    uint32_t mism=0, firstBad=0xffffffffu;
    for (uint32_t i=0;i<COUNT;++i) if (got[i]!=expected(i)){ if(mism==0) firstBad=i; ++mism; }
    uint32_t sample0=got[0], sampleMid=got[COUNT/2], sampleN=got[COUNT-1];
    vkUnmapMemory(a.dev,rbMem);
    LOG("[7] verify: mismatches=%u  sample[0]=%u(exp %u) sample[mid]=%u(exp %u) sample[N-1]=%u(exp %u)",
        mism, sample0, expected(0), sampleMid, expected(COUNT/2), sampleN, expected(COUNT-1));

    // Result.
    const bool pass = (mism==0) && (g_valErrors==0) && (!g_glDebugSawError) && (g_sharedBufReadbacks==0);
    LOG("=====================================================================");
    LOG("STEP-0 BUFFER INTEROP PROBE: %s", pass ? "PASS" : "FAIL");
    LOG("  import(glNamedBufferStorageMemEXT)=OK  gl_wrote=OK  vk_read_gpu_side=%s",
        mism==0?"OK":"MISMATCH");
    LOG("  validation_errors=%d  gl_debug_errors=%d  shared_buffer_cpu_readbacks=%u  first_bad_index=%d",
        g_valErrors, g_glDebugSawError?1:0, g_sharedBufReadbacks, (int)firstBad);
    LOG("=====================================================================");
    if (g_logFile) std::fclose(g_logFile);
    return pass ? 0 : 20;
}
