#include "mc2_czstring.h"
#include <memory>
#include "../../mclib/sim_overlap.h"
#include "gameos.hpp"
#include "toolos.hpp"
#include "memorymanager.hpp" // gos_Heap
#include <stdio.h>
#include <time.h>
#include <stdlib.h> // rand
#include <new>      // std::bad_alloc
#include "mc2_alloc_backend.h" // allocator behind gos_Malloc and the global operator new/delete (mimalloc; CRT under MC2_ASAN)
#include "utils/timing.h"
#include "utils/timing_math.h"

#ifndef PLATFORM_WINDOWS
#include <sys/statvfs.h> // statvfs
#else
#include <Shlobj.h> // SHGetFolderPath
#include <Objbase.h> // CoTaskMemFree
#endif


#include <stdlib.h> // getenv

// fstat
#include <sys/types.h>
#include <sys/stat.h>
#ifndef PLATFORM_WINDOWS
#include <unistd.h>
#else
#include<direct.h>
#define S_ISDIR(x) ((_S_IFDIR & (x))!=0)
#endif

#include "strres.h"

#include "det_rng.h"          // DETERMINISTIC-RNG-1: GL-free LCG kernel
#include "diagnostic_trace.h" // DETERMINISTIC-RNG-1: optional "RNG" JSONL trace
#include "Gates/gate.h"
#include "WindowInput.hpp"
#include "mc2fmt.h"
#include "FileSystem.hpp"
#include "utils/mc2log.h"
#include <tracy/Tracy.hpp>

////////////////////////////////////////////////////////////////////////////////
void __stdcall AddDebuggerMenuItem(char const*, bool (__stdcall *)(), void (__stdcall *)(), bool (__stdcall *)(), DWORD (__stdcall *)(char const*, DWORD))
{
    // TODO: maybe use dconsole for this
}

void __stdcall AddStatistic( mc2::czstring Name, mc2::czstring TypeName, gosType Type, void* Value, DWORD Flags )
{
}
void __stdcall StatisticFormat(mc2::czstring s)
{
}
////////////////////////////////////////////////////////////////////////////////

void* DecodeJPG( mc2::czstring FileName, BYTE* Data, DWORD DataSize, DWORD* TextureWidth, DWORD* TextureHeight, bool TextureLoad, void *pDestSurf )
{
    return nullptr;
}

gosEnvironment Environment;

////////////////////////////////////////////////////////////////////////////////
void EnterFullScreenMode(void)
{
    Environment.fullScreen = true;
}

void EnterWindowMode(void)
{
    Environment.fullScreen = false;
}

// R1-window: the exit flag and the MC2_TRACE_EXIT caller-stack shim live in Platform/WindowInput.cpp.
void __stdcall ExitGameOS()
{
    mc2::platform::RequestExit("ExitGameOS");
}

bool gosExitGameOS() {
    return mc2::platform::ExitRequested();
}

float frameRate = 30.0f; // apparently tiny geometry needs this

__int64 __stdcall GetCycles()
{
#ifdef PLATFORM_WINDOWS
	return rdtsc();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    int64_t v = ts.tv_sec * 1e+9;
    v += ts.tv_nsec;
    return v;
#endif
}

double g_prev_elapsed_time_value = -1;
double __stdcall gos_GetElapsedTime( int RealTime )
{
    (void)RealTime;

#ifdef PLATFORM_WINDOWS
	uint64_t ticks = timing::gettickcount();
	return static_cast<double>(timing::ticks2ms(ticks)) / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    double time_sec = ts.tv_sec;
    // Behavior-preserving delegation to the pure clamp (GAMEOS-TIMING-MATH-HARNESS-1).
    g_prev_elapsed_time_value = timing_math::advance_elapsed_clamp(
        g_prev_elapsed_time_value, time_sec, Environment.MaxTimeDelta);
    return g_prev_elapsed_time_value;
#endif
}

double __stdcall gos_GetHiResTime()
{
#ifdef PLATFORM_LINUX
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    double time_sec = ts.tv_sec + (static_cast<double>(ts.tv_nsec))/10.0e+9;
    return time_sec;
#else
	return (static_cast<double>(timeGetTime())) * 0.001;
#endif
}

////////////////////////////////////////////////////////////////////////////////

