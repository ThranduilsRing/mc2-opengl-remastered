#include <memory>
#include "gameos.hpp"
#include "toolos.hpp"
#include "memorymanager.hpp" // gos_Heap
#include <stdio.h>
#include <time.h>
#include <stdlib.h> // rand
#include <new>      // std::bad_alloc
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
#include "mc2fmt.h"
#include "utils/mc2log.h"

////////////////////////////////////////////////////////////////////////////////
void __stdcall AddDebuggerMenuItem(char const*, bool (__stdcall *)(), void (__stdcall *)(), bool (__stdcall *)(), DWORD (__stdcall *)(char const*, DWORD))
{
    // TODO: maybe use dconsole for this
}

void __stdcall AddStatistic( const char* Name, const char* TypeName, gosType Type, void* Value, DWORD Flags )
{
}
void __stdcall StatisticFormat(char const* s)
{
}
////////////////////////////////////////////////////////////////////////////////

void* DecodeJPG( const char* FileName, BYTE* Data, DWORD DataSize, DWORD* TextureWidth, DWORD* TextureHeight, bool TextureLoad, void *pDestSurf )
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

static bool g_gos_exit_game_os = false;

// sebi 2026-04-22: when ExitGameOS/gos_TerminateApplication fires, the game
// quietly stops at the top of the next frame with no visible error. This
// shim logs the caller's stack so silent exits are debuggable. Gated on
// env var MC2_TRACE_EXIT so release builds stay quiet by default.
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
static void mc2_log_exit_caller(const char* fn)
{
    static const bool s_trace = mc2::gate::get<"MC2_TRACE_EXIT">();
    static bool s_once = false;
    if (!s_trace && s_once) return;   // always log first; repeats only if env set
    s_once = true;

    mc2::fmt::print_err("[EXIT] {} called. Stack:\n", mc2::fmt::safe(fn));
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    static bool s_sym_initialized = false;
    if (!s_sym_initialized) { SymInitialize(proc, nullptr, TRUE); s_sym_initialized = true; }

    void* frames[32];
    USHORT n = CaptureStackBackTrace(1, 32, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        char symBuf[sizeof(SYMBOL_INFO) + 512];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 512;
        DWORD64 disp64 = 0;
        const char* symName = "?";
        IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD dispL = 0;
        const char* fileName = "?"; DWORD lineNum = 0;
        if (SymFromAddr(proc, (DWORD64)frames[i], &disp64, sym))
            symName = sym->Name;
        if (SymGetLineFromAddr64(proc, (DWORD64)frames[i], &dispL, &line)) {
            fileName = line.FileName; lineNum = line.LineNumber;
        }
        mc2::fmt::print_err("  #{:02d}  {}  ({}:{:d})\n", mc2::fmt::as_i32(i), mc2::fmt::safe(symName), mc2::fmt::safe(fileName), mc2::fmt::as_u32(lineNum));
    }
    fflush(stderr);
}
#else
static void mc2_log_exit_caller(const char*) {}
#endif

void __stdcall ExitGameOS()
{
    mc2_log_exit_caller("ExitGameOS");
    g_gos_exit_game_os = true;
}

bool gosExitGameOS() {
    return g_gos_exit_game_os;
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

static const int g_hepsStackSize = 128;
static gos_Heap* g_heapsStack[g_hepsStackSize];
static int g_heapStackPointer = -1;

////////////////////////////////////////////////////////////////////////////////
HGOSHEAP __stdcall gos_CreateMemoryHeap(char const* HeapName, DWORD MaximumSize/* = 0*/, HGOSHEAP parentHeap/* = ParentClientHeap*/)
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
void __stdcall gos_PushCurrentHeap(HGOSHEAP Heap)
{
    gosASSERT(g_heapStackPointer < g_hepsStackSize);
    g_heapsStack[++g_heapStackPointer] = Heap;
}
void __stdcall gos_PopCurrentHeap()
{
    gosASSERT(g_heapStackPointer >= 0 && g_heapStackPointer < g_hepsStackSize);
    g_heapsStack[g_heapStackPointer--] = nullptr;
}

HGOSHEAP __stdcall gos_GetCurrentHeap()
{
    if(g_heapStackPointer == -1)
        return nullptr;
    return g_heapsStack[g_heapStackPointer];
}

void __stdcall gos_WalkMemoryHeap(HGOSHEAP pHeap, bool vociferous/* = false*/)
{

}

////////////////////////////////////////////////////////////////////////////////
#ifdef MC2_ALLOC_STATS
#include "alloc_stats_proof.inc"
#endif

void* operator new(size_t sz) {
    // A replaced operator new must not return null: callers (and the compiler) assume the result is valid.
    void* p = gos_Malloc(sz ? sz : 1, nullptr);
    if (!p)
        throw std::bad_alloc();
    return p;
}
void operator delete(void* ptr)
#ifndef PLATFORM_WINDOWS
noexcept 
#endif
{
    gos_Free(ptr);
}

void* __cdecl operator new[](size_t size, HGOSHEAP Heap)
{
    return malloc(size);
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
    void* p = malloc(bytes);
    if (p) {
        if (!tl_inTracker) {
            g_allocStatsLive.fetch_add(1, std::memory_order_relaxed);
            g_allocStatsBytes.fetch_add(static_cast<long long>(_msize(p)), std::memory_order_relaxed);
            trackAlloc(p, bytes);
        }
    }
    return p;
#else
    return malloc(bytes);
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
        g_allocStatsBytes.fetch_sub(static_cast<long long>(_msize(ptr)), std::memory_order_relaxed);
        trackFree(ptr);
    }
#endif
    free(ptr);
}
////////////////////////////////////////////////////////////////////////////////

