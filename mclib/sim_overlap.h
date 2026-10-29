// G7b K5: sim tick N+1 overlaps frame N's snapshot draw.
//
// Extract runs with the sim quiescent. Render consumes the extracted views and presentation-owned resources until
// the wait after DrawScreen; probes and frame-counter changes then touch live state safely. Queues commit after present.
#pragma once

#include <functional>
#include <cstdint>
#include <string_view>

namespace mc2::overlap {

// The mission code installs these; the main loop kicks after extract and joins after present.
struct Hooks { void (*kick)(); void (*join)(); void (*extract)() = nullptr; };
void installHooks(const Hooks& hooks);
void extractRender(); // main thread, at ExtractRenderSnapshot: builds the frame's render list/clock (G7b K3)
void joinTail();   // main thread: wait for the sim worker to finish (no-op if none is in flight)

// Sim worker.
void runOnWorker(std::function<void()> job);   // main thread only; one job at a time
void waitWorker();                             // main thread only; returns once the job has finished
bool jobInFlight();                            // true between runOnWorker and the worker finishing the job
bool onSimWorker();                            // true on the sim worker thread
uint32_t simulationFrame(uint32_t serialFrame); // worker's tick epoch; render's frame counter stays unchanged during draw
void setSimulationFrame(uint32_t frame);        // sim worker only, before each tick

// Audit. The first 20 violations print "[SIM_OVERLAP v1] VIOLATION ..."; MC2_SIM_OVERLAP_AUDIT=1 aborts.
void assertSimQuiescent(std::string_view site);     // render side: reads live sim objects, so no tick may be in flight
void assertMainThread(std::string_view site);      // sim side: GL/Vulkan/texture/sound entry points, never on the worker
void beginFrameDraw(); // main thread: bank and kick after extraction, before DrawScreen (no-op if not eligible)

} // namespace mc2::overlap
