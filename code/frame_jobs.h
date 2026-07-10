// code/frame_jobs.h
// FRAME-JOBS-1: Fixed worker pool for batched CPU-only prep passes.
// Workers must not call GL, gos_*, txmmgr, or ObjectManager mutation.
// Gates: MC2_FRAME_JOBS=1  MC2_FRAME_JOBS_WORKERS=N (1..hw_concurrency)
//        MC2_FRAME_JOBS_BATCH=N (default 64)  MC2_FRAME_JOBS_TRACE=1
//
// MC2-JOBS-CORE-1: this header is now the BACK-COMPAT SHIM. The pool was
// evolved into code/job_system.{h,cpp} (the new domain-tagged `mc2::jobs` API);
// this file keeps the exact FRAME-JOBS-1 free-function surface so existing
// callers (objmgr.cpp, terrobj.cpp, mission.cpp, msl.cpp) compile unchanged and
// with ZERO behavior change. The implementation lives in job_system.cpp. New
// code should prefer the `mc2::jobs` API in job_system.h.
//
// Header-drift note (CORE-1 fix): the comment previously claimed workers "1-8"
// and batch default 128; the code clamps to hw_concurrency and defaults batch
// to 64. The code wins — corrected above.
#pragma once
#include <functional>
#include <cstdint>

// Call once before first frame (e.g., near gpu_cull::substrate_init).
void frameJobsInit();
// Call once at shutdown (near gpu_cull substrate shutdown).
void frameJobsShutdown();

// Execute fn(begin, end) for each chunk of [0, count).
// Serial on main thread when MC2_FRAME_JOBS=0 or resolved workers <= 1.
void parallelForRange(int count, int batchSize,
                      const std::function<void(int, int)>& fn);

bool frameJobsEnabled();  // true when parallel mode is active
int  frameJobsBatch();    // resolved batch size (MC2_FRAME_JOBS_BATCH or 128)
bool frameJobsTrace();    // true when MC2_FRAME_JOBS_TRACE=1
// Returns true only if MC2_FRAME_JOBS=1 AND MC2_FRAME_JOBS_TOUCH=1
bool frameJobsTouchEnabled();

struct FrameJobsFrameStats {
    int  chunksExecuted;
    int  workerCount;
    bool serialFallback;
};
FrameJobsFrameStats frameJobsGetFrameStats();
void                frameJobsResetFrameStats();

// FRAME-JOBS-2D: Thread-local flag set true during touchWorkerPrepass dispatch
// on worker threads. Used by txmmgr/light-data paths to assert they are never
// called from workers. worker_resubmit_calls > 0 = split boundary is broken.
extern thread_local bool g_isFrameJobsWorker;
inline bool isFrameJobsWorkerThread() { return g_isFrameJobsWorker; }
