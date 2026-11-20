// Startup preload worker (see startup_preload.h).  The path list is built on the main thread from
// the FastFile list and the mod VFS winners (main-thread-owned registries); the worker only owns
// its path vector and a private read buffer.
#include "startup_preload.h"

#include "diagnostic_trace.h"
#include "Gates/gate.h"

#include "../mclib/fastfile.h"
#include "../mclib/ffile.h"
#include "../mclib/file.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

extern FastFile** fastFiles;
extern long numFastFiles;

namespace {

constexpr unsigned long long kModBudgetBytes = 1536ull * 1024 * 1024;
constexpr unsigned long long kModFileMaxBytes = 96ull * 1024 * 1024;

struct Entry {
	std::string path;
	bool fromMod = false;
};

std::thread g_worker;
std::atomic<bool> g_stop{false};

void addModFile(std::string_view key, std::vector<Entry>& out)
{
	if (key.starts_with("data/movies/") || key.starts_with("data/sound") || key.starts_with("data/music"))
		return;
	const std::string_view abs = LookupModAbsolutePath(key) ? std::string_view(LookupModAbsolutePath(key)) : std::string_view();
	if (!abs.empty())
		out.push_back({std::string(abs), true});
}

// ModFitCallback ABI is fixed (absPath is const char*); forward straight into the string_view helper.
template <class KeyPtr>
void collectModFile(KeyPtr key, void* user)
{
	addModFile(std::string_view(key), *static_cast<std::vector<Entry>*>(user));
}

#ifdef _WIN32
void runPlan(std::vector<Entry> plan)
{
	SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
	constexpr size_t kChunk = 1u << 20;
	std::vector<char> buf(kChunk);
	unsigned long long bytes = 0, modBytes = 0;
	size_t files = 0;
	const auto t0 = std::chrono::steady_clock::now();
	for (const Entry& e : plan) {
		if (g_stop.load(std::memory_order_relaxed))
			break;
		HANDLE h = CreateFileA(e.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, // path-audit: loose-only (VFS-resolved paths; OS cache warm only)
			nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			continue;
		LARGE_INTEGER sz{};
		GetFileSizeEx(h, &sz);
		const auto size = static_cast<unsigned long long>(sz.QuadPart);
		if (e.fromMod && (size > kModFileMaxBytes || modBytes + size > kModBudgetBytes)) {
			CloseHandle(h);
			continue;
		}
		DWORD got = 0;
		while (!g_stop.load(std::memory_order_relaxed)
			&& ReadFile(h, buf.data(), static_cast<DWORD>(kChunk), &got, nullptr) && got) {
			bytes += got;
			if (e.fromMod)
				modBytes += got;
		}
		CloseHandle(h);
		++files;
	}
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (mc2_diag::tagRequested("STARTUP_PRELOAD"))
	std::printf("[STARTUP_PRELOAD] done files=%zu mb=%.1f mod_mb=%.1f ms=%.0f%s\n", files,
		static_cast<double>(bytes) / (1024.0 * 1024.0), static_cast<double>(modBytes) / (1024.0 * 1024.0), ms,
		g_stop.load() ? " (stopped)" : "");
	std::fflush(stdout);
}
#else
void runPlan(std::vector<Entry>) {}
#endif

} // namespace

void startStartupPreload()
{
	if (g_worker.joinable())
		return;
	std::vector<Entry> plan;
	// Base archives first: they back every mission.
	// Warm only the active source: the pak when it serves (MC2_PAK on and present), else the FSTs.
	std::error_code pakEc;
	if (mc2::gate::get<"MC2_PAK">() && std::filesystem::is_regular_file("paks/stock.mc2pak", pakEc))
		plan.push_back({"paks/stock.mc2pak", false});
	else
		for (long i = 0; i < numFastFiles; ++i)
			if (fastFiles && fastFiles[i] && fastFiles[i]->getFileName())
				plan.push_back({fastFiles[i]->getFileName(), false});
	const size_t archives = plan.size();
	// Then every mod-winning data file (the worker applies the mod size budget).
	EnumerateModFiles("data/", &collectModFile<const char*>, &plan);
	if (mc2_diag::tagRequested("STARTUP_PRELOAD"))
	std::printf("[STARTUP_PRELOAD] start archives=%zu mod_files=%zu\n", archives, plan.size() - archives);
	std::fflush(stdout);
	g_worker = std::thread(runPlan, std::move(plan));
}

void stopStartupPreload()
{
	g_stop.store(true);
	if (g_worker.joinable())
		g_worker.join();
}

namespace {
// Exit paths that skip TerminateGameEngine (STOP/fatal, exit()) would destroy a joinable g_worker and
// call std::terminate. Declared after g_worker, so this guard is destroyed first; the worker checks
// g_stop every 1 MiB chunk, so the join is short.
struct PreloadExitGuard {
	~PreloadExitGuard() { stopStartupPreload(); }
} g_preloadExitGuard;
} // namespace