HGOSHEAP __stdcall gos_CreateMemoryHeap(mc2::czstring HeapName, DWORD MaximumSize/* = 0*/, HGOSHEAP parentHeap/* = ParentClientHeap*/)
{
    gos_Heap* pheap = new gos_Heap();
    memset(pheap, 0, sizeof(gos_Heap));
    pheap->pParent = parentHeap;
    pheap->Magic = static_cast<DWORD>((reinterpret_cast<size_t>(HeapName) & 0xffffffff));
    if(HeapName) {
        strncpy(pheap->Name, HeapName, sizeof(pheap->Name)-1);
        pheap->Name[sizeof(pheap->Name)-1] = '\0';
    }

#ifdef LAB_ONLY
    pheap->BytesAllocated = 0;
    pheap->MaximumSize = MaximumSize;
#endif

    return pheap;
}
void __stdcall gos_DestroyMemoryHeap(HGOSHEAP Heap, bool shouldBeEmpty/* = true*/)
{
    // shouldBeEmpty ?? should Instances be zero or what?
    delete Heap;
    Heap = nullptr;
}
void __stdcall gos_PushCurrentHeap(HGOSHEAP) {}
void __stdcall gos_PopCurrentHeap() {}
HGOSHEAP __stdcall gos_GetCurrentHeap() { return nullptr; }

void __stdcall gos_WalkMemoryHeap(HGOSHEAP pHeap, bool vociferous/* = false*/)
{

}

////////////////////////////////////////////////////////////////////////////////
#ifdef MC2_ALLOC_STATS
#include "alloc_stats_proof.inc"
#endif

// ALLOCATOR: mimalloc (static, 3rdparty/mimalloc) backs gos_Malloc/gos_Free and every global operator new/delete below, aligned and
// sized forms included (MC2_ASAN builds use the CRT malloc instead, see mc2_alloc_backend.h). CRT malloc/free stay CRT: never free()
// a block from new/gos_Malloc, never delete/gos_Free a malloc'd block; the guard in gos_Free/aligned delete aborts on a mismatch.
#ifdef PLATFORM_WINDOWS
#define MC2_DELETE_NOEXCEPT
#else
#define MC2_DELETE_NOEXCEPT noexcept
#endif

