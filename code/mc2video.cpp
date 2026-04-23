//=============================================================================
// FFmpeg-backed video decoder — sole translation unit with FFmpeg symbols.
//=============================================================================
#include "mc2video.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>   // std::min

#include "file.h"   // fileExists
#include "gameos.hpp"  // gos_NewEmptyTexture, gos_DestroyTexture, gos_Texture_Alpha, RECT_TEX, gosHint_DisableMipmap, DWORD
extern char moviePath[80];  // defined in mclib/paths.cpp

#if defined(_WIN32)
#  include <windows.h>
#  include <delayimp.h>
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
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

// Plain LoadLibraryA probe. Catches the only case that matters for the
// startup gate: "DLL is not present / cannot be found on disk". We do
// NOT use __HrLoadAllImportsForDll here because it additionally
// resolves every delay-loaded symbol against the DLL, which is
// over-strict — a single symbol mismatch (e.g., cross-build symbol
// churn) would falsely disable playback even though the DLL is
// functional for our real call set. If any symbol genuinely cannot be
// resolved at first use, the delay-load failure hook above still
// catches it and flips g_ffmpegAvailable then.
static bool tryLoadOne(const char* dllName)
{
    __try {
        HMODULE h = LoadLibraryA(dllName);
        if (!h) return false;
        // Keep the DLL loaded for the process lifetime. Not calling
        // FreeLibrary avoids a reload churn when real call sites hit.
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
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
        if (!tryLoadOne(d)) {
            DWORD err = GetLastError();
            VIDEO_LOG("FFmpeg unavailable: LoadLibraryA(%s) failed (err=%lu). "
                      "Video playback disabled for this session.", d, err);
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
// Resolver — viable-index enumeration over the loose-file override chain
//-----------------------------------------------------------------------------
bool resolveVideoCandidate(const char* shortName, bool preferUpscaled,
                           int index, char* outPath, unsigned outPathSize)
{
    if (!shortName || !outPath || outPathSize < 2 || index < 0) return false;

    // The raw chain, in priority order.  Each entry is (extension,
    // requiresLooseFile).  The last entry (FST .bik) has
    // requiresLooseFile=false: it is always considered viable because
    // File::open consults the FST archive if the loose path is missing.
    struct Slot { const char* ext; bool looseRequired; };
    static constexpr Slot kUpscaleSlots[] = {
        { ".mp4",  true  },
        { ".mkv",  true  },
        { ".webm", true  },
    };
    static constexpr Slot kOriginalSlots[] = {
        { ".bik",  true  },   // loose .bik
        { ".bik",  false },   // FST-fallback .bik (always viable)
    };

    // Walk the raw chain in order, skipping loose slots whose file is absent.
    // The caller's index selects the Nth *viable* slot.
    int viableSoFar = 0;
    auto tryEmit = [&](const Slot& s) -> bool {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s%s%s", moviePath, shortName, s.ext);
        if (s.looseRequired && !fileExists(tmp)) return false;
        if (viableSoFar == index) {
            snprintf(outPath, outPathSize, "%s", tmp);
            VIDEO_TRACE("resolver: viable idx=%d ext=%s path=%s%s",
                        index, s.ext,
                        s.looseRequired ? "" : "(FST) ", outPath);
            return true;
        }
        ++viableSoFar;
        return false;
    };

    if (preferUpscaled) {
        for (const Slot& s : kUpscaleSlots) {
            if (tryEmit(s)) return true;
        }
    }
    for (const Slot& s : kOriginalSlots) {
        if (tryEmit(s)) return true;
    }

    // Chain exhausted without reaching the requested viable index.
    return false;
}

//-----------------------------------------------------------------------------
// Session open/close/update/render — implemented in Tasks 9-15
//-----------------------------------------------------------------------------
struct VideoSession {
    // Demuxer / video decoder
    AVFormatContext* fmt       = nullptr;
    int              vStream   = -1;
    AVCodecContext*  vCodec    = nullptr;
    SwsContext*      sws       = nullptr;
    AVFrame*         vFrame    = nullptr;
    AVPacket*        pkt       = nullptr;
    int              srcW = 0, srcH = 0;
    double           sar = 1.0;
    double           fps = 30.0;
    double           timeBase = 0.0;   // seconds per PTS unit

    // gos texture (Path GL-A)
    DWORD            texHandle = 0;   // 0 == INVALID_TEXTURE_ID (caller must treat as invalid)

    // Draw geometry (screen-space, computed once)
    float            quadX0 = 0, quadY0 = 0, quadX1 = 0, quadY1 = 0;
    int              rectX = 0, rectY = 0, rectW = 0, rectH = 0;
    bool             frameReady = false;

    // Playback clock (audio-master replaces wall clock in Task 13)
    double           clockStart   = 0.0;
    double           presentedPTS = -1.0;

    // Decoded-but-not-yet-due frame (Task 10 uses these)
    bool             pendingFrameValid = false;
    double           pendingFramePTS   = 0.0;

    // Audio — Task 12 fills these in (keep forward-compatible struct shape)
    int              aStream   = -1;
    AVCodecContext*  aCodec    = nullptr;
    SwrContext*      swr       = nullptr;

    // Lifecycle
    bool             paused = false;
    bool             eof    = false;
};

static void computeLetterboxQuad(int srcW, int srcH, double sar,
                                 int rectX, int rectY, int rectW, int rectH,
                                 float& x0, float& y0, float& x1, float& y1)
{
    if (rectW <= 0 || rectH <= 0 || srcW <= 0 || srcH <= 0) {
        x0 = (float)rectX; y0 = (float)rectY;
        x1 = (float)(rectX + rectW); y1 = (float)(rectY + rectH);
        return;
    }
    double srcAspectW = (double)srcW * sar;
    double scale = std::min((double)rectW / srcAspectW, (double)rectH / srcH);
    double quadW = srcAspectW * scale;
    double quadH = srcH * scale;
    double cx = rectX + rectW * 0.5;
    double cy = rectY + rectH * 0.5;
    x0 = (float)(cx - quadW * 0.5);
    y0 = (float)(cy - quadH * 0.5);
    x1 = (float)(cx + quadW * 0.5);
    y1 = (float)(cy + quadH * 0.5);
}

VideoSession* video_open(const VideoOpenParams& p, VideoOpenResult* out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!g_ffmpegAvailable || !p.resolvedPath) return nullptr;

    VideoSession* s = new VideoSession();

    // 1. Demux
    if (avformat_open_input(&s->fmt, p.resolvedPath, nullptr, nullptr) < 0) {
        VIDEO_LOG("avformat_open_input failed for '%s'", p.resolvedPath);
        video_close(s); return nullptr;
    }
    if (avformat_find_stream_info(s->fmt, nullptr) < 0) {
        VIDEO_LOG("avformat_find_stream_info failed for '%s'", p.resolvedPath);
        video_close(s); return nullptr;
    }

    // 2. Video stream
    s->vStream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (s->vStream < 0) {
        VIDEO_LOG("no video stream in '%s'", p.resolvedPath);
        video_close(s); return nullptr;
    }
    AVStream* vst = s->fmt->streams[s->vStream];
    const AVCodec* vcodec = avcodec_find_decoder(vst->codecpar->codec_id);
    if (!vcodec) { video_close(s); return nullptr; }
    s->vCodec = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(s->vCodec, vst->codecpar);
    if (avcodec_open2(s->vCodec, vcodec, nullptr) < 0) {
        VIDEO_LOG("video decoder open failed");
        video_close(s); return nullptr;
    }

    s->srcW = s->vCodec->width;
    s->srcH = s->vCodec->height;
    s->sar  = (vst->sample_aspect_ratio.num && vst->sample_aspect_ratio.den)
              ? av_q2d(vst->sample_aspect_ratio) : 1.0;
    s->fps  = (vst->avg_frame_rate.num && vst->avg_frame_rate.den)
              ? av_q2d(vst->avg_frame_rate) : 30.0;
    s->timeBase = av_q2d(vst->time_base);

    // 3. Swscale — target BGRA (matches gos_LockTexture pixel layout)
    s->sws = sws_getContext(s->srcW, s->srcH, s->vCodec->pix_fmt,
                             s->srcW, s->srcH, AV_PIX_FMT_BGRA,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!s->sws) { video_close(s); return nullptr; }

    s->vFrame = av_frame_alloc();
    s->pkt    = av_packet_alloc();

    // 4. gos texture (Path GL-A) — sized to source, gos_Texture_Alpha = BGRA8.
    //    RECT_TEX(w,h) packs non-square dims into one DWORD.
    s->texHandle = gos_NewEmptyTexture(gos_Texture_Alpha,
                                        "mc2video",
                                        RECT_TEX((DWORD)s->srcW, (DWORD)s->srcH),
                                        gosHint_DisableMipmap);
    if (s->texHandle == 0) {
        VIDEO_LOG("gos_NewEmptyTexture failed %dx%d", s->srcW, s->srcH);
        video_close(s); return nullptr;
    }

    // 5. Quad geometry
    s->rectX = 0; s->rectY = 0;
    s->rectW = p.destRectW;
    s->rectH = p.destRectH;
    computeLetterboxQuad(s->srcW, s->srcH, s->sar,
                         s->rectX, s->rectY, s->rectW, s->rectH,
                         s->quadX0, s->quadY0, s->quadX1, s->quadY1);

    // 6. Audio stream detection (Task 12 opens the decoder)
    s->aStream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    // 7. Fill out
    if (out) {
        out->srcW = s->srcW; out->srcH = s->srcH;
        out->sar  = s->sar;  out->fps  = s->fps;
        out->hasAudio = (s->aStream >= 0);
        out->hasAlpha = (s->vCodec->pix_fmt == AV_PIX_FMT_YUVA420P ||
                         s->vCodec->pix_fmt == AV_PIX_FMT_ARGB ||
                         s->vCodec->pix_fmt == AV_PIX_FMT_RGBA ||
                         s->vCodec->pix_fmt == AV_PIX_FMT_BGRA);
        out->glTextureId = 0;             // Path GL-A: we don't expose GL id
        out->quadX0 = s->quadX0; out->quadY0 = s->quadY0;
        out->quadX1 = s->quadX1; out->quadY1 = s->quadY1;
    }

    VIDEO_LOG("init: opened '%s' (%dx%d, sar=%.3f, fps=%.2f, audio=%d)",
              p.resolvedPath, s->srcW, s->srcH, s->sar, s->fps, (int)(s->aStream >= 0));

    // 8. Clock
    s->clockStart   = (double)av_gettime() / 1000000.0;
    s->presentedPTS = -1.0;

    return s;
}

void video_close(VideoSession* s)
{
    if (!s) return;
    if (s->texHandle != 0) {
        gos_DestroyTexture(s->texHandle);
        s->texHandle = 0;
    }
    if (s->vFrame)  { av_frame_free(&s->vFrame); }
    if (s->pkt)     { av_packet_free(&s->pkt); }
    if (s->sws)     { sws_freeContext(s->sws); s->sws = nullptr; }
    if (s->vCodec)  { avcodec_free_context(&s->vCodec); }
    if (s->aCodec)  { avcodec_free_context(&s->aCodec); }
    if (s->swr)     { swr_free(&s->swr); }
    if (s->fmt)     { avformat_close_input(&s->fmt); }
    delete s;
}

//-----------------------------------------------------------------------------
// Master-clock helpers
//-----------------------------------------------------------------------------
static double nowSeconds()
{
    return (double)av_gettime() / 1000000.0;
}

// Placeholder for Task 13. For now: wall-clock only. Task 13 will
// replace this with audio-master clock when audio is available.
static double videoMasterClock(const VideoSession* s)
{
    return nowSeconds() - s->clockStart;
}

//-----------------------------------------------------------------------------
// Decode-one-frame helper
// Pulls packets and decodes until one video frame is produced in
// s->vFrame, or EOF. Returns: 1 = frame produced, -1 = EOF/error.
// Audio packets are discarded until Task 12 adds handling.
//-----------------------------------------------------------------------------
static int decodeNextVideoFrame(VideoSession* s)
{
    for (;;) {
        int ret = avcodec_receive_frame(s->vCodec, s->vFrame);
        if (ret == 0) return 1;
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) return -1;

        // Need more input
        ret = av_read_frame(s->fmt, s->pkt);
        if (ret == AVERROR_EOF) {
            avcodec_send_packet(s->vCodec, nullptr);  // flush
            ret = avcodec_receive_frame(s->vCodec, s->vFrame);
            return (ret == 0) ? 1 : -1;
        }
        if (ret < 0) return -1;

        if (s->pkt->stream_index == s->vStream) {
            avcodec_send_packet(s->vCodec, s->pkt);
        }
        // audio packets handled in Task 12; for now discard
        av_packet_unref(s->pkt);
    }
}

//-----------------------------------------------------------------------------
// Upload helper — Path GL-A (gos_LockTexture, BGRA)
// sws_scale directly into the locked buffer. Linesize is in bytes;
// for a DWORD* BGRA buffer, bytes per row = Width * 4.
// NOTE: Pitch is in DWORDs — do NOT use for sws stride.
//-----------------------------------------------------------------------------
static void uploadFrameToTexture(VideoSession* s)
{
    if (s->texHandle == 0) return;

    TEXTUREPTR td = {};
    gos_LockTexture(s->texHandle, /*MipMapSize*/0, /*ReadOnly*/false, &td);
    if (!td.pTexture) {
        VIDEO_LOG("gos_LockTexture returned null pTexture");
        return;
    }

    // sws_scale directly into the locked buffer.
    // For a DWORD* BGRA buffer, bytes per row = Width * 4.
    uint8_t* dst[4]     = { (uint8_t*)td.pTexture, nullptr, nullptr, nullptr };
    int      dstLine[4] = { (int)(td.Width * 4), 0, 0, 0 };
    sws_scale(s->sws,
              s->vFrame->data, s->vFrame->linesize,
              0, s->srcH,
              dst, dstLine);

    gos_UnLockTexture(s->texHandle);
    s->frameReady = true;
}

//-----------------------------------------------------------------------------
// video_update — decode loop with frame-hold for future PTS + late-frame drop
//-----------------------------------------------------------------------------
bool video_update(VideoSession* s)
{
    if (!s || s->eof) return true;
    if (s->paused) return false;

    const double masterNow = videoMasterClock(s);

    // Step A: if we have a pending frame, check whether it is now due.
    if (s->pendingFrameValid) {
        if (s->pendingFramePTS > masterNow) {
            // Still in the future — hold, do not upload, do not present.
            return false;
        }
        // Due. Promote to the display texture.
        uploadFrameToTexture(s);
        s->presentedPTS = s->pendingFramePTS;
        s->pendingFrameValid = false;
    }

    // Step B: pull frames from the decoder. Future frames park in the
    // pending slot. Severely-late frames drop silently without upload.
    // On-time frames upload and we keep looking for a future frame
    // to park before returning.
    for (;;) {
        int r = decodeNextVideoFrame(s);
        if (r < 0) { s->eof = true; return true; }

        double pts = (s->vFrame->pts == AV_NOPTS_VALUE)
                     ? (s->presentedPTS + 1.0 / s->fps)
                     : s->vFrame->pts * s->timeBase;

        if (pts > masterNow) {
            // Future frame — park and return. Do NOT upload yet.
            s->pendingFrameValid = true;
            s->pendingFramePTS   = pts;
            return false;
        }

        // Frame is due or late. If more than ~2 frames behind, drop
        // without upload and keep pulling; else upload and continue.
        const double lateThreshold = 2.0 / (s->fps > 0 ? s->fps : 30.0);
        if (masterNow - pts > lateThreshold) {
            continue;  // drop silently; vFrame gets overwritten next iter
        }

        uploadFrameToTexture(s);
        s->presentedPTS = pts;
        // loop; next iteration parks a future frame or continues catching up
    }
}

void video_render(VideoSession* /*s*/) { /* Task 11 */ }
void video_pause(VideoSession* /*s*/, bool /*paused*/) { /* Task 15 */ }
void video_stop(VideoSession* /*s*/) { /* Task 15 */ }
void video_restart(VideoSession* /*s*/) { /* Task 15 */ }
void video_setRect(VideoSession* /*s*/, int /*x0*/, int /*y0*/,
                   int /*w*/, int /*h*/) { /* Task 15 */ }
