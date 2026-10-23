// tools/tgl_loader_standalone_spike/stubs.cpp
//
// Game-free infra stubs for the NS3 TGL loader link-proof spike.
//
// RULE: every symbol here is something the geometry-loader closure
// (mclib msl.cpp / tgl.cpp / file.cpp / heap.cpp / ...) references but which
// normally lives in GameOS gameos.cpp or in code/ (the game). We provide
// minimal no-op / CRT-backed definitions so we never link the engine.
//
// The vast majority are referenced ONLY from TG_Shape / TG_MultiShape
// transform / render / texture-bind methods that the loader NEVER calls on
// the LoadBinaryCopy read path — they are pulled in only because they share a
// TU with the loader. Stubbing them is therefore safe AND is itself the proof
// that the loader does not depend on real game/render state to LOAD geometry.
//
// CODE/ (GAME) SYMBOLS stubbed below (the headline finding):
//   - eye          (extern CameraPtr;  code/gamecam.cpp)
//   - land         (extern TerrainPtr; code/ game terrain singleton)
//   - mc2::textures::ready() (extern legacy texture manager*; game texture singleton)
//   - Camera::HazeFactor / Camera::projectModernClipGL (render-time only)
//   - Terrain::getTerrainElevation (game terrain query; render/transform only)
//   - legacy texture manager::* / MC_TextureNode::* (GPU texture bind; render only)
//   None are on the geometry LOAD path; all are reachable only from
//   transform/render entry points the spike never invokes.

#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <string_view>
#include "texture_assets.h"
#include <cstdlib>
#include <cstdarg>
#include <cstring>

#include "platform_windows.h"
#include "gameos.hpp"

#include "heap.h"
#include "tgl.h"
#include "camera.h"
#include "terrain.h"
#include "render_submissions.h"
#include "object_admission_predicate.h"
#include "gos_object_recon_tracy.h"

// ===========================================================================
// (1) Engine heap. TG shapes allocate with plain new; only the global
//     `systemHeap` needs a CRT-backed stand-in.
// ===========================================================================

void InstallTglHeap() {}

// ===========================================================================
// (2) GameOS globals (normally defined in gameos.cpp / the platform layer).
// ===========================================================================
gosEnvironment Environment;          // global env block; loader reads nothing critical
HGOSHEAP       ParentClientHeap = 0; // default heap handle sentinel

// ===========================================================================
// (3) gos memory — back with CRT malloc/free. Heaps are no-op handles
//     because file.cpp/heap.cpp use the UserHeap (CRT-backed) path, not GOS.
// ===========================================================================
void* __stdcall gos_Malloc(size_t bytes, HGOSHEAP /*Heap*/) { return std::malloc(bytes); }
void  __stdcall gos_Free(void* ptr)                          { std::free(ptr); }
void  __stdcall gos_PushCurrentHeap(HGOSHEAP /*Heap*/)       {}
void  __stdcall gos_PopCurrentHeap()                         {}
// Return a NON-NULL dummy handle: UserHeap::Malloc routes through gos_Malloc
// (CRT) only when its gosHeap member is non-null (USE_GOS_HEAP build). A null
// handle would send it down the compiled-out x86 asm path -> returns NULL ->
// crash. The handle value is never dereferenced by our gos_* stubs.
HGOSHEAP __stdcall gos_CreateMemoryHeap(const char* /*n*/, DWORD /*sz*/, HGOSHEAP /*p*/)
{
    static int dummyHeapStorage = 0;
    return reinterpret_cast<HGOSHEAP>(&dummyHeapStorage);
}
void  __stdcall gos_DestroyMemoryHeap(HGOSHEAP /*Heap*/, bool /*empty*/) {}
void  __stdcall gos_WalkMemoryHeap(HGOSHEAP /*Heap*/, bool /*vociferous*/) {}

// gos placement-new overloads (operator new[](size_t, HGOSHEAP)).
void* operator new[](size_t size, HGOSHEAP /*Heap*/) { return std::malloc(size); }
void* operator new  (size_t size, HGOSHEAP /*Heap*/) { return std::malloc(size); }

