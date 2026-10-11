"""Apply the TEMPORARY free-quarantine diagnostic to GameOS/gameos/gameos.cpp (never committed)."""
import sys
p = 'A:/Games/mc2-lane3/GameOS/gameos/gameos.cpp'
s = open(p, encoding='utf-8', newline='').read()
nl = '\r\n' if '\r\n' in s else '\n'

diag = r'''
// ===== TEMPORARY DIAGNOSTIC (never committed): free-quarantine to catch writes into freed chunks =====
#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <dbghelp.h>
#include <malloc.h>
#include <mutex>
#include <thread>
namespace diagq {
constexpr size_t kMinSz = 8, kMaxSz = 65536;
constexpr size_t kQCap = 1u << 17;            // entries
constexpr size_t kQBytes = 32u << 20; constexpr size_t kScanEvery = 4000;        // quarantine byte cap
constexpr size_t kAllocTab = 1u << 21;        // lossy direct-mapped alloc-stack table
struct AllocRec { void* p; size_t req; void* st[10]; USHORT n; };
struct QEnt { void* p; size_t sz; AllocRec a; void* fst[10]; USHORT fn; };
thread_local bool t_in = false;
std::mutex g_mu;
AllocRec g_alloc[kAllocTab];
QEnt g_q[kQCap];
size_t g_qHead = 0, g_qCount = 0, g_qBytes = 0, g_hits = 0, g_frees = 0;
bool g_symInit = false;
extern uintptr_t g_updLo, g_updHi;
void armWatch(void* addr);

inline size_t hashp(void* p) { return (size_t)(((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull >> 20) & (kAllocTab - 1); }

void sym(void* a, char* out, size_t cap) {
    HANDLE proc = GetCurrentProcess();
    if (!g_symInit) { SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME); SymInitialize(proc, nullptr, TRUE); g_symInit = true; }
    char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO* si = (SYMBOL_INFO*)buf; si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
    DWORD64 d = 0; IMAGEHLP_LINE64 ln; ln.SizeOfStruct = sizeof(ln); DWORD ld = 0;
    if (SymFromAddr(proc, (DWORD64)a, &d, si)) {
        if (SymGetLineFromAddr64(proc, (DWORD64)a, &ld, &ln)) _snprintf(out, cap, "%s (%s:%lu)", si->Name, ln.FileName, ln.LineNumber);
        else _snprintf(out, cap, "%s+0x%llx", si->Name, (unsigned long long)d);
    } else _snprintf(out, cap, "%p", a);
}
void printStack(const char* tag, void* const* st, USHORT n) {
    char line[600];
    for (USHORT i = 0; i < n; ++i) { sym(st[i], line, sizeof line); fprintf(stderr, "  %s #%u %s\n", tag, (unsigned)i, line); }
}
void verify(QEnt& e) {
    const unsigned char* b = (const unsigned char*)e.p;
    size_t first = (size_t)-1, last = 0, bad = 0;
    for (size_t i = 0; i < e.sz; ++i) if (b[i] != 0xDD) { if (first == (size_t)-1) first = i; last = i; ++bad; }
    if (bad) {
        ++g_hits;
        fprintf(stderr, "[QUARANTINE_HIT] #%zu ptr=%p size=%zu modified_bytes=%zu first=%zu last=%zu bytes@first=", g_hits, e.p, e.sz, bad, first, last);
        for (size_t i = first; i < e.sz && i < first + 16; ++i) fprintf(stderr, "%02X ", b[i]);
        fprintf(stderr, "\n");
        {   // text context: 256 bytes before the chunk and 32 after
            const unsigned char* c = b - 256;
            for (int row = 0; row < 5; ++row) {
                char ln[80]; int n = 0;
                for (int i = 0; i < 64 && row * 64 + i < 288; ++i) { unsigned char ch = c[row * 64 + i]; ln[n++] = (ch >= 32 && ch < 127) ? (char)ch : '.'; }
                ln[n] = 0;
                fprintf(stderr, "  QCTX%d off=%d |%s|\n", row, row * 64 - 256, ln);
            }
        }
        if (e.a.p == e.p) printStack("alloc", e.a.st, e.a.n); else fprintf(stderr, "  alloc stack unknown\n");
        printStack("free", e.fst, e.fn);
        fflush(stderr);
        memset(e.p, 0xDD, e.sz);   // report each corrupted chunk once
    }
}
void initWatch();
void ensureSym() {
    if (g_symInit || t_in) return;
    t_in = true; char tmp[64]; sym((void*)&ensureSym, tmp, sizeof tmp); initWatch(); t_in = false;
}
void onAlloc(void* p, size_t req) {
    if (!p || t_in) return;
    ensureSym();
    AllocRec& r = g_alloc[hashp(p)];
    r.p = p; r.req = req; r.n = RtlCaptureStackBackTrace(2, 10, r.st, nullptr);
    // canary in the last 8 bytes of the usable block (the request was enlarged by 8)
}
void checkCanary(void* p, size_t us) {
    AllocRec& r = g_alloc[hashp(p)];
    if (r.p != p || us < r.req + 8) return;
    const unsigned char* c = (const unsigned char*)p + us - 8;
    for (int i = 0; i < 8; ++i) if (c[i] != 0xCA) {
        fprintf(stderr, "[CANARY_HIT] ptr=%p req=%zu usable=%zu tail bytes=", p, r.req, us);
        for (int k = 0; k < 8; ++k) fprintf(stderr, "%02X ", c[k]);
        fprintf(stderr, " text-before-tail=");
        const unsigned char* t = (const unsigned char*)p + (r.req > 96 ? r.req - 96 : 0);
        for (size_t k = 0; k < us + 8 - (t - (const unsigned char*)p); ++k) { unsigned char ch = t[k]; fputc((ch >= 32 && ch < 127) ? ch : '.', stderr); }
        fprintf(stderr, "\n");
        printStack("owner-alloc", r.st, r.n);
        fflush(stderr);
        break;
    }
}
void scanAll() {
    for (size_t i = 0; i < g_qCount; ++i) verify(g_q[(g_qHead + i) % kQCap]);
}
bool onFree(void* p) {   // true = quarantined (do not free now)
    if (!p || t_in) return false;
    size_t sz = _msize(p);
    if (sz < kMinSz || sz > kMaxSz) return false;
    t_in = true;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_frees;
        if (g_frees % kScanEvery == 0) scanAll();
        memset(p, 0xDD, sz);
        while (g_qCount > 0 && (g_qCount == kQCap || g_qBytes + sz > kQBytes)) {
            QEnt& old = g_q[g_qHead];
            verify(old);
            g_qBytes -= old.sz; free(old.p);
            g_qHead = (g_qHead + 1) % kQCap; --g_qCount;
        }
        QEnt& e = g_q[(g_qHead + g_qCount) % kQCap];
        e.p = p; e.sz = sz;
        AllocRec& r = g_alloc[hashp(p)];
        if (r.p == p) e.a = r; else e.a.p = nullptr;
        e.fn = RtlCaptureStackBackTrace(2, 10, e.fst, nullptr);
        ++g_qCount; g_qBytes += sz;
        if (sz == 56 && g_updHi && e.fn > 1 && (uintptr_t)e.fst[1] >= g_updLo && (uintptr_t)e.fst[1] < g_updHi) armWatch(p);
    }
    t_in = false;
    return true;
}

// ---- page-guard allocator for user sizes 49..64 (the 80-byte NT-heap bucket where the corruption lands) ----
constexpr size_t kGuardLo = 1, kGuardHi = 0, kPage = 4096, kGuardPages = 8u << 20;   // 32 GB of address space
char* g_gbase = nullptr; size_t g_gnext = 0; std::mutex g_gmu; size_t g_gcount = 0;
inline bool isGuard(void* p) { return g_gbase && (char*)p >= g_gbase && (char*)p < g_gbase + kGuardPages * kPage; }
void* guardAlloc(size_t n) {
    std::lock_guard<std::mutex> lk(g_gmu);
    if (!g_gbase) g_gbase = (char*)VirtualAlloc(nullptr, kGuardPages * kPage, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_gbase) { static bool w = false; if (!w) { w = true; fprintf(stderr, "[GUARD] reserve failed err=%lu\n", GetLastError()); } return nullptr; }
    size_t idx = (g_gnext++ % (kGuardPages / 2)) * 2;     // data page + never-committed guard page
    char* pg = g_gbase + idx * kPage;
    void* r = VirtualAlloc(pg, kPage, MEM_COMMIT, PAGE_READWRITE);
    if (!r) { static int w = 0; if (w++ < 3) fprintf(stderr, "[GUARD] commit failed err=%lu count=%zu\n", GetLastError(), g_gcount); return nullptr; }
    ++g_gcount;
    size_t asz = (n + 15) & ~(size_t)15; if (asz == 0) asz = 16;
    return pg + kPage - asz;                              // end-aligned: an overflow runs into the guard page
}
void guardFree(void* p) { VirtualFree((void*)((uintptr_t)p & ~(uintptr_t)(kPage - 1)), kPage, MEM_DECOMMIT); }

// ---- hardware write watchpoints on the freed 56-byte chunk of updateTerrainObjects (4 rotating slots) ----
#include <tlhelp32.h>
uintptr_t g_updLo = 0, g_updHi = 0;
void* g_watch[4] = {0, 0, 0, 0};
int g_wslot = 0;
volatile LONG g_hwHits = 0;
void armSlotAllThreads(int slot, void* addr) {
    DWORD me = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!th) continue;
        bool self = (te.th32ThreadID == me);
        if (!self) SuspendThread(th);
        CONTEXT c; ZeroMemory(&c, sizeof c); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &c)) {
            (&c.Dr0)[slot] = (DWORD64)addr;
            c.Dr7 &= ~((DWORD64)0xF << (16 + slot * 4));
            c.Dr7 |= ((DWORD64)1 << (slot * 2)) | ((DWORD64)0x1 << (16 + slot * 4)) | ((DWORD64)0x2 << (18 + slot * 4));   // enable, write, 8 bytes
            SetThreadContext(th, &c);
        }
        if (!self) ResumeThread(th);
        CloseHandle(th);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}
struct WEnt { volatile void* p; DWORD tid; };
WEnt g_wring[32]; volatile LONG g_wcount = 0; HANDLE g_wthread = nullptr; volatile LONG g_wfound = 0;
void walkThread(DWORD tid, int maxFrames, const char* tag) {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
    if (!th) return;
    SuspendThread(th);
    CONTEXT ctx; ZeroMemory(&ctx, sizeof ctx); ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(th, &ctx)) {
        HANDLE proc = GetCurrentProcess();
        STACKFRAME64 f; ZeroMemory(&f, sizeof f);
        f.AddrPC.Offset = ctx.Rip; f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < maxFrames; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &f, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
            if (!f.AddrPC.Offset) break;
            char line[600]; sym((void*)f.AddrPC.Offset, line, sizeof line);
            fprintf(stderr, "  %s tid=%lu #%d %s\n", tag, tid, i, line);
        }
    }
    ResumeThread(th);
    CloseHandle(th);
}
DWORD WINAPI watcherMain(LPVOID) {
    t_in = true;
    for (;;) {
        LONG n = g_wcount; if (n > 32) n = 32;
        for (LONG i = 0; i < n; ++i) {
            const unsigned char* b = (const unsigned char*)g_wring[i].p;
            if (!b) continue;
            bool changed = false; for (int k = 0; k < 16; ++k) if (b[k] != 0xDD) { changed = true; break; }
            if (changed && InterlockedIncrement(&g_wfound) <= 6) {
                unsigned char snap[16]; memcpy(snap, b, 16);
                fprintf(stderr, "[POLLWATCH_HIT] chunk=%p bytes=", (const void*)b);
                for (int k = 0; k < 16; ++k) fprintf(stderr, "%02X ", snap[k]);
                fprintf(stderr, " text=");
                for (int k = 0; k < 16; ++k) fputc((snap[k] >= 32 && snap[k] < 127) ? snap[k] : '.', stderr);
                fprintf(stderr, "\n");
                // stacks of every thread of the process, main thread (the owner of the chunk) first
                walkThread(g_wring[i].tid, 24, "owner");
                HANDLE snapT = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 te; te.dwSize = sizeof te;
                if (snapT != INVALID_HANDLE_VALUE) {
                    if (Thread32First(snapT, &te)) do {
                        if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != g_wring[i].tid && te.th32ThreadID != GetCurrentThreadId()) walkThread(te.th32ThreadID, 6, "other");
                    } while (Thread32Next(snapT, &te));
                    CloseHandle(snapT);
                }
                fflush(stderr);
            }
            if (changed) g_wring[i].p = nullptr;
        }
        YieldProcessor();
    }
    return 0;
}
void armWatch(void* addr) {
    if (!g_wthread) g_wthread = CreateThread(nullptr, 0, watcherMain, nullptr, 0, nullptr);
    LONG i = InterlockedIncrement(&g_wcount) - 1;
    if (i >= 32) { InterlockedExchange(&g_wcount, 1); i = 0; }
    g_wring[i].tid = GetCurrentThreadId();
    g_wring[i].p = addr;
}
LONG CALLBACK hwVeh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    DWORD64 dr6 = ep->ContextRecord->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    int slot = 0; while (!((dr6 >> slot) & 1)) ++slot;
    t_in = true;
    LONG n = InterlockedIncrement(&g_hwHits);
    if (n <= 12) {
        fprintf(stderr, "[HWWATCH_HIT] #%ld tid=%lu slot=%d addr=%p bytes=", n, GetCurrentThreadId(), slot, g_watch[slot]);
        const unsigned char* b = (const unsigned char*)g_watch[slot];
        for (int i = 0; i < 24; ++i) fprintf(stderr, "%02X ", b[i]);
        fprintf(stderr, " text=");
        for (int i = 0; i < 24; ++i) fputc((b[i] >= 32 && b[i] < 127) ? b[i] : '.', stderr);
        fprintf(stderr, "\n");
        HANDLE proc = GetCurrentProcess();
        CONTEXT ctx = *ep->ContextRecord;
        STACKFRAME64 f; ZeroMemory(&f, sizeof f);
        f.AddrPC.Offset = ctx.Rip; f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 20; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &f, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
            if (!f.AddrPC.Offset) break;
            char line[600]; sym((void*)f.AddrPC.Offset, line, sizeof line);
            fprintf(stderr, "  hw #%d %p %s\n", i, (void*)f.AddrPC.Offset, line);
        }
        fflush(stderr);
    }
    t_in = false;
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
void initWatch() {
    static bool done = false; if (done) return; done = true;
    AddVectoredExceptionHandler(1, hwVeh);
    char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO* si = (SYMBOL_INFO*)buf; si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
    if (SymFromName(GetCurrentProcess(), "GameObjectManager::updateTerrainObjects", si)) { g_updLo = (uintptr_t)si->Address; g_updHi = g_updLo + si->Size; }
    fprintf(stderr, "[HWWATCH] updateTerrainObjects range %p..%p\n", (void*)g_updLo, (void*)g_updHi); fflush(stderr);
}
}  // namespace diagq
#endif
// ===== END TEMPORARY DIAGNOSTIC =====
'''.replace('\n', nl)

