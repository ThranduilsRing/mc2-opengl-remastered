//=============================================================================
// FFmpeg-backed video decoder — sole translation unit with FFmpeg symbols.
//=============================================================================
#include "mc2video.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#if defined(_WIN32)
#  include <windows.h>
#  include <delayimp.h>
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

//-----------------------------------------------------------------------------
// Availability gate
//-----------------------------------------------------------------------------
bool g_ffmpegAvailable = true;

static const bool s_videoTrace = (getenv("MC2_DEBUG_VIDEO") != nullptr);
#define VIDEO_TRACE(fmt, ...) do { \
    if (s_videoTrace) { printf("[VIDEO] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } \
} while (0)

#define VIDEO_LOG(fmt, ...) do { \
    printf("[VIDEO] " fmt "\n", ##__VA_ARGS__); fflush(stdout); \
} while (0)

#if defined(_WIN32)
// Delay-load failure hook. Defense-in-depth: flags unavailability if a
// delayed import ever triggers post-probe. Returns a sentinel so the
// loader's internal dispatch does not terminate the process at the
// import site. The PRIMARY mechanism is ffmpegProbeAvailability()
// below — the hook is only reached if we slip a call past the gate.
static FARPROC WINAPI Mc2VideoDelayLoadFailureHook(unsigned dliNotify,
                                                    PDelayLoadInfo pdli)
{
    if (dliNotify == dliFailLoadLib || dliNotify == dliFailGetProc) {
        if (g_ffmpegAvailable) {
            VIDEO_LOG("FFmpeg delay-load failed post-probe (dll=%s, notify=%u). "
                      "This should not happen if the gate is respected.",
                      pdli && pdli->szDll ? pdli->szDll : "<unknown>",
                      dliNotify);
            g_ffmpegAvailable = false;
        }
        return (FARPROC)0;
    }
    return 0;
}

extern "C" const PfnDliHook __pfnDliFailureHook2 = Mc2VideoDelayLoadFailureHook;

static HRESULT tryLoadOne(const char* dllName)
{
    __try {
        return __HrLoadAllImportsForDll(dllName);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

// mc2video_dlls.h is CMake-generated. It defines MC2_VIDEO_DLL_LIST as
// a static const char* const[] of the runtime DLL filenames. Single
// source of truth: the 3rdparty/ffmpeg-lgpl-win64/bin/ directory.
#include "mc2video_dlls.h"

void ffmpegProbeAvailability()
{
    // Bumping FFmpeg updates one place (the vendored tree); CMake
    // regenerates this header automatically.
    for (const char* d : MC2_VIDEO_DLL_LIST) {
        HRESULT hr = tryLoadOne(d);
        if (FAILED(hr)) {
            VIDEO_LOG("FFmpeg unavailable: probe of %s failed (hr=0x%08lx). "
                      "Video playback disabled for this session.", d, (long)hr);
            g_ffmpegAvailable = false;
            return;
        }
    }
    VIDEO_TRACE("FFmpeg availability probe passed");
}
#else
void ffmpegProbeAvailability() { /* non-Windows: link-loaded, always avail */ }
#endif

//-----------------------------------------------------------------------------
// Resolver — implemented in Task 6
//-----------------------------------------------------------------------------
bool resolveVideoCandidate(const char* /*shortName*/, bool /*preferUpscaled*/,
                           int /*index*/,
                           char* /*outPath*/, unsigned /*outPathSize*/)
{
    return false;  // Task 6
}

//-----------------------------------------------------------------------------
// Session open/close/update/render — implemented in Tasks 9-15
//-----------------------------------------------------------------------------
struct VideoSession {
    bool dummy;   // fields added incrementally by later tasks
};

VideoSession* video_open(const VideoOpenParams& /*p*/, VideoOpenResult* out)
{
    if (!g_ffmpegAvailable) return nullptr;
    if (out) memset(out, 0, sizeof(*out));
    return nullptr;  // Task 9
}

void video_close(VideoSession* /*s*/) { /* Task 9 */ }
bool video_update(VideoSession* /*s*/) { return true; /* Task 10 */ }
void video_render(VideoSession* /*s*/) { /* Task 11 */ }
void video_pause(VideoSession* /*s*/, bool /*paused*/) { /* Task 15 */ }
void video_stop(VideoSession* /*s*/) { /* Task 15 */ }
void video_restart(VideoSession* /*s*/) { /* Task 15 */ }
void video_setRect(VideoSession* /*s*/, int /*x0*/, int /*y0*/,
                   int /*w*/, int /*h*/) { /* Task 15 */ }
