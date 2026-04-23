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
#include "soundsys.h"  // SoundSystem — PCM push adapter (Task 12)
extern char moviePath[80];  // defined in mclib/paths.cpp
// soundSystem is the global GameSoundSystem* declared in code/gamesound.cpp.
// Forward-declare the subclass so the extern type matches the definition;
// SoundSystem methods are callable via the subclass pointer.
class GameSoundSystem;
extern GameSoundSystem* soundSystem;

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

    // Audio — Task 12
    int              aStream   = -1;
    AVCodecContext*  aCodec    = nullptr;
    SwrContext*      swr       = nullptr;
    int              aOutRate     = 0;
    int              aOutChannels = 0;
    bool             audioStreamStarted = false;

    // Audio-master clock — Task 13
    bool             audioStallLogged = false;

    // Failure-handling log-once flags — Task 16
    bool             loggedDegenerateRect     = false;
    bool             loggedVideoDecodeError   = false;

    // Lifecycle
    bool             paused   = false;
    bool             eof      = false;
    double           pausedAt = 0.0;
};

//-----------------------------------------------------------------------------
// Audio-source helpers (Task 14)
//-----------------------------------------------------------------------------

// Returns true if a WAV sidecar was successfully started via
// SoundSystem::playDigitalStream. Treats return value 0 (== NO_ERR in
// all MC2 headers) as success; any other value means the stream
// could not be started (file not found, mixer busy, etc.).
static bool tryStartSidecarWAV(const char* waveShortName)
{
    if (!soundSystem || !waveShortName || !waveShortName[0]) return false;
    long r = reinterpret_cast<SoundSystem*>(soundSystem)->playDigitalStream(waveShortName);
    return (r == 0L);  // 0 == NO_ERR (project-wide constant)
}

// Returns true iff: an embedded audio stream was found, its decoder
// opened, SwrContext was initialised, and beginVideoPCMStream succeeded.
// On any failure, partial state (aCodec/swr) is torn down cleanly and
// s->aStream is reset to -1.
static bool tryOpenEmbeddedAudio(VideoSession* s)
{
    if (!s->fmt) return false;
    s->aStream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (s->aStream < 0) return false;

    AVStream* ast = s->fmt->streams[s->aStream];
    const AVCodec* ac = avcodec_find_decoder(ast->codecpar->codec_id);
    if (!ac) { s->aStream = -1; return false; }

    s->aCodec = avcodec_alloc_context3(ac);
    avcodec_parameters_to_context(s->aCodec, ast->codecpar);

    SoundSystem* snd = reinterpret_cast<SoundSystem*>(soundSystem);
    if (avcodec_open2(s->aCodec, ac, nullptr) != 0 ||
        !snd ||
        !snd->queryNativeFormat(&s->aOutRate, &s->aOutChannels))
    {
        if (s->aCodec) { avcodec_free_context(&s->aCodec); s->aCodec = nullptr; }
        s->aStream = -1;
        return false;
    }

    // Build SwrContext.
    AVChannelLayout inLayout;
    av_channel_layout_copy(&inLayout, &s->aCodec->ch_layout);
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, s->aOutChannels);

    s->swr = nullptr;
    int swrRet = swr_alloc_set_opts2(&s->swr,
        &outLayout, AV_SAMPLE_FMT_S16, s->aOutRate,
        &inLayout,  s->aCodec->sample_fmt, s->aCodec->sample_rate,
        0, nullptr);
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);

    if (swrRet < 0 || !s->swr || swr_init(s->swr) < 0) {
        if (s->swr)    { swr_free(&s->swr); }
        if (s->aCodec) { avcodec_free_context(&s->aCodec); s->aCodec = nullptr; }
        s->aStream = -1;
        return false;
    }

    if (!snd->beginVideoPCMStream(s->aOutRate, s->aOutChannels)) {
        swr_free(&s->swr);
        avcodec_free_context(&s->aCodec);
        s->aCodec = nullptr;
        s->aStream = -1;
        return false;
    }

    s->audioStreamStarted = true;
    VIDEO_TRACE("audio: embedded stream opened, rate=%d ch=%d",
                s->aOutRate, s->aOutChannels);
    return true;
}

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

    // 6. Audio — precedence ladder (Task 14, spec §Audio-source precedence):
    //   1. useWaveFile=true  → prefer sidecar WAV
    //   2. useWaveFile=false → prefer embedded audio
    //   3. if preferred fails, fall back to the other source
    //   4. if both fail, play silent video
    // Note: tryOpenEmbeddedAudio owns av_find_best_stream + decoder open.
    //       s->aStream >= 0 iff embeddedOK. Wall-clock master kicks in
    //       automatically when audioStreamStarted stays false.
    {
        bool sidecarOK  = false;
        bool embeddedOK = false;
        if (p.useWaveFile) {
            sidecarOK = tryStartSidecarWAV(p.waveFileShortName);
            if (!sidecarOK) {
                embeddedOK = tryOpenEmbeddedAudio(s);
                if (!embeddedOK) {
                    VIDEO_LOG("audio: sidecar and embedded both unavailable; silent video");
                }
            }
        } else {
            embeddedOK = tryOpenEmbeddedAudio(s);
            if (!embeddedOK) {
                sidecarOK = tryStartSidecarWAV(p.waveFileShortName);
                if (!sidecarOK) {
                    VIDEO_LOG("audio: embedded and sidecar both unavailable; silent video");
                }
            }
        }
    }

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
    if (s->audioStreamStarted && soundSystem) {
        reinterpret_cast<SoundSystem*>(soundSystem)->endVideoPCMStream();
        s->audioStreamStarted = false;
    }
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

