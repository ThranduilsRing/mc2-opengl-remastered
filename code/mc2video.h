//=============================================================================
// FFmpeg-backed video decoder facade for MC2Movie.
//
// Every FFmpeg symbol lives in mc2video.cpp. This header exposes only
// plain C / C++ types so no other translation unit needs FFmpeg headers.
//=============================================================================
#ifndef MC2VIDEO_H
#define MC2VIDEO_H

#include <cstdint>

struct VideoSession;          // opaque

// Availability of the FFmpeg DLLs at runtime. Flipped to false by
// ffmpegProbeAvailability() at startup if any delay-loaded DLL is
// missing. Readable from anywhere.
extern bool g_ffmpegAvailable;

// MUST be called once at engine startup, before any code path that
// could reach a MC2Movie::init. Probes every FFmpeg DLL inside an SEH
// wrapper and sets g_ffmpegAvailable accordingly. Calling it late (or
// not at all) means the first missing-DLL case will only be caught by
// the delay-load failure hook's defensive path, which is harder to
// reason about.
void ffmpegProbeAvailability();

// Resolves a movie short-name (e.g. "msft") against the override chain.
// See Task 6 for the full contract; stub returns false for now.
bool resolveVideoCandidate(const char* shortName, bool preferUpscaled,
                           int index, char* outPath, unsigned outPathSize);

struct VideoOpenParams {
    const char* resolvedPath;
    // Destination rect in the game's screen-space coordinate system.
    // video_open preserves the origin (X,Y), not just the size —
    // callers that pass a non-zero origin (mission-briefing VIDCOM,
    // in-mission pilot cam, pop-up cinemas) need the quad to land at
    // that position, not at (0,0) with only the size honoured.
    int   destRectX;
    int   destRectY;
    int   destRectW;
    int   destRectH;
    bool  useWaveFile;
    const char* waveFileShortName;
};

struct VideoOpenResult {
    int    srcW;
    int    srcH;
    double sar;
    double fps;
    bool   hasAudio;
    bool   hasAlpha;
    unsigned glTextureId;
    float  quadX0, quadY0, quadX1, quadY1;
};

VideoSession* video_open(const VideoOpenParams& p, VideoOpenResult* out);
void          video_close(VideoSession* s);
bool          video_update(VideoSession* s);
void          video_render(VideoSession* s);
void          video_pause(VideoSession* s, bool paused);
void          video_stop(VideoSession* s);
void          video_restart(VideoSession* s);
void          video_setRect(VideoSession* s, int x0, int y0, int w, int h);

#endif // MC2VIDEO_H