// DETERMINISTIC-RNG-1: seedable per-process LCG behind gos_rand/gos_srand,
// gated by MC2_DETERMINISTIC_RNG (default-OFF). When the gate is OFF every line
// below is the original CRT srand/rand path => byte-identical to stock.
static uint32_t s_lcg_state     = 0u;

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
}
void _stdcall gos_ToggleIME(bool activeInactive)
{
}
void _stdcall gos_GetIMEAppearance(gosIME_Appearance* pia)
{
}
void _stdcall gos_SetIMEAppearance(const gosIME_Appearance* pia)
{
}
void _stdcall gos_PositionIME(DWORD x, DWORD y)
{
}
void _stdcall gos_SetIMELevel(DWORD dwImeLevel)
{
}
void _stdcall gos_FinalizeStringIME()
{
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
    mc2_log_exit_caller("gos_TerminateApplication");
    g_gos_exit_game_os = true;
}

void __stdcall gos_AbortTermination()
{
    g_gos_exit_game_os = false;
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
static const char* g_user_reg_file = ".registry";
static const char* g_user_base_config_dir = ".config";
static const char* g_user_config_dir = ".mechcommander2";

// TODO: use inifile.h functionality for this
void __stdcall gos_LoadDataFromRegistry( const char* keyName, void* pData, DWORD* szData, bool HKLM/*=false*/)
{
    // this code can be useful to get registry file location
    
    const char* home_dir = getenv("HOME");
    if(home_dir == nullptr)
    {
        MC2_LOG(Io, Debug, "gos_Registry: home directory is not set");
        *szData = 0;
        return;
    }

    const int cfg_dir_size = 1024;
    char cfg_dir[cfg_dir_size] = {0};

    const char* config_dir = getenv("XDG_CONFIG_HOME");
    if(config_dir == nullptr) {
        mc2::fmt::to_buf(cfg_dir, "{}/{}", mc2::fmt::safe(home_dir), mc2::fmt::safe(g_user_base_config_dir));
    } else {
        strncpy(cfg_dir, config_dir, 1023);
        cfg_dir[cfg_dir_size-1] = '\0';
    }

    struct stat st;
    if(-1 == stat(cfg_dir, &st)) {
        MC2_LOG(Core, Debug, "stat: {}", mc2::fmt::safe(strerror(errno)));
        *szData = 0;
        return;
    }
    if(!S_ISDIR(st.st_mode)) {
        MC2_LOG(Core, Debug, "gos_LoadDataFromRegistry: {} is not directory", mc2::fmt::safe(cfg_dir));
        *szData = 0;
        return;
    }

    // 1 + 1 =  for \0 and /
    const size_t confDirBytes = strlen(cfg_dir) + strlen(g_user_config_dir) + 1 + 1;
    char* mc2_conf_dir = new char[confDirBytes];
    mc2::fmt::to_buf_n(mc2_conf_dir, confDirBytes, "{}/{}", cfg_dir, g_user_config_dir);

    // if directory does not exist create it
#ifndef PLATFORM_WINDOWS
    int permissions = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
#endif
	if(-1 == stat(mc2_conf_dir, &st)) {
#ifdef PLATFORM_WINDOWS
        if(-1 == _mkdir(mc2_conf_dir)) {
#else
        if(-1 == mkdir(mc2_conf_dir, permissions)) {
#endif
            MC2_LOG(Core, Debug, "stat: {}", mc2::fmt::safe(strerror(errno)));
            *szData = 0;
            delete[] mc2_conf_dir;
            return;
        }
    }

    const size_t regFileBytes = strlen(mc2_conf_dir) + 1 + strlen(g_user_reg_file) + 1;
    char* reg_file = new char[regFileBytes];
    mc2::fmt::to_buf_n(reg_file, regFileBytes, "{}/{}", mc2_conf_dir, g_user_reg_file);

    // check if reg file exists
    if(-1 == stat(reg_file, &st)) {
        *szData = 0;
        return;
    } else {
        *szData = 0;
        // open file and read value
    }
}

void __stdcall gos_SaveDataToRegistry( const char* keyName,  void* pData,  DWORD szData )
{
    // TODO:
}

void __stdcall gos_SaveStringToRegistry( const char* keyName,  char* pData,  DWORD szData )
{
    // TODO;
}

bool __stdcall gos_GetUserDataDirectory(char* user_dir, const int len)
{
	const int cfg_dir_size = 1024;
    char cfg_dir[cfg_dir_size] = {0};
    struct stat st;

#ifdef PLATFORM_WINDOWS
	PWSTR homeDir = nullptr;
	SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &homeDir);
	wcstombs(cfg_dir, homeDir, cfg_dir_size-1);
	cfg_dir[cfg_dir_size-1] = '\0';
	CoTaskMemFree(homeDir);
#else
    const char* home_dir = getenv("HOME");
    if(home_dir == nullptr)
    {
        MC2_LOG(Io, Debug, "gos_Registry: home directory is not set");
        return false;
    }

    const char* config_dir = getenv("XDG_CONFIG_HOME");
    if(config_dir != nullptr) {
        strncpy(cfg_dir, config_dir, cfg_dir_size-1);
        cfg_dir[cfg_dir_size-1] = '\0';

        if(-1 == stat(cfg_dir, &st)) {
            MC2_LOG(Core, Debug, "stat: {}", mc2::fmt::safe(strerror(errno)));
            return false;
        }

        if(!S_ISDIR(st.st_mode)) {
            MC2_LOG(Io, Debug, "gos_GetUserDataDirectory: {} is not directory", mc2::fmt::safe(cfg_dir));
            return false;
        }
    } else {
        strncpy(cfg_dir, home_dir, cfg_dir_size-1);
        cfg_dir[cfg_dir_size-1] = '\0';
    }
#endif

    // 1 + 1 =  for \0 and /
    const size_t confDirBytes = strlen(cfg_dir) + strlen(g_user_config_dir) + 1 + 1;
    auto mc2_conf_dirOwner = std::make_unique_for_overwrite<char[]>(confDirBytes);
    char* mc2_conf_dir = mc2_conf_dirOwner.get();
    mc2::fmt::to_buf_n(mc2_conf_dir, confDirBytes, "{}/{}", cfg_dir, g_user_config_dir);

    // if directory does not exist create it
#ifndef PLATFORM_WINDOWS
	int permissions = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH | S_IXUSR | S_IXGRP;      
#endif

    if(-1 == stat(mc2_conf_dir, &st)) {
#ifdef PLATFORM_WINDOWS
        if(-1 == _mkdir(mc2_conf_dir)) {
#else
        if(-1 == mkdir(mc2_conf_dir, permissions)) {
#endif
            MC2_LOG(Core, Debug, "stat: {}", mc2::fmt::safe(strerror(errno)));
            mc2_conf_dirOwner.reset();
            return false;
        }
    }
    
    size_t conf_len = strlen(mc2_conf_dir);
    conf_len = len - 1 < conf_len ? len - 1 : conf_len;

    strncpy(user_dir, mc2_conf_dir, conf_len);
    user_dir[conf_len] = '\0';
    mc2_conf_dirOwner.reset();
    return true;
}


////////////////////////////////////////////////////////////////////////////////

bool volatile mc2IsInDisplayBackBuffer = false;
bool volatile mc2IsInMouseTimer = false;

////////////////////////////////////////////////////////////////////////////////

HGOSHEAP ParentClientHeap = nullptr;
float ProcessorSpeed = 10595.42f; // sebi: put something cool
float OneOverProcessorSpeed = 1.0f / ProcessorSpeed; // sebi: put something cool
