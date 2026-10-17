// Proof build only (cmake -DMC2_ALLOC_STATS=ON); included from gameos.cpp under that define, nothing in a default build sees it.
//
// WHAT: live-block counters behind gos_Malloc / gos_Free, and a site tracker that remembers the allocation stack and the load generation of
// every live block.
// WHY: the ownership step removed the bulk-free heaps, so a leak they hid must show as growth per mission reload. The tracker groups the
// blocks of generation k still alive after load k+1 (the previous world's leftovers) by call site. Direct malloc/free sites are not
// counted; a block freed here that was allocated elsewhere only pulls the figure down, never up.
#include <atomic>
#include <malloc.h>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <array>
#include <algorithm>

static std::atomic<long long> g_allocStatsLive{0};
static std::atomic<long long> g_allocStatsBytes{0};

namespace {
struct TrkRec { uint32_t size; uint32_t gen; uint32_t site; };
std::mutex g_trkMu;
std::unordered_map<void*, TrkRec>* g_trk = nullptr;
std::vector<std::array<void*, 10>>* g_sites = nullptr;
std::unordered_map<uint64_t, uint32_t>* g_siteIdx = nullptr;
uint32_t g_trkGen = 0;
bool g_trkOn = false;
thread_local bool tl_inTracker = false;
}

void mc2AllocStatsSnapshot(long long* live, long long* bytes)
{
    *live = g_allocStatsLive.load(std::memory_order_relaxed);
    *bytes = g_allocStatsBytes.load(std::memory_order_relaxed);
}

static void trackAlloc(void* p, size_t bytes)
{
    if (!g_trkOn || tl_inTracker || !p) return;
    tl_inTracker = true;
    void* fr[12];
    USHORT n = CaptureStackBackTrace(2, 12, fr, nullptr);
    uint64_t h = 1469598103934665603ull;
    std::array<void*, 10> key{};
    for (USHORT i = 0; i < n && i < 10; ++i) { key[i] = fr[i]; h = (h ^ (uint64_t)fr[i]) * 1099511628211ull; }
    {
        std::lock_guard<std::mutex> lk(g_trkMu);
        auto it = g_siteIdx->find(h);
        uint32_t si;
        if (it == g_siteIdx->end()) { si = (uint32_t)g_sites->size(); g_sites->push_back(key); (*g_siteIdx)[h] = si; }
        else si = it->second;
        (*g_trk)[p] = TrkRec{(uint32_t)bytes, g_trkGen, si};
    }
    tl_inTracker = false;
}

static void trackFree(void* p)
{
    if (!g_trkOn || tl_inTracker || !p) return;
    tl_inTracker = true;
    { std::lock_guard<std::mutex> lk(g_trkMu); g_trk->erase(p); }
    tl_inTracker = false;
}

void mc2AllocTrackStart()
{
    tl_inTracker = true;
    g_trk = new std::unordered_map<void*, TrkRec>();
    g_sites = new std::vector<std::array<void*, 10>>();
    g_siteIdx = new std::unordered_map<uint64_t, uint32_t>();
    g_trk->reserve(1 << 20);
    tl_inTracker = false;
    g_trkOn = true;
}

void mc2AllocTrackNextGen() { ++g_trkGen; }
uint32_t mc2AllocTrackGen() { return g_trkGen; }

void mc2AllocTrackReport(uint32_t gen)
{
    if (!g_trkOn) return;
    tl_inTracker = true;
    struct Agg { uint64_t count = 0; uint64_t bytes = 0; };
    std::unordered_map<uint32_t, Agg> agg;
    uint64_t total = 0, totalBytes = 0;
    {
        std::lock_guard<std::mutex> lk(g_trkMu);
        for (auto& kv : *g_trk)
            if (kv.second.gen == gen) { auto& a = agg[kv.second.site]; ++a.count; a.bytes += kv.second.size; ++total; totalBytes += kv.second.size; }
    }
    std::vector<std::pair<uint32_t, Agg>> v(agg.begin(), agg.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.count > b.second.count; });
    mc2::fmt::print_out("[ALLOC_SURVIVORS] gen={} blocks={} bytes={} sites={}\n", mc2::fmt::as_u32(gen), mc2::fmt::as_u64(total), mc2::fmt::as_u64(totalBytes), mc2::fmt::as_u64(v.size()));
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    static bool s_symInit = false;
    if (!s_symInit) { SymInitialize(proc, nullptr, TRUE); s_symInit = true; }
    for (size_t r = 0; r < v.size() && r < 12; ++r) {
        mc2::fmt::print_out("[ALLOC_SITE] rank={} blocks={} bytes={}\n", mc2::fmt::as_u64(r), mc2::fmt::as_u64(v[r].second.count), mc2::fmt::as_u64(v[r].second.bytes));
        auto& key = (*g_sites)[v[r].first];
        for (int i = 0; i < 8 && key[i]; ++i) {
            char symBuf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 256;
            DWORD64 d = 0; const char* nm = "?";
            IMAGEHLP_LINE64 ln{}; ln.SizeOfStruct = sizeof(ln); DWORD dl = 0; const char* fn = "?"; DWORD lno = 0;
            if (SymFromAddr(proc, (DWORD64)key[i], &d, sym)) nm = sym->Name;
            if (SymGetLineFromAddr64(proc, (DWORD64)key[i], &dl, &ln)) { fn = ln.FileName; lno = ln.LineNumber; }
            mc2::fmt::print_out("    {} ({}:{})\n", mc2::fmt::safe(nm), mc2::fmt::safe(fn), mc2::fmt::as_u32(lno));
        }
    }
    fflush(stdout);
    tl_inTracker = false;
}