// Path B: consumed-frame counter is a monotonic sample-count in
// the mixer's output rate. Dividing by sample rate yields seconds.
static double audioMasterClock(const VideoSession* s)
{
    if (!soundSystem) return 0.0;
    int frames = reinterpret_cast<SoundSystem*>(soundSystem)->videoPCMSamplesConsumed();
    int rate   = s->aOutRate > 0 ? s->aOutRate : 44100;
    return (double)frames / (double)rate;
}

// Anchor the wall-clock baseline at the current audio PTS so
// wall-clock continues from there — NO backward correction.
static void markAudioStalled(VideoSession* s, const char* reason)
{
    if (!s->audioStallLogged) {
        double lastAudioSec = audioMasterClock(s);
        s->clockStart = nowSeconds() - lastAudioSec;
        s->audioStallLogged = true;
        VIDEO_LOG("audio stalled (%s); master clock fallback to wall-clock", reason);
    }
}

// Prefer audio-master clock when available and not stalled.
// On audio stall: re-anchor wall-clock to the last good audio position
// (one-shot, no recovery, no backward correction).
static double videoMasterClock(const VideoSession* s)
{
    if (s->aCodec && s->aStream >= 0 && s->audioStreamStarted && !s->audioStallLogged) {
        return audioMasterClock(s);
    }
    return nowSeconds() - s->clockStart;
}

