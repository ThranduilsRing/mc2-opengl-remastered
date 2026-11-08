// tools/mc2fx/mc2fx_stubs.cpp
//
// Slim game-free infra stubs for the mc2fx tool. Mirrors the asset_viewer
// engine_stubs.cpp pattern but DROPS all TGL/mesh/camera/terrain symbols —
// mc2fx never links tgl.cpp, so it must not reference TG_Shape::tglHeap etc.
//
// Provides only what the gosFX / MLR spec Load+Save closure pulls:
//   - gos memory heap (CRT-backed; heaps are opaque non-null handles)
//   - gos placement-new overloads
//   - AddStatistic / StatisticFormat (gosfx.cpp InitializeClasses bookkeeping)
//   - STOP / error-handler family (release no-op)
//   - gos_rand / misc
//
// If the link surfaces additional undefined symbols, add minimal no-op
// definitions here (NOT by pulling in real engine TUs).

#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include "texture_assets.h"
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string_view>

#include "platform_windows.h"
#include "gameos.hpp"
#include "toolos.hpp"          // gos_OpenFile / gosEnum_FileWriteStatus / gos file I/O

// Heap installer called once from main() before MLR/gosFX init. With CRT-backed
// gos_Malloc the heaps are pure handles, so this is currently a no-op hook kept
// for future extension (e.g. seeding systemHeap if a pak path is added later).
void InstallMc2fxHeaps() {}

// ---------------------------------------------------------------------------
// gos memory — CRT-backed. Heaps are non-null opaque handles (never deref'd).
// ---------------------------------------------------------------------------
// Track a coherent current-heap so SpecLibrary::Load's
//   Verify(gos_GetCurrentHeap() == gosFX::Heap)
// (and any heap-equality asserts in armored builds) hold. Each
// gos_CreateMemoryHeap hands out a unique non-null handle; push/pop maintain a
// small stack; get returns the top. Allocation itself is CRT-backed regardless.
static HGOSHEAP s_heapStack[64];
static int      s_heapTop = 0;  // 0 => sentinel "no heap pushed"
static HGOSHEAP s_sentinelHeap = reinterpret_cast<HGOSHEAP>(&s_heapTop);

void* __stdcall gos_Malloc(size_t bytes, HGOSHEAP /*Heap*/) { return std::malloc(bytes); }
void  __stdcall gos_Free(void* ptr)                          { std::free(ptr); }
void  __stdcall gos_PushCurrentHeap(HGOSHEAP Heap)
{ if (s_heapTop < 64) s_heapStack[s_heapTop++] = Heap; }
void  __stdcall gos_PopCurrentHeap()
{ if (s_heapTop > 0) --s_heapTop; }
HGOSHEAP __stdcall gos_GetCurrentHeap()
{ return s_heapTop > 0 ? s_heapStack[s_heapTop - 1] : s_sentinelHeap; }
HGOSHEAP __stdcall gos_CreateMemoryHeap(const char* /*n*/, DWORD /*sz*/, HGOSHEAP /*p*/)
{
    // Unique non-null handle per heap so equality checks distinguish heaps.
    static unsigned char heapPool[256];
    static int next = 0;
    return reinterpret_cast<HGOSHEAP>(&heapPool[(next++ * 4) & 0xFF]);
}
void  __stdcall gos_DestroyMemoryHeap(HGOSHEAP /*Heap*/, bool /*empty*/) {}
void  __stdcall gos_WalkMemoryHeap(HGOSHEAP /*Heap*/, bool /*vociferous*/) {}

void* operator new[](size_t size, HGOSHEAP /*Heap*/) { return std::malloc(size); }
void* operator new  (size_t size, HGOSHEAP /*Heap*/) { return std::malloc(size); }

// ---------------------------------------------------------------------------
// GameOS statistics / lifecycle bookkeeping — no-ops.
// ---------------------------------------------------------------------------
void AddStatistic(const char*, const char*, gosType, void*, DWORD) {}
void StatisticFormat(const char*) {}
void ExitGameOS() {}

int  __stdcall gos_rand() { return std::rand(); }
void __stdcall gos_srand(unsigned int seed) { std::srand(seed); }

// STOP / Pause family (release no-op; aborts in the engine).
int InternalFunctionStop(const char* /*fmt*/, const char* /*arg*/) { return 0; }
int InternalFunctionStop(const char* /*fmt*/, ...) { return 0; }
int InternalFunctionPause(const char* /*fmt*/, ...) { return 0; }

// ---------------------------------------------------------------------------
// GameOS globals referenced by Stuff/MLR/gosFX init (never load-critical).
// ---------------------------------------------------------------------------
gosEnvironment Environment;          // global env block
HGOSHEAP       ParentClientHeap = 0; // default heap handle sentinel

void AddDebuggerMenuItem(const char*, bool(*)(), void(*)(), bool(*)(), DWORD(*)(const char*, DWORD)) {}