// ===========================================================================
// (4) gos file I/O — CRT-backed. NOTE: mclib/file.cpp opens .tgl via the CRT
//     _open path directly, so these gos file fns are referenced only by the
//     ASE / animation paths the spike never exercises. CRT-back them anyway
//     so the link is clean and they'd work if called.
// ===========================================================================
DWORD __stdcall gos_FileSize(const char* fn)
{
    FILE* f = std::fopen(fn, "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fclose(f);
    return (DWORD)(sz < 0 ? 0 : sz);
}
bool  __stdcall gos_DoesFileExist(const char* fn)
{
    FILE* f = std::fopen(fn, "rb");
    if (f) { std::fclose(f); return true; }
    return false;
}
bool  gos_FileExists(const char* fn) { return gos_DoesFileExist(fn); }

void  __stdcall gos_OpenFile(HGOSFILE* hfile, const char* path, gosEnum_FileWriteStatus /*fw*/)
{
    if (hfile) *hfile = (HGOSFILE)std::fopen(path, "rb");
}
void  __stdcall gos_CloseFile(HGOSFILE hfile) { if (hfile) std::fclose((FILE*)hfile); }
DWORD __stdcall gos_ReadFile(HGOSFILE hfile, void* buf, DWORD size)
{
    if (!hfile) return 0;
    return (DWORD)std::fread(buf, 1, size, (FILE*)hfile);
}
DWORD __stdcall gos_WriteFile(HGOSFILE hfile, const void* buf, DWORD size)
{
    if (!hfile) return 0;
    return (DWORD)std::fwrite(buf, 1, size, (FILE*)hfile);
}
void  __stdcall gos_GetFile(const char* /*fn*/, BYTE** /*img*/, SIZE_T* /*sz*/) {}
void* __stdcall gos_OpenMemoryMappedFile(const char* /*fn*/, BYTE** /*img*/, DWORD* /*sz*/) { return nullptr; }
void  __stdcall gos_CloseMemoryMappedFile(void* /*h*/) {}
__int64 __stdcall gos_FileTimeStamp(const char* /*fn*/) { return 0; }
void  __stdcall gos_FileSetReadWrite(const char* /*fn*/) {}
bool  __stdcall gos_CreateDirectory(const char* /*p*/) { return true; }
char* __stdcall gos_FindFiles(const char* /*p*/) { return nullptr; }
char* __stdcall gos_FindFilesNext() { return nullptr; }
void  __stdcall gos_FindFilesClose() {}
char* __stdcall gos_FindDirectories(const char* /*p*/) { return nullptr; }
char* __stdcall gos_FindDirectoriesNext() { return nullptr; }
void  __stdcall gos_FindDirectoriesClose() {}

// ===========================================================================
// (5) gos GPU resource creation — return dummy handles. The loader populates
//     the CPU vertex/triangle arrays BEFORE any GPU upload, so the spike's
//     read-out never needs a real buffer.
// ===========================================================================
HGOSBUFFER __stdcall gos_CreateBuffer(gosBUFFER_TYPE, gosBUFFER_USAGE, int, uint32_t, void*) { return nullptr; }
void __stdcall gos_DestroyBuffer(HGOSBUFFER) {}
HGOSVERTEXDECLARATION __stdcall gos_CreateVertexDeclaration(gosVERTEX_FORMAT_RECORD*, int) { return nullptr; }
void __stdcall gos_DestroyVertexDeclaration(HGOSVERTEXDECLARATION) {}
void __stdcall gos_DrawTriangles(gos_VERTEX*, int) {}
void __stdcall gos_SetRenderState(gos_RenderState, int) {}
void __stdcall gos_GetViewport(float*, float*, float*, float*) {}
int  __stdcall gos_rand() { return std::rand(); }
void __stdcall gos_srand(unsigned int seed) { std::srand(seed); }
__int64 __stdcall gos_GetTimeDate() { return 0; }

// ===========================================================================
// (6) GameOS misc (statistics / debugger / lifecycle) — no-ops.
// ===========================================================================
void AddStatistic(const char*, const char*, gosType, void*, DWORD) {}
void StatisticFormat(const char*) {}
void AddDebuggerMenuItem(const char*, bool(*)(), void(*)(), bool(*)(), DWORD(*)(const char*, DWORD)) {}
void EnterFullScreenMode() {}
void EnterWindowMode() {}
bool gos_NoUserPresentRequested() { return false; }
void ExitGameOS() {}

// STOP/ErrorHandler family (release no-op; would abort in the engine).
int InternalFunctionStop(const char* /*fmt*/, const char* /*arg*/) { return 0; }
int InternalFunctionStop(const char* /*fmt*/, ...) { return 0; }
int InternalFunctionPause(const char* /*fmt*/, ...) { return 0; }

// ===========================================================================
// (7) LZ compress (decomp comes from mclib/lzdecomp.cpp; compress is unused
//     on the read path — provide a no-op so .tgl SAVE references link).
// ===========================================================================
size_t LZCompress(unsigned char* /*dst*/, unsigned char* /*src*/, size_t /*len*/) { return 0; }

// ===========================================================================
// (8) tgl.cpp render-state globals (declared extern in tgl.cpp; the loader
//     never reads them on LoadBinaryCopy). Defaults match the engine's.
// ===========================================================================
bool  useVertexLighting = true;
bool  useFaceLighting   = false;
bool  useFog            = true;
bool  InEditor          = false;
int   ObjectTextureSize = 128;
DWORD BaseVertexColor   = 0xffffffff;

// Missing-asset placeholder strings used by error paths.
char* CDMissingString    = (char*)"";
char* FileMissingString  = (char*)"";
char* MissingTitleString = (char*)"";

// ===========================================================================
// (9) GPU-object / mech feature flags + object-recon counters (render-side
//     toggles read by transform/submit methods, never by the loader).
// ===========================================================================
bool g_useGpuObjects = false;
bool g_useGpuMechs   = false;
bool mc2LightBridgeRepointEnabled() { return false; }

namespace mc2_object_recon {
    bool     g_enabled        = false;
    Counters g_per_frame;
    Counters g_mono;
    uint32_t g_frame_with_data = 0;
    void initFromEnv() {}
    void drainPerFrame(uint32_t) {}
    void drainOnShutdown() {}
}

// ===========================================================================
// (10) GameOS terrain-lighting cost-split shim (gos_terrain_indirect::*) —
//      perf counters read only inside the lit transform kernels. No-op.
// ===========================================================================
namespace gos_terrain_indirect {
    bool IsLightCostSplitEnabled() { return false; }
    void LightCostSplit_AddC2DirectCall() {}
    void LightCostSplit_AddC2DirectCycles(uint64_t) {}
    void LightCostSplit_AddC5PerActorCall() {}
    void LightCostSplit_AddC5PerActorCycles(uint64_t) {}
    void LightCostSplit_AddC6ResubmitCall() {}
    void LightCostSplit_AddC6ResubmitCycles(uint64_t) {}
}

// ===========================================================================
// (11) Lighting / shadow / draw helpers referenced by TG render methods.
// ===========================================================================
void GatherLightsParameters(TG_HWLightsData*) {}
void addShadowShape(HGOSBUFFER, HGOSBUFFER, HGOSVERTEXDECLARATION, const float*) {}

// GPU static-prop eligibility predicate (defined in gos_static_prop_batcher.cpp,
// a render TU we deliberately exclude). Referenced only from the per-leaf
// transform kernel; the loader never reaches it. Stub -> always ineligible.
bool eligibleForGpuObjects(class TG_Shape*) { return false; }

// Crash-bundle instrumentation sink (GameOS gos_crashbundle.cpp). extern "C"
// per gos_crashbundle.h. No-op.
extern "C" void crashbundle_append(const char* /*line*/) {}
void crashbundle_content_fd(int, std::string_view, std::string_view) {}
void crashbundle_content_image(std::string_view, const void*, size_t) {}
void crashbundle_mod(std::string_view, int) {}
void crashbundle_save_point(std::string_view, std::string_view) {}

// ===========================================================================
// (12) CODE/ (GAME) SYMBOLS — the headline finding. All render/transform-only.
// ===========================================================================
CameraPtr  eye  = nullptr;   // code/gamecam.cpp — render camera singleton
TerrainPtr land = nullptr;   // game terrain singleton

// Camera statics/methods (render-time projection; never on load path).
float Camera::HazeFactor = 0.0f;
ModernClipResult Camera::projectModernClipGL(const glm::vec3&) const
{
    ModernClipResult r; std::memset(&r, 0, sizeof(r)); return r;
}

// Terrain elevation query (transform-time only).
float Terrain::getTerrainElevation(const glm::vec3&) { return 0.0f; }

// legacy texture manager / MC_TextureNode (GPU texture bind; render only).

unsigned int mc2::vkLights::addLightDataStructure(TG_HWLightsData*) { return 0; }
unsigned int mc2::vkLights::addLightDataStructureWithPerActorColor(TG_HWLightsData*) { return 0; }
void mc2::RenderSubmissions::addRenderShape(DWORD, TG_RenderShape*, DWORD) {}
// This loader never acquires texture assets: render-only metadata is unavailable.
namespace mc2::textures {
DWORD sourceId(DWORD) { return 0; }
const char* name(DWORD) { return nullptr; }
}

mc2::RenderSubmissions::~RenderSubmissions() = default;
mc2::RenderSubmissions& mc2::renderSubmissions() { static RenderSubmissions queue; return queue; }

namespace mc2::textures { bool ready() { return false; } }