//-----------------------------------------------------------------------------
// Decode-one-frame helper — log-once error wrapper
//-----------------------------------------------------------------------------
static int logOnceDecodeError(VideoSession* s, const char* where, int ret)
{
    if (!s->loggedVideoDecodeError) {
        char errbuf[128] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        VIDEO_LOG("decode error at %s: %s (ret=%d)", where, errbuf, ret);
        s->loggedVideoDecodeError = true;
    }
    return -1;
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
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) return logOnceDecodeError(s, "avcodec_receive_frame", ret);

        // Need more input
        ret = av_read_frame(s->fmt, s->pkt);
        if (ret == AVERROR_EOF) {
            avcodec_send_packet(s->vCodec, nullptr);  // flush
            ret = avcodec_receive_frame(s->vCodec, s->vFrame);
            if (ret == 0) return 1;
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                return logOnceDecodeError(s, "avcodec_receive_frame(flush)", ret);
            return -1;
        }
        if (ret < 0) return logOnceDecodeError(s, "av_read_frame", ret);

        if (s->pkt->stream_index == s->vStream) {
            avcodec_send_packet(s->vCodec, s->pkt);
        } else if (s->pkt->stream_index == s->aStream && s->aCodec && s->swr) {
            int sendRet = avcodec_send_packet(s->aCodec, s->pkt);
            if (sendRet < 0 && sendRet != AVERROR_INVALIDDATA) {
                // Unrecoverable send failure — stall the audio clock.
                markAudioStalled(s, "avcodec_send_packet error");
            }
            AVFrame* af = av_frame_alloc();
            int recvRet;
            while ((recvRet = avcodec_receive_frame(s->aCodec, af)) == 0) {
                int outSamplesMax = (int)av_rescale_rnd(
                    swr_get_delay(s->swr, s->aCodec->sample_rate) + af->nb_samples,
                    s->aOutRate, s->aCodec->sample_rate, AV_ROUND_UP);
                int outBytes = av_samples_get_buffer_size(
                    nullptr, s->aOutChannels, outSamplesMax, AV_SAMPLE_FMT_S16, 1);
                uint8_t* outBuf = (uint8_t*)av_malloc(outBytes);
                uint8_t* outPtr = outBuf;
                int outSamples = swr_convert(s->swr, &outPtr, outSamplesMax,
                                             (const uint8_t**)af->data, af->nb_samples);
                if (outSamples < 0) {
                    // swr_convert failure — stall the audio clock.
                    av_free(outBuf);
                    markAudioStalled(s, "swr_convert error");
                    break;
                }
                if (outSamples > 0 && soundSystem) {
                    reinterpret_cast<SoundSystem*>(soundSystem)->pushVideoPCMSamples(
                        (const int16_t*)outBuf, outSamples);
                }
                av_free(outBuf);
            }
            // EAGAIN is normal backpressure; EOF is end-of-stream — neither is a stall.
            av_frame_free(&af);
        }
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

void video_render(VideoSession* s)
{
    if (!s || !s->frameReady || s->texHandle == 0) {
        return;
    }

    // Set render states explicitly each call (mirrors mechicon.cpp / gametacmap.cpp pattern).
    gos_SetRenderState( gos_State_AlphaMode,  gos_Alpha_OneZero );
    gos_SetRenderState( gos_State_Specular,   FALSE );
    gos_SetRenderState( gos_State_AlphaTest,  FALSE );
    gos_SetRenderState( gos_State_Filter,     gos_FilterBiLinear );
    gos_SetRenderState( gos_State_ZWrite,     0 );
    gos_SetRenderState( gos_State_ZCompare,   0 );
    gos_SetRenderState( gos_State_Clipping,   1 );

    // Letterbox/pillarbox fill — draw a black background rect if the
    // video quad does not fully cover MC2Rect.
    const bool needsLetterbox = (s->quadX0 > (float)s->rectX)
                             || (s->quadY0 > (float)s->rectY)
                             || (s->quadX1 < (float)(s->rectX + s->rectW))
                             || (s->quadY1 < (float)(s->rectY + s->rectH));
    if (needsLetterbox) {
        gos_SetRenderState( gos_State_Texture, 0 );   // untextured
        gos_VERTEX bg[4];
        auto fillBg = [](gos_VERTEX& v, float x, float y) {
            v.x = x; v.y = y; v.z = 0.0f; v.rhw = 1.0f;
            v.argb = 0xFF000000;   // opaque black
            v.frgb = 0;
            v.u = 0.0f; v.v = 0.0f;
        };
        fillBg(bg[0], (float)s->rectX,                      (float)s->rectY);
        fillBg(bg[1], (float)(s->rectX + s->rectW),         (float)s->rectY);
        fillBg(bg[2], (float)(s->rectX + s->rectW),         (float)(s->rectY + s->rectH));
        fillBg(bg[3], (float)s->rectX,                      (float)(s->rectY + s->rectH));
        gos_DrawQuads(bg, 4);
    }

    // Textured video quad — full-bright, no modulation.
    gos_SetRenderState( gos_State_Texture, s->texHandle );
    gos_VERTEX vq[4];
    auto fillQ = [](gos_VERTEX& v, float x, float y, float u, float vv) {
        v.x = x; v.y = y; v.z = 0.0f; v.rhw = 1.0f;
        v.argb = 0xFFFFFFFF;
        v.frgb = 0;
        v.u = u; v.v = vv;
    };
    fillQ(vq[0], s->quadX0, s->quadY0, 0.0f, 0.0f);
    fillQ(vq[1], s->quadX1, s->quadY0, 1.0f, 0.0f);
    fillQ(vq[2], s->quadX1, s->quadY1, 1.0f, 1.0f);
    fillQ(vq[3], s->quadX0, s->quadY1, 0.0f, 1.0f);
    gos_DrawQuads(vq, 4);
}
void video_pause(VideoSession* s, bool paused)
{
    if (!s || s->paused == paused) return;
    s->paused = paused;
    if (paused) {
        s->pausedAt = nowSeconds();
        // Pause the audio stream (Path B: end/begin is not symmetric; there
        // is no "pause" on Mix_HookMusic — we leave the callback installed
        // but stop pushing new frames, which drains the ring to silence
        // after ~2 seconds. That's acceptable for pause/resume UX. If a
        // cleaner pause is ever needed, we'd add a paused flag inside
        // the SoundSystem adapter.)
    } else {
        // Resume: shift clock origin forward by pause duration so the
        // wall-clock fallback path doesn't think we lost time during pause.
        double delta = nowSeconds() - s->pausedAt;
        s->clockStart += delta;
        // Spec: up to one video frame of catch-up permitted — which falls
        // out naturally from the pending-frame hold in video_update.
    }
}