// ---------------------------------------------------------------------------
// gos file I/O — CRT-backed (Stuff FileStream/Database/Directory closure).
// ---------------------------------------------------------------------------
void  __stdcall gos_OpenFile(HGOSFILE* hfile, const char* path, gosEnum_FileWriteStatus /*fw*/)
{ if (hfile) *hfile = (HGOSFILE)std::fopen(path, "rb"); }
void  __stdcall gos_CloseFile(HGOSFILE hfile) { if (hfile) std::fclose((FILE*)hfile); }
DWORD __stdcall gos_WriteFile(HGOSFILE hfile, const void* buf, DWORD size)
{ return hfile ? (DWORD)std::fwrite(buf, 1, size, (FILE*)hfile) : 0; }
bool  __stdcall gos_DoesFileExist(const char* fn)
{ FILE* f = std::fopen(fn, "rb"); if (f) { std::fclose(f); return true; } return false; }
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
__int64 __stdcall gos_GetTimeDate() { return 0; }
size_t  __stdcall gos_GetMachineInformation(MachineInfo, int, int, int, int) { return 0; }

// ---------------------------------------------------------------------------
// gos GPU draw / texture — never reached on the spec LOAD path (render only).
// ---------------------------------------------------------------------------
void __stdcall gos_DrawTriangles(gos_VERTEX*, int) {}
void __stdcall gos_DrawLines(gos_VERTEX*, int) {}
void __stdcall gos_DrawQuads(gos_VERTEX*, int) {}
void __stdcall gos_RenderIndexedArray(gos_VERTEX*, DWORD, WORD*, DWORD) {}
void __stdcall gos_RenderIndexedArray(gos_VERTEX_2UV*, DWORD, WORD*, DWORD) {}
void __stdcall gos_SetRenderState(gos_RenderState, int) {}
void __stdcall gos_GetViewport(float*, float*, float*, float*) {}
void __stdcall gos_LockTexture(DWORD, DWORD, bool, TEXTUREPTR* out) { if (out) *out = {}; }
void __stdcall gos_UnLockTexture(DWORD) {}

// ---------------------------------------------------------------------------
// Stuff platform string helpers (GameOS/src/platform_str.cpp not linked here).
// ---------------------------------------------------------------------------
int S_stricmp(const char* a, const char* b)  { return _stricmp(a, b); }
int S_strnicmp(const char* a, const char* b, size_t n) { return _strnicmp(a, b, n); }

// ---------------------------------------------------------------------------
// legacy texture manager / MC_TextureNode — GPU texture metadata. The GOSImage /
// MLR state path references these but never touches GL on the LOAD path.
// ---------------------------------------------------------------------------
// Offline GOSImage keeps names with an invalid node when no manager exists.
// The load/save closure never constructs a service, renderer, or game device.
namespace mc2::textures { DWORD load(const char*, gos_TextureFormat, DWORD, DWORD, DWORD) { return 0; } }
namespace mc2::textures { void release(DWORD) {} }
namespace mc2::textures { DWORD sourceId(DWORD) { return 0; } }

// ---------------------------------------------------------------------------
// MLR global limit flag (mlrclipper et al.).
// ---------------------------------------------------------------------------
bool MLRVertexLimitReached = false;

// ---------------------------------------------------------------------------
// fx_trace hooks (mclib/fx_trace not linked; tracing disabled).
// ---------------------------------------------------------------------------
namespace mc2 { namespace fx_trace {
    bool is_enabled() { return false; }
    void record_spawn(const char*) {}
    void record_draw(const char*) {}
    void record_mlr_enqueue(const char*) {}
    void record_class(unsigned int, const char*) {}
}}

// CPU projection cost-split counter (not linked; no-op).
namespace mc2_cpu_proj_cost { void add_workload_mlr_prim_clipped() {} }

// ---------------------------------------------------------------------------
// Particle bridge (render TU excluded). Never reached on the spec LOAD path.
// ---------------------------------------------------------------------------
namespace mc2 { namespace particles {
    struct GpuParticle; struct GroupInfo;
}}
extern "C" void gos_particle_bridge_flush(const mc2::particles::GpuParticle*, unsigned int,
                                          const mc2::particles::GroupInfo*, unsigned int) {}

// ---------------------------------------------------------------------------
// gosFX draw-side bridges (gos_particle_bridge.cpp / gos_vfx_mesh_bridge.cpp are render TUs we exclude).
// Tube::Draw and Shape::Draw reference them; the spec LOAD / dump path never draws.
// ---------------------------------------------------------------------------
struct GosVfxMeshUpload; struct GosVfxMeshInstance;
extern "C" void gos_tube_ribbon_enqueue(const float*, const float*, const float*, unsigned int,
                                        const unsigned short*, unsigned int, unsigned int, int) {}
extern "C" void gos_vfx_mesh_flush(const GosVfxMeshUpload*, unsigned int,
                                   const GosVfxMeshInstance*, unsigned int) {}
extern "C" uint64_t gos_vfx_mesh_generation() { return 1; }
namespace mc2::fx { class RenderResources; struct FramePackets; struct FrameView; }
int gos_fx_native_flush(const mc2::fx::RenderResources&, const mc2::fx::FramePackets&, const mc2::fx::FrameView&) { return 0; }

namespace mc2::textures { bool ready() { return false; } }

// gosfx point/shard clouds guard their [VFX_PRIMITIVE] lines with mc2_diag::tagRequested (diagnostic_trace.cpp is engine-only): no tags in the tool.
namespace mc2_diag { bool tagRequested(std::string_view) { return false; } }
