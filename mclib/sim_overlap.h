// G7b K5: sim tick N+1 overlaps frame N's snapshot draw.
//
// Extract runs with the sim quiescent. Render consumes the extracted views and presentation-owned resources until
// the wait after DrawScreen; probes and frame-counter changes then touch live state safely. Queues commit after present.
#pragma once

#include <atomic>
#include <functional>
#include <cstdint>
#include <string_view>

namespace mc2::overlap {

// The mission code installs these; the main loop kicks after extract and joins after present.
struct Hooks { void (*kick)(); void (*join)(); void (*extract)() = nullptr; };
void installHooks(const Hooks& hooks);
// Idempotent: routes SIGABRT and the CRT invalid-parameter handler into crashbundle_fatal. Called from crashbundle_init.
void installFatalHandlers(void (*fatalSink)(std::string_view));
void extractRender(); // main thread, at ExtractRenderSnapshot: builds the frame's render list/clock (G7b K3)
void joinTail();   // main thread: wait for the sim worker to finish (no-op if none is in flight)

// Sim worker.
void runOnWorker(std::function<void()> job);   // main thread only; one job at a time
void waitWorker();                             // main thread only; returns once the job has finished
void shutdownWorker();                         // main thread only, at exit: waits out any job, stops and joins the worker
bool jobInFlight();                            // true between runOnWorker and the worker finishing the job
bool onSimWorker();                            // true on the sim worker thread
uint32_t simulationFrame(uint32_t serialFrame); // worker's tick epoch; render's frame counter stays unchanged during draw
void setSimulationFrame(uint32_t frame);        // sim worker only, before each tick

// Thread registry. Each long-lived thread names its role once; the job pool's threads stay unregistered (never fail).
//   Main   = the frame loop thread (input, sound, ABL commit, queue commits).
//   Sim    = the overlap worker running banked ticks.
//   Render = GPU mutation, transferred from Main to the P2 worker until the frame is sealed.
//   Present = WSI after sealing; the worker can block there while Main records the next frame.
enum class Owner : uint8_t { Main, Sim, Render, Present };
void registerThread(Owner role);   // call once on the thread; the sim worker registers itself
// P2 transfers GPU mutation ownership at the sealed-frame boundary; WSI has no game-state access.
void setRenderWorkerOwns(bool owns);
void setPresentWorkerRegistered(bool registered);
// Audit. The first 20 violations print "[SIM_OVERLAP v1] VIOLATION ..."; MC2_SIM_OVERLAP_AUDIT=1 prints and aborts.
// assertOwner fails when the calling thread is registered and does not own `domain` (Sim is owned by the worker, or by
// Main while no tick is in flight). requireOwner is the same check but aborts whatever the gate says (GPU resource entries).
void assertOwner(Owner domain, std::string_view site);
bool isOwner(Owner domain);
void requireOwner(Owner domain, std::string_view site);
void assertSimQuiescent(std::string_view site);     // render side: reads live sim objects, so no tick may be in flight
inline void assertMainThread(std::string_view site) { assertOwner(Owner::Main, site); }  // sound/ABL/RNG entry points
void beginFrameDraw(); // main thread: bank and kick after extraction, before DrawScreen (no-op if not eligible)

// MC2_SIM_OVERLAP_AUDIT>=2: "render owns" fence. From beginFrameDraw until waitWorker the frame thread draws the
// snapshot only; a live GameObject read on Main inside that window is a violation even if the tick already finished.
// Audit levels 3..52 sleep 0..(value-2) ms (private RNG) at tick start and before DrawScreen.
extern std::atomic<bool> g_renderFence;   // armed at audit level >=2 while a tick was kicked this frame
void fenceRead(std::string_view site);
inline void assertLiveSimRead(std::string_view site) {
    if (g_renderFence.load(std::memory_order_relaxed)) [[unlikely]] fenceRead(site);
}

} // namespace mc2::overlap