void video_stop(VideoSession* s)
{
    if (!s) return;
    s->eof = true;
    // End the audio stream if we started one. Texture is retained
    // until video_close (destructor) so a render() after stop still
    // shows the last frame harmlessly.
    if (s->audioStreamStarted && soundSystem) {
        reinterpret_cast<SoundSystem*>(soundSystem)->endVideoPCMStream();
        s->audioStreamStarted = false;
    }
}

void video_restart(VideoSession* s)
{
    if (!s) return;
    if (s->fmt) {
        av_seek_frame(s->fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
    }
    if (s->vCodec) avcodec_flush_buffers(s->vCodec);
    if (s->aCodec) avcodec_flush_buffers(s->aCodec);
    s->eof = false;
    s->paused = false;
    s->frameReady = false;
    s->pendingFrameValid = false;
    s->presentedPTS = -1.0;
    s->audioStallLogged = false;
    s->loggedDegenerateRect = false;
    s->loggedVideoDecodeError = false;
    s->clockStart = nowSeconds();
    // Re-arm audio push: end the existing stream (if any) and start
    // a fresh one to reset the consumed-frame counter.
    if (s->audioStreamStarted && soundSystem) {
        reinterpret_cast<SoundSystem*>(soundSystem)->endVideoPCMStream();
        s->audioStreamStarted = false;
    }
    if (s->aCodec && soundSystem &&
        reinterpret_cast<SoundSystem*>(soundSystem)->beginVideoPCMStream(s->aOutRate, s->aOutChannels)) {
        s->audioStreamStarted = true;
    }
}

void video_setRect(VideoSession* s, int x0, int y0, int w, int h)
{
    if (!s) return;
    if (w <= 0 || h <= 0) {
        if (!s->loggedDegenerateRect) {
            VIDEO_LOG("setRect: degenerate rect ignored (w=%d h=%d)", w, h);
            s->loggedDegenerateRect = true;
        }
        return;
    }
    s->rectX = x0;
    s->rectY = y0;
    s->rectW = w;
    s->rectH = h;
    computeLetterboxQuad(s->srcW, s->srcH, s->sar,
                         s->rectX, s->rectY, s->rectW, s->rectH,
                         s->quadX0, s->quadY0, s->quadX1, s->quadY1);
}