anchor = '////////////////////////////////////////////////////////////////////////////////' + nl + 'void* operator new(size_t sz) {'
assert s.count(anchor) == 1
s = s.replace(anchor, diag + nl + anchor)

old_m = '    return malloc(bytes);' + nl + '}' + nl + 'void __stdcall gos_Free(void* ptr)'
assert s.count(old_m) == 1
s = s.replace(old_m, '    if (bytes >= diagq::kGuardLo && bytes <= diagq::kGuardHi) { void* g_ = diagq::guardAlloc(bytes); if (g_) return g_; }' + nl + '    void* r_ = malloc(bytes);' + nl + '#ifdef PLATFORM_WINDOWS' + nl + '    diagq::onAlloc(r_, bytes);' + nl + '#endif' + nl + '    return r_;' + nl + '}' + nl + 'void __stdcall gos_Free(void* ptr)')

old_f = '    free(ptr);' + nl + '}' + nl + '////////////////////////////////////////////////////////////////////////////////' + nl + nl + '// DETERMINISTIC-RNG-1'
assert s.count(old_f) == 1, s.count(old_f)
s = s.replace(old_f, '#ifdef PLATFORM_WINDOWS' + nl + '    if (diagq::isGuard(ptr)) { diagq::guardFree(ptr); return; }' + nl + '    if (diagq::onFree(ptr)) return;' + nl + '#endif' + nl + '    free(ptr);' + nl + '}' + nl + '////////////////////////////////////////////////////////////////////////////////' + nl + nl + '// DETERMINISTIC-RNG-1')
open(p, 'w', encoding='utf-8', newline='').write(s)
print("patched")
