// G7: sim tick N+1 overlapped with the tail of frame N (see docs/plans/2026-10-28-g7-overlap-audit.md).
//
// One persistent sim worker runs the banked tick(s) while the main thread finishes frame N (pass timers, present,
// frame cap). The render CPU walk, the flushes, post-process, HUD and ImGui are all done before the kick, and the main
// loop joins right after the frame cap, so no render read of a live sim object overlaps a tick. The audit entry points
// below check that claim: they are one relaxed atomic load each.
#pragma once

#include <functional>

namespace mc2::overlap {

// The mission code installs these; the main loop calls kickTail() / joinTail() around the frame's presenting tail.
struct Hooks { void (*kick)(); void (*join)(); void (*extract)() = nullptr; };
void installHooks(const Hooks& hooks);
void extractRender(); // main thread, at ExtractRenderSnapshot: builds the frame's render list/clock (G7b K3)
void kickTail();   // main thread: bank the next tick(s) and start them on the sim worker (no-op if not eligible)
void joinTail();   // main thread: wait for the sim worker to finish (no-op if none is in flight)

// Sim worker.
void runOnWorker(std::function<void()> job);   // main thread only; one job at a time
void waitWorker();                             // main thread only; returns once the job has finished
bool jobInFlight();                            // true between runOnWorker and the worker finishing the job
bool onSimWorker();                            // true on the sim worker thread

// Audit. A violation prints "[SIM_OVERLAP v1] VIOLATION ..." once per site; MC2_SIM_OVERLAP_AUDIT=1 aborts.
void assertSimQuiescent(const char* site);     // render side: reads live sim objects, so no tick may be in flight
void assertMainThread(const char* site);       // sim side: GL/Vulkan/texture/sound entry points, never on the worker
unsigned violationCount();
void beginFrameDraw(); // counts diagnostic frames; optionally kicks before DrawScreen
void printHazards();   // once at run shutdown, after the final join

} // namespace mc2::overlap
