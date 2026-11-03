// Startup preload worker (see startup_preload.h).  The path list is built on the main thread from
// the FastFile list and the mod VFS winners (main-thread-owned registries); the worker only owns
// its path vector and a private read buffer.
#include "startup_preload.h"

#include "../mclib/fastfile.h"
#include "../mclib/ffile.h"
#include "../mclib/file.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
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

void collectModFile(const char* key, void* user)
{
	auto* out = static_cast<std::vector<Entry>*>(user);
	if (std::strncmp(key, "data/movies/", 12) == 0 || std::strncmp(key, "data/sound", 10) == 0
		|| std::strncmp(key, "data/music", 10) == 0)
		return;
	if (const char* abs = LookupModAbsolutePath(key))
		out->push_back({abs, true});
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
		HANDLE h = CreateFileA(e.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
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
	for (long i = 0; i < numFastFiles; ++i)
		if (fastFiles && fastFiles[i] && fastFiles[i]->getFileName())
			plan.push_back({fastFiles[i]->getFileName(), false});
	const size_t archives = plan.size();
	// Then every mod-winning data file (the worker applies the mod size budget).
	EnumerateModFiles("data/", &collectModFile, &plan);
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