void* operator new(size_t sz) {
    // A replaced operator new must not return null: callers (and the compiler) assume the result is valid.
    void* p = gos_Malloc(sz ? sz : 1, nullptr);
    if (!p)
        throw std::bad_alloc();
    return p;
}
void* operator new[](size_t sz) {
    void* p = gos_Malloc(sz ? sz : 1, nullptr);
    if (!p)
        throw std::bad_alloc();
    return p;
}
void* operator new(size_t sz, const std::nothrow_t&) noexcept { return gos_Malloc(sz ? sz : 1, nullptr); }
void* operator new[](size_t sz, const std::nothrow_t&) noexcept { return gos_Malloc(sz ? sz : 1, nullptr); }
void operator delete(void* ptr) MC2_DELETE_NOEXCEPT { gos_Free(ptr); }
void operator delete[](void* ptr) MC2_DELETE_NOEXCEPT { gos_Free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { gos_Free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { gos_Free(ptr); }
void operator delete(void* ptr, size_t) MC2_DELETE_NOEXCEPT { gos_Free(ptr); }
void operator delete[](void* ptr, size_t) MC2_DELETE_NOEXCEPT { gos_Free(ptr); }

void* operator new(size_t sz, std::align_val_t al) {
    void* p = mc2a_aligned(sz ? sz : 1, static_cast<size_t>(al));
    if (!p)
        throw std::bad_alloc();
    return p;
}
void* operator new[](size_t sz, std::align_val_t al) {
    void* p = mc2a_aligned(sz ? sz : 1, static_cast<size_t>(al));
    if (!p)
        throw std::bad_alloc();
    return p;
}
void* operator new(size_t sz, std::align_val_t al, const std::nothrow_t&) noexcept { return mc2a_aligned(sz ? sz : 1, static_cast<size_t>(al)); }
void* operator new[](size_t sz, std::align_val_t al, const std::nothrow_t&) noexcept { return mc2a_aligned(sz ? sz : 1, static_cast<size_t>(al)); }
void operator delete(void* ptr, std::align_val_t) MC2_DELETE_NOEXCEPT { mc2a_check_owned(ptr, "delete"); mc2a_aligned_free(ptr); }
void operator delete[](void* ptr, std::align_val_t) MC2_DELETE_NOEXCEPT { mc2a_check_owned(ptr, "delete"); mc2a_aligned_free(ptr); }
void operator delete(void* ptr, size_t, std::align_val_t) MC2_DELETE_NOEXCEPT { mc2a_check_owned(ptr, "delete"); mc2a_aligned_free(ptr); }
void operator delete[](void* ptr, size_t, std::align_val_t) MC2_DELETE_NOEXCEPT { mc2a_check_owned(ptr, "delete"); mc2a_aligned_free(ptr); }
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { mc2a_check_owned(ptr, "delete"); mc2a_aligned_free(ptr); }
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { mc2a_check_owned(ptr, "delete"); mc2a_aligned_free(ptr); }

void* __cdecl operator new[](size_t size, HGOSHEAP Heap)
{
    return gos_Malloc(size, Heap);
}

void* __stdcall gos_Malloc(size_t bytes, HGOSHEAP Heap/* = 0*/)
{
    // FIXME: TODO: !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // FIXME: TODO: each malloc should record memory allocation for current heap on a stack
    // FIXME: TODO: so that when Heap destroy() is called all memory will be freed

    /*
    gosHeap* heap = NULL;

    if(Heap) {
        heap = (gosHeap*)Heap;
    } else { 
        heap = gos_GetCurrentHeap();
    }

    if(heap)
        heap->BytesAllocated += bytes;

    if(bytes > 1024*1024)
    {
        PAUSE((""));
    }
    */
#ifdef MC2_ALLOC_STATS
    void* p = mc2a_malloc(bytes);
    if (p) {
        if (!tl_inTracker) {
            g_allocStatsLive.fetch_add(1, std::memory_order_relaxed);
            g_allocStatsBytes.fetch_add(static_cast<long long>(mc2a_usable(p)), std::memory_order_relaxed);
            trackAlloc(p, bytes);
        }
    }
    return p;
#else
    return mc2a_malloc(bytes);
#endif
}
void __stdcall gos_Free(void* ptr)
{
    /*
    gosHeap* heap = (gosHeap*)gos_GetCurrentHeap();
    if(Heap) {
        gosHeap* heap = (gosHeap*)Heap;
        heap->BytesAllocated -= // ??? ;
    }
    */
#ifdef MC2_ALLOC_STATS
    if (ptr && !tl_inTracker) {
        g_allocStatsLive.fetch_sub(1, std::memory_order_relaxed);
        g_allocStatsBytes.fetch_sub(static_cast<long long>(mc2a_usable(ptr)), std::memory_order_relaxed);
        trackFree(ptr);
    }
#endif
    mc2a_check_owned(ptr, "gos_Free");
    mc2a_free(ptr);
}
////////////////////////////////////////////////////////////////////////////////

// DETERMINISTIC-RNG-1: seedable per-process LCG behind gos_rand/gos_srand,
// gated by MC2_DETERMINISTIC_RNG (default ON; =0 is the kill-switch). When the gate is OFF every line
// below is the original CRT srand/rand path => byte-identical to stock.
static uint32_t s_lcg_state     = 0u;
uint32_t mc2GetSimRngState() { return s_lcg_state; }
void mc2RestoreSimRngState(uint32_t state) { s_lcg_state = state; }

// MC2_DETERMINISTIC_RNG=1, read once on first use. A function-local static is initialised under the language's own lock,
// so a gos_rand call on a worker thread cannot see the "checked" latch before the value (the old two-flag latch could).
static inline bool gos_detRngEnabled()
{
    static const bool enabled = mc2::gate::get<"MC2_DETERMINISTIC_RNG">();
    return enabled;
}

void __stdcall gos_srand(unsigned int seed)
{
    if (gos_detRngEnabled()) {
        s_lcg_state = static_cast<uint32_t>(seed);
        return;
    }
    return srand(seed);
}

int __stdcall gos_rand()
{
    mc2::overlap::assertSimQuiescent("rng::sim");
    if (gos_detRngEnabled()) {
        const int result = static_cast<int>(mc2_det_rng::next15(s_lcg_state));
        // Trace is gated twice: compile-time MC2_RNG_TRACE define AND the
        // runtime "RNG" tag whitelist. Default builds pay nothing.
#ifdef MC2_RNG_TRACE
        if (mc2_diag::tagEnabled("RNG")) {
            char buf[48];
            mc2::fmt::to_buf(buf, "{{\"v\":{:d}}}", mc2::fmt::as_i32(result));
            mc2_diag::writeEvent("RNG", 1, 0, buf);
        }
#endif
        return result;
    }
    return rand() % (1<<15);
}
////////////////////////////////////////////////////////////////////////////////
void __stdcall gos_EnableIME(bool enabledDisabled)
{
    mc2::platform::EnableIME(enabledDisabled);
}
void _stdcall gos_ToggleIME(bool activeInactive)
{
    mc2::platform::ToggleIME(activeInactive);
}
void _stdcall gos_GetIMEAppearance(gosIME_Appearance* pia)
{
    mc2::platform::GetIMEAppearance(pia);
}
void _stdcall gos_SetIMEAppearance(const gosIME_Appearance* pia)
{
    mc2::platform::SetIMEAppearance(pia);
}
void _stdcall gos_PositionIME(DWORD x, DWORD y)
{
    mc2::platform::PositionIME(x, y);
}
void _stdcall gos_SetIMELevel(DWORD dwImeLevel)
{
    mc2::platform::SetIMELevel(dwImeLevel);
}
void _stdcall gos_FinalizeStringIME()
{
    mc2::platform::FinalizeStringIME();
}

////////////////////////////////////////////////////////////////////////////////
float __stdcall gosJoystick_GetAxis( DWORD index, GOSJoystickAxis axis )
{
    (void)index; (void)axis;
    gosASSERT(0 && "Not implemented");
    return 0.0f;
}

DWORD __stdcall gosJoystick_CountJoysticks( bool ReDetect )
{
    (void)ReDetect;
    //gosASSERT(0 && "Not implemented");
    return 0;
}

void __stdcall gosJoystick_SetPolling( DWORD index, bool yesNo, float howOften/*=0.03333f*/ )
{
    (void)index, (void)yesNo, (void)howOften;
    gosASSERT(0 && "Not implemented");
}

void __stdcall gosJoystick_GetInfo( DWORD index, gosJoystick_Info* gji )
{
    gosASSERT(gji);
	gji->lpstrName = nullptr;
	gji->bAxisValid = 0;
	gji->nAxes = 0;			
	gji->nButtons = 0;	
	gji->nSliders = 0;
	gji->nPOVs = 0;				
	gji->bIsPolled = 0;		
	gji->bIsForceFeedback = 0;
}

////////////////////////////////////////////////////////////////////////////////
char* __stdcall gos_GetFormattedDate( bool Verbose, WORD Year/*=-1*/, WORD Month/*=-1*/, WORD Day/*=-1*/ )
{
    time_t tim = time(nullptr);
    struct tm* gm_time_data = gmtime(&tim);
    if(Year < 0) {
        Year = 1900 + gm_time_data -> tm_year;
    }
    if(Month < 0) {
        Month = 1 + gm_time_data -> tm_mon;
    }
    if(Day < 0) {
        Day = gm_time_data -> tm_mday;
    }

    if(Year > 9999) {
        Year = 0;
    }

    if(Month > 99) {
        Month = 0;
    }

    if(Day > 99) {
        Day = 0;
    }

    if(!Verbose)
    {
        constexpr size_t kDateBytes = 16;
        char* date = new char[kDateBytes];
        mc2::fmt::to_buf_n(date, kDateBytes, "{:02d}/{:02d}/{:04d}", mc2::fmt::as_i32(Day), mc2::fmt::as_i32(Month), mc2::fmt::as_i32(Year));
        return date;
    }

    gm_time_data->tm_year = Year;
    gm_time_data->tm_mon = Month;
    gm_time_data->tm_mday = Day;

    time_t tt = mktime(gm_time_data);
    char* str_time = ctime(&tt);
    
    char* date = new char[strlen(str_time)+1];
    strcpy(date, str_time);
    return date;

}

////////////////////////////////////////////////////////////////////////////////
char* __stdcall gos_GetFormattedTime( WORD Hour/*=-1*/, WORD Minute/*=-1*/, WORD Second/*=-1*/ )
{
    time_t tim = time(nullptr);
    struct tm* gm_time_data = gmtime(&tim);
    if(Hour < 0) {
        Hour = gm_time_data -> tm_hour;
    }
    if(Minute < 0) {
        Minute = gm_time_data -> tm_min;
    }
    if(Second < 0) {
        Second = gm_time_data -> tm_sec;
    }

    if(Hour > 99) {
       Hour = 0;
    }

    if(Minute > 99) {
        Minute = 0;
    }

    if(Second > 99) {
        Second = 0;
    }

    constexpr size_t kTimeBytes = 16;
    char* date = new char[kTimeBytes];
    mc2::fmt::to_buf_n(date, kTimeBytes, "{:02d}:{:02d}:{:02d}", mc2::fmt::as_i32(Hour), mc2::fmt::as_i32(Minute), mc2::fmt::as_i32(Second));
    return date;
}

////////////////////////////////////////////////////////////////////////////////

void __stdcall gos_TerminateApplication()
{
    mc2::platform::TerminateApplication();
}

////////////////////////////////////////////////////////////////////////////////
__int64 __stdcall gos_GetDriveFreeSpace( char* Path )
{
#ifndef PLATFORM_WINDOWS
    struct statvfs s;
    if(-1 == statvfs(Path, &s)) {
        MC2_LOG(Core, Debug, "statvfs: {}", mc2::fmt::safe(strerror(errno)));
        return 0;
    }

    return s.f_bsize * s.f_bfree;
#else
	// DISK-GUARD-1 (2026-07-29): this passed NULL instead of Path, so it
	// reported free space on the CURRENT drive and silently ignored its own
	// argument. Both callers (mechcmd2.cpp startup check, saveload.cpp
	// Mission::save) pass gos_GetCurrentPath(), so on a single-drive install
	// the answer happened to be right -- but a save-game directory or install
	// on another volume got a free-space figure for the wrong disk, which is
	// exactly how you end up writing a save until ENOSPC truncates it.
	//
	// Also: the return value was unchecked. On failure freeBytesAvailable is
	// left UNINITIALISED and a garbage value was returned -- a large garbage
	// value defeats the caller's "do I have room" test entirely. Return 0 on
	// failure so callers take the conservative branch.
	ULARGE_INTEGER freeBytesAvailable;
	freeBytesAvailable.QuadPart = 0;
	if (!GetDiskFreeSpaceEx(Path, &freeBytesAvailable, nullptr, nullptr))
	{
		// Path may be a file, a relative path, or a UNC root we cannot query.
		// Fall back to the previous behaviour (current drive) rather than
		// hard-failing a startup check that used to succeed -- but say so
		// out loud. NOT SPEW(): SPEW compiles to ((void)0) unless _ARMOR is
		// defined (gameos.hpp:135), so a SPEW here would be invisible in
		// every shipping build, which is the failure mode this lane exists
		// to remove. The fallback answers for the WRONG VOLUME, so the line
		// has to survive to stderr.
		const DWORD err = GetLastError();
		mc2::fmt::print_err("[DISK_GUARD] gos_GetDriveFreeSpace: GetDiskFreeSpaceEx failed for '{}' (err {:d}); falling back to CURRENT DRIVE -- the figure below may describe a different volume\n", mc2::fmt::safe(Path ? Path : "(null)"), mc2::fmt::as_u32(static_cast<unsigned long>(err)));
		fflush(stderr);
		freeBytesAvailable.QuadPart = 0;
		if (!GetDiskFreeSpaceEx(nullptr, &freeBytesAvailable, nullptr, nullptr))
		{
			mc2::fmt::print_err("[DISK_GUARD] gos_GetDriveFreeSpace: current-drive fallback also failed (err {:d}); returning 0 so callers take the out-of-space branch\n", mc2::fmt::as_u32(static_cast<unsigned long>(GetLastError())));
			fflush(stderr);
			return 0;
		}
	}
	return (__int64)freeBytesAvailable.QuadPart;
#endif
}

DWORD __stdcall gos_EnableSetting( gosSetting Setting, DWORD Value )
{
    // stub now implement later
    return 0;
}
////////////////////////////////////////////////////////////////////////////////
// registry read/write
void __stdcall gos_LoadDataFromRegistry( mc2::czstring keyName, void* pData, DWORD* szData, bool HKLM/*=false*/)
{
    std::uint32_t size = 0;
    mc2::platform::LoadDataFromRegistry(keyName, pData, &size, HKLM);
    *szData = size;
}

void __stdcall gos_SaveDataToRegistry( mc2::czstring keyName,  void* pData,  DWORD szData )
{
    mc2::platform::SaveDataToRegistry(keyName, pData, szData);
}

bool __stdcall gos_GetUserDataDirectory(std::span<char> user_dir)
{
    return mc2::platform::GetUserDataDirectory(user_dir);
}


////////////////////////////////////////////////////////////////////////////////

bool volatile mc2IsInDisplayBackBuffer = false;
bool volatile mc2IsInMouseTimer = false;

////////////////////////////////////////////////////////////////////////////////

HGOSHEAP ParentClientHeap = nullptr;
float ProcessorSpeed = 10595.42f; // sebi: put something cool
float OneOverProcessorSpeed = 1.0f / ProcessorSpeed; // sebi: put something cool
