#include "present_thread.h"
#include "engine_present_vk.h"
#include "vulkan_context.h"
#include "vk_texture_residency.h"
#include "../../mclib/sim_overlap.h"
#include "../../mclib/battle_bench_wait.h"
#include "Gates/gate.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <cassert>

namespace mc2vk::present_thread {
namespace {
struct Packet {
    uint32_t width, height, renderFrame, fireFrame;
    bool verify;
    int fps;
    std::vector<ResidentTextureRef> retiredTextures;
};
std::mutex mutex;
std::condition_variable changed;
std::thread worker;
thread_local bool isWorker = false;
bool running = false, quitting = false, pending = false, busy = false, sealed = true, ready = false;
bool rollback = false;
Packet packet{};
int paceFps = 0;
std::vector<ResidentTextureRef> retiredTextures;

void run() {
    isWorker = true;
    mc2::overlap::registerThread(mc2::overlap::Owner::Render);
    {
        std::lock_guard lock(mutex);
        mc2::overlap::setRenderWorkerOwns(false);
        ready = true;
    }
    changed.notify_all();
    auto lastPaced = std::chrono::steady_clock::now();
    for (;;) {
        Packet next;
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [] { return pending || quitting; });
            if (!pending && quitting) break;
            next = std::move(packet);
            pending = false;
            busy = true;
        }
        mc2::overlap::registerThread(mc2::overlap::Owner::Render);
        next.retiredTextures.clear();
        auto& present = getOrCreateEnginePresentVk();
        present.setCaptureFrame(next.renderFrame, next.fireFrame);
        if (!present.presentFrame(next.width, next.height, next.verify)) {
            std::fprintf(stderr, "[PRESENT_THREAD] present failed: %s\n", present.health().fallback_reason.c_str());
        }
        if (next.fps > 0) {
            const auto deadline = lastPaced + std::chrono::milliseconds(1000 / next.fps);
            std::this_thread::sleep_until(deadline);
            lastPaced = std::chrono::steady_clock::now();
        } else {
            lastPaced = std::chrono::steady_clock::now();
        }
        {
            std::lock_guard lock(mutex);
            busy = false;
        }
        changed.notify_all();
    }
}
}

void start() {
    mc2::overlap::assertMainThread("present_thread::start");
    std::unique_lock lock(mutex);
    if (running) return;
    running = true;
    quitting = false;
    ready = false;
    mc2::overlap::setPresentWorkerRegistered(true);
    worker = std::thread(run);
    changed.wait(lock, [] { return ready; });
    std::fprintf(stderr, "[PRESENT_THREAD] enabled mailbox_depth=1\n");
}
bool enabled() { return running; }
bool onWorker() { return isWorker; }
void waitSealed(std::source_location site) {
    if (!running || isWorker) return;
    std::unique_lock lock(mutex);
    const auto began = std::chrono::steady_clock::now();
    if (!sealed) mc2::battlebench::noteVkWait(site.function_name(), began);
    mc2::battlebench::WaitScope timed(mc2::battlebench::WaitSealedNs);
    if (!changed.wait_for(lock, std::chrono::seconds(5), [] { return sealed; })) {
        std::fprintf(stderr, "[PRESENT_THREAD] STALL >5s; inline rollback after owner quiescence\n");
        rollback = true;
        changed.wait(lock, [] { return sealed; });
    }
    if (rollback) {
        lock.unlock();
        stop();
        std::fprintf(stderr, "[PRESENT_THREAD] INLINE FALLBACK active\n");
    }
}
void waitIdle() {
    if (!running || isWorker) return;
    std::unique_lock lock(mutex);
    changed.wait(lock, [] { return !pending && !busy; });
}
void enqueue(uint32_t width, uint32_t height, bool verify, uint32_t renderFrame, uint32_t fireFrame) {
    mc2::overlap::assertMainThread("present_thread::enqueue");
    std::lock_guard lock(mutex);
    assert(!pending && sealed);
    packet = {width, height, renderFrame, fireFrame, verify, paceFps, std::move(retiredTextures)};
    sealed = false;
    pending = true;
    mc2::overlap::setRenderWorkerOwns(true);
    changed.notify_all();
}
void seal() {
    if (!isWorker) return;
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "present_thread::seal");
    std::lock_guard lock(mutex);
    mc2::overlap::setRenderWorkerOwns(false);
    mc2::overlap::registerThread(mc2::overlap::Owner::Present);
    sealed = true;
    changed.notify_all();
}
void pace(uint64_t, int fps) {
    mc2::overlap::assertMainThread("present_thread::pace");
    paceFps = fps;
}
void forgetTexture(uint32_t handle) {
    mc2::overlap::assertMainThread("present_thread::forgetTexture");
    auto retired = getOrCreateEngineVulkanContext().textures().detachLegacyUi(handle);
    for (auto& texture : retired) retiredTextures.push_back(std::move(texture));
}
void requestInline() {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "present_thread::requestInline");
    std::lock_guard lock(mutex);
    rollback = true;
}
void stop() {
    mc2::overlap::assertMainThread("present_thread::stop");
    if (!running) return;
    waitIdle();
    {
        std::lock_guard lock(mutex);
        quitting = true;
    }
    changed.notify_all();
    worker.join();
    running = false;
    mc2::overlap::setRenderWorkerOwns(false);
    mc2::overlap::setPresentWorkerRegistered(false);
    retiredTextures.clear();
}
}
