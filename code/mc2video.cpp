//=============================================================================
// FFmpeg-backed video decoder — sole translation unit with FFmpeg symbols.
//=============================================================================
#include "mc2_czstring.h"
#include <memory>
#include <string_view>
#include "mc2video.h"
#include "gos_crashbundle.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>   // std::min
#include <bit>
#include <SDL2/SDL_mixer.h>

#include "file.h"   // fileExists
#include "vfs.h"
#include "gameos.hpp"  // gos_NewEmptyTexture, gos_DestroyTexture, gos_Texture_Alpha, RECT_TEX, gosHint_DisableMipmap, DWORD
#include "soundsys.h"  // SoundSystem — PCM push adapter (Task 12)
extern char moviePath[80];  // defined in mclib/paths.cpp
// soundSystem is the global GameSoundSystem* declared in code/gamesound.cpp.
// Forward-declare the subclass so the extern type matches the definition;
// SoundSystem methods are callable via the subclass pointer.
class GameSoundSystem;
extern GameSoundSystem* soundSystem;
extern void startup_phase(std::string_view name);

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
#include "Gates/gate.h"
#include "mc2fmt.h"
#include <format>
#include <string>
#include <string_view>
#include <utility>

//-----------------------------------------------------------------------------
// Availability gate
//-----------------------------------------------------------------------------
bool g_ffmpegAvailable = true;

static const bool s_videoTrace = mc2::gate::get<"MC2_DEBUG_VIDEO">();
template <class... Args>
void VIDEO_TRACE(std::format_string<Args...> fmt, Args&&... args)
{
    if (s_videoTrace) crashbundle_printf<512>("[VIDEO] ", fmt, std::forward<Args>(args)...);
}

template <class... Args>
void VIDEO_LOG(std::format_string<Args...> fmt, Args&&... args)
{
    crashbundle_printf<512>("[VIDEO] ", fmt, std::forward<Args>(args)...);
}

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
            VIDEO_LOG("FFmpeg delay-load failed post-probe (dll={}, notify={:d}). "
                      "This should not happen if the gate is respected.",
                      mc2::fmt::safe(pdli && pdli->szDll ? pdli->szDll : "<unknown>"),
                      mc2::fmt::as_u32(dliNotify));
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
static bool tryLoadOne(mc2::czstring dllName)
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
            VIDEO_LOG("FFmpeg unavailable: LoadLibraryA({}) failed (err={:d}). "
                      "Video playback disabled for this session.", mc2::fmt::safe(d), mc2::fmt::as_u32(err));
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
bool resolveVideoCandidate(mc2::czstring shortName, bool preferUpscaled,
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
        mc2::fmt::to_buf(tmp, "{}{}{}", mc2::fmt::safe(moviePath), mc2::fmt::safe(shortName), mc2::fmt::safe(s.ext));
        // FFmpeg opens native paths, unlike File::open: preserve case and mod precedence.
        const auto entry = mc2::vfs::lookup(tmp);
        if (entry && !entry->physicalPath.empty())
            mc2::fmt::to_buf(tmp, "{}", entry->physicalPath);
        if (s.looseRequired && !fileExists(tmp)) return false;
        if (viableSoFar == index) {
            mc2::fmt::to_buf_n(outPath, outPathSize, "{}", mc2::fmt::safe(tmp));
            VIDEO_TRACE("resolver: viable idx={:d} ext={} path={}{}",
                        mc2::fmt::as_i32(index), mc2::fmt::safe(s.ext),
                        s.looseRequired ? "" : "(FST) ", mc2::fmt::safe(outPath));
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
    VideoObjectiveReceipt objectiveReceipt;
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
    double           videoOrigin = 0.0;
    double           decodedPTS = -1.0;
    double           nextSyncTrace = 0.0;

    // gos texture (Path GL-A)
    DWORD            texHandle = 0;   // 0 == INVALID_TEXTURE_ID (caller must treat as invalid)

    // Draw geometry (screen-space, computed once)
    float            quadX0 = 0, quadY0 = 0, quadX1 = 0, quadY1 = 0;
    int              rectX = 0, rectY = 0, rectW = 0, rectH = 0;
    bool             frameReady = false;

    // Playback clock (audio-master, monotonic wall time for silent clips)
    double           clockStart   = 0.0;
    double           presentedPTS = -1.0;

    // Decoded-but-not-yet-due frame (Task 10 uses these)
    bool             pendingFrameValid = false;
    double           pendingFramePTS   = 0.0;

    // Audio — Task 12
    int              aStream   = -1;
    AVCodecContext*  aCodec    = nullptr;
    AVFormatContext* aFmt      = nullptr;
    AVFrame*         aFrame    = nullptr;
    AVPacket*        aPkt      = nullptr;
    SwrContext*      swr       = nullptr;
    int              aOutRate     = 0;
    int              aOutChannels = 0;
    bool             audioStreamStarted = false;
    bool             audioEOF = false;
    bool             audioFlushing = false;
    bool             audioSidecar = false;
    int64_t          audioFramesQueued = 0;
    double           audioOrigin = 0.0;
    double           audioTailWall = -1.0;

    // Audio-master clock — Task 13
    bool             audioStallLogged = false;

    // Failure-handling log-once flags — Task 16
    bool             loggedDegenerateRect     = false;
    bool             loggedVideoDecodeError   = false;
    bool             loggedPresented = false;

    // Lifecycle
    bool             paused   = false;
    bool             eof      = false;
    double           pausedAt = 0.0;
};

//-----------------------------------------------------------------------------
// Audio-source helpers (Task 14)
//-----------------------------------------------------------------------------

// Sidecars use the same decoder/resampler and consumed-sample clock as
// embedded audio. Missing or unsupported WAVs fall back to embedded audio.
extern char soundPath[80];   // mclib/paths.cpp
static bool tryOpenAudio(VideoSession* s, std::string_view path);
static bool tryStartSidecarWAV(VideoSession* s, mc2::czstring waveShortName)
{
    if (!soundSystem || !waveShortName || !waveShortName[0]) return false;
    char wavPath[1024];
    mc2::fmt::to_buf(wavPath, "{}{}.wav", mc2::fmt::safe(soundPath), mc2::fmt::safe(waveShortName));
    if (!fileExists(wavPath)) return false;
    if (!tryOpenAudio(s, wavPath)) return false;
    s->audioSidecar = true;
    return true;
}

// Returns true iff an audio stream was found, its decoder
// opened, SwrContext was initialised, and beginVideoPCMStream succeeded.
// On any failure, partial state (aCodec/swr) is torn down cleanly and
// s->aStream is reset to -1.
static bool tryOpenAudio(VideoSession* s, std::string_view path)
{
    // Audio must decode ahead even while a future video frame is held. A separate
    // demux cursor also puts WAV sidecars on the same consumed-sample clock.
    const std::string inputPath(path); // FFmpeg requires a NUL-terminated path.
    if (avformat_open_input(&s->aFmt, inputPath.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(s->aFmt, nullptr) < 0) {
        avformat_close_input(&s->aFmt);
        return false;
    }
    s->aStream = av_find_best_stream(s->aFmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (s->aStream < 0) { avformat_close_input(&s->aFmt); return false; }

    AVStream* ast = s->aFmt->streams[s->aStream];
    const AVCodec* ac = avcodec_find_decoder(ast->codecpar->codec_id);
    if (!ac) { avformat_close_input(&s->aFmt); s->aStream = -1; return false; }

    s->aCodec = avcodec_alloc_context3(ac);
    avcodec_parameters_to_context(s->aCodec, ast->codecpar);

    SoundSystem* snd = reinterpret_cast<SoundSystem*>(soundSystem);
    if (avcodec_open2(s->aCodec, ac, nullptr) != 0 ||
        !snd ||
        !snd->queryNativeFormat(&s->aOutRate, &s->aOutChannels))
    {
        if (s->aCodec) { avcodec_free_context(&s->aCodec); s->aCodec = nullptr; }
        avformat_close_input(&s->aFmt);
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
        avformat_close_input(&s->aFmt);
        s->aStream = -1;
        return false;
    }

    if (!snd->beginVideoPCMStream(s->aOutRate, s->aOutChannels)) {
        swr_free(&s->swr);
        avcodec_free_context(&s->aCodec);
        s->aCodec = nullptr;
        avformat_close_input(&s->aFmt);
        s->aStream = -1;
        return false;
    }

    s->audioStreamStarted = true;
    s->aFrame = av_frame_alloc();
    s->aPkt = av_packet_alloc();
    VIDEO_TRACE("speech start: source={}", path);
    VIDEO_TRACE("audio: stream opened, rate={:d} ch={:d}",
                mc2::fmt::as_i32(s->aOutRate), mc2::fmt::as_i32(s->aOutChannels));
    return true;
}

static void computeLetterboxQuad(int srcW, int srcH, double sar,
                                 int rectX, int rectY, int rectW, int rectH,
                                 float& x0, float& y0, float& x1, float& y1)
{
    if (rectW <= 0 || rectH <= 0 || srcW <= 0 || srcH <= 0) {
        x0 = static_cast<float>(rectX); y0 = static_cast<float>(rectY);
        x1 = static_cast<float>((rectX + rectW)); y1 = static_cast<float>((rectY + rectH));
        return;
    }
    double srcAspectW = static_cast<double>(srcW) * sar;
    double scale = std::min(static_cast<double>(rectW) / srcAspectW, static_cast<double>(rectH) / srcH);
    double quadW = srcAspectW * scale;
    double quadH = srcH * scale;
    double cx = rectX + rectW * 0.5;
    double cy = rectY + rectH * 0.5;
    x0 = static_cast<float>((cx - quadW * 0.5));
    y0 = static_cast<float>((cy - quadH * 0.5));
    x1 = static_cast<float>((cx + quadW * 0.5));
    y1 = static_cast<float>((cy + quadH * 0.5));
}

VideoSession* video_open(const VideoOpenParams& p, VideoOpenResult* out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!g_ffmpegAvailable || !p.resolvedPath) return nullptr;

    auto sessionOwner = std::make_unique<VideoSession>();
    auto* s = sessionOwner.get();

    s->objectiveReceipt.capture(p.resolvedPath);

    if (avformat_open_input(&s->fmt, p.resolvedPath, nullptr, nullptr) < 0) {
        VIDEO_LOG("avformat_open_input failed for '{}'", mc2::fmt::safe(p.resolvedPath));
        video_close(sessionOwner.release()); return nullptr;
    }
    if (avformat_find_stream_info(s->fmt, nullptr) < 0) {
        VIDEO_LOG("avformat_find_stream_info failed for '{}'", mc2::fmt::safe(p.resolvedPath));
        video_close(sessionOwner.release()); return nullptr;
    }

    s->vStream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (s->vStream < 0) {
        VIDEO_LOG("no video stream in '{}'", mc2::fmt::safe(p.resolvedPath));
        video_close(sessionOwner.release()); return nullptr;
    }
    AVStream* vst = s->fmt->streams[s->vStream];
    const AVCodec* vcodec = avcodec_find_decoder(vst->codecpar->codec_id);
    if (!vcodec) { video_close(sessionOwner.release()); return nullptr; }
    s->vCodec = avcodec_alloc_context3(vcodec);
    avcodec_parameters_to_context(s->vCodec, vst->codecpar);
    if (avcodec_open2(s->vCodec, vcodec, nullptr) < 0) {
        VIDEO_LOG("video decoder open failed");
        video_close(sessionOwner.release()); return nullptr;
    }

    s->srcW = s->vCodec->width;
    s->srcH = s->vCodec->height;
    s->sar  = (vst->sample_aspect_ratio.num && vst->sample_aspect_ratio.den)
              ? av_q2d(vst->sample_aspect_ratio) : 1.0;
    const AVRational frameRate = av_guess_frame_rate(s->fmt, vst, nullptr);
    s->fps = frameRate.num > 0 && frameRate.den > 0 ? av_q2d(frameRate) : 30.0;
    s->timeBase = av_q2d(vst->time_base);
    s->videoOrigin = vst->start_time == AV_NOPTS_VALUE ? 0.0 : vst->start_time * s->timeBase;

    // BGRA matches the destination texture format.
    s->sws = sws_getContext(s->srcW, s->srcH, s->vCodec->pix_fmt,
                             s->srcW, s->srcH, AV_PIX_FMT_BGRA,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!s->sws) { video_close(sessionOwner.release()); return nullptr; }

    s->vFrame = av_frame_alloc();
    s->pkt    = av_packet_alloc();

    s->texHandle = gos_NewEmptyTexture(gos_Texture_Alpha,
                                        "mc2video",
                                        RECT_TEX(static_cast<DWORD>(s->srcW), static_cast<DWORD>(s->srcH)),
                                        gosHint_DisableMipmap);
    if (s->texHandle == 0) {
        VIDEO_LOG("gos_NewEmptyTexture failed {}x{}", mc2::fmt::as_i32(s->srcW), mc2::fmt::as_i32(s->srcH));
        video_close(sessionOwner.release()); return nullptr;
    }

    // Briefing panels and pilot cameras require the destination origin.
    s->rectX = p.destRectX;
    s->rectY = p.destRectY;
    s->rectW = p.destRectW;
    s->rectH = p.destRectH;
    computeLetterboxQuad(s->srcW, s->srcH, s->sar,
                         s->rectX, s->rectY, s->rectW, s->rectH,
                         s->quadX0, s->quadY0, s->quadX1, s->quadY1);

    // Prefer the requested audio source; silent videos use the wall clock.
    {
        bool sidecarOK  = false;
        bool embeddedOK = false;
        if (p.useWaveFile) {
            sidecarOK = tryStartSidecarWAV(s, p.waveFileShortName);
            if (!sidecarOK) {
                embeddedOK = tryOpenAudio(s, p.resolvedPath);
                if (!embeddedOK) {
                    VIDEO_LOG("audio: sidecar and embedded both unavailable; silent video");
                }
            }
        } else {
            embeddedOK = tryOpenAudio(s, p.resolvedPath);
            if (!embeddedOK) {
                sidecarOK = tryStartSidecarWAV(s, p.waveFileShortName);
                if (!sidecarOK) {
                    VIDEO_LOG("audio: embedded and sidecar both unavailable; silent video");
                }
            }
        }
    }

    if (out) {
        out->srcW = s->srcW; out->srcH = s->srcH;
        out->sar  = s->sar;  out->fps  = s->fps;
        out->hasAudio = (s->aStream >= 0);
        out->hasAlpha = (s->vCodec->pix_fmt == AV_PIX_FMT_YUVA420P ||
                         s->vCodec->pix_fmt == AV_PIX_FMT_ARGB ||
                         s->vCodec->pix_fmt == AV_PIX_FMT_RGBA ||
                         s->vCodec->pix_fmt == AV_PIX_FMT_BGRA);
        out->glTextureId = 0;
        out->quadX0 = s->quadX0; out->quadY0 = s->quadY0;
        out->quadX1 = s->quadX1; out->quadY1 = s->quadY1;
    }

    VIDEO_LOG("init: opened '{}' ({}x{}, sar={:.3f}, fps={:.2f}, audio={:d})",
              mc2::fmt::safe(p.resolvedPath), mc2::fmt::as_i32(s->srcW), mc2::fmt::as_i32(s->srcH), s->sar, s->fps, mc2::fmt::as_i32(static_cast<int>((s->aStream >= 0))));

    s->clockStart   = static_cast<double>(av_gettime_relative()) / 1000000.0;
    s->presentedPTS = -1.0;

    return sessionOwner.release();
}

void video_close(VideoSession* s)
{
    std::unique_ptr<VideoSession> sessionOwner(s);
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
    if (s->aFrame)  { av_frame_free(&s->aFrame); }
    if (s->aPkt)    { av_packet_free(&s->aPkt); }
    if (s->aFmt)   { avformat_close_input(&s->aFmt); }
    if (s->swr)     { swr_free(&s->swr); }
    if (s->fmt)     { avformat_close_input(&s->fmt); }
    sessionOwner.reset();
}

//-----------------------------------------------------------------------------
// Master-clock helpers
//-----------------------------------------------------------------------------
static double nowSeconds()
{
    return static_cast<double>(av_gettime_relative()) / 1000000.0;
}

// Path B: consumed-frame counter is a monotonic sample-count in
// the mixer's output rate. Dividing by sample rate yields seconds.
static double audioMasterClock(const VideoSession* s)
{
    if (!soundSystem) return 0.0;
    int frames = reinterpret_cast<SoundSystem*>(soundSystem)->videoPCMSamplesConsumed();
    int rate   = s->aOutRate > 0 ? s->aOutRate : 44100;
    return s->audioOrigin + static_cast<double>(frames) / static_cast<double>(rate);
}

// Anchor the wall-clock baseline at the current audio PTS so
// wall-clock continues from there — NO backward correction.
static void markAudioStalled(VideoSession* s, mc2::czstring reason)
{
    if (!s->audioStallLogged) {
        double lastAudioSec = audioMasterClock(s);
        s->clockStart = nowSeconds() - lastAudioSec;
        s->audioStallLogged = true;
        VIDEO_LOG("audio stalled ({}); master clock fallback to wall-clock", mc2::fmt::safe(reason));
    }
}

// Prefer audio-master clock when available and not stalled.
// On audio stall: re-anchor wall-clock to the last good audio position
// (one-shot, no recovery, no backward correction).
static double videoMasterClock(VideoSession* s)
{
    if (s->aCodec && s->aStream >= 0 && s->audioStreamStarted && !s->audioStallLogged) {
        double audio = audioMasterClock(s);
        // A video tail can outlast its audio. Only run it on wall time after
        // the decoder AND the PCM queue have drained, never on an underrun.
        if (s->audioEOF && reinterpret_cast<SoundSystem*>(soundSystem)->videoPCMSamplesConsumed() >= s->audioFramesQueued) {
            if (s->audioTailWall < 0.0) s->audioTailWall = nowSeconds();
            return audio + nowSeconds() - s->audioTailWall;
        }
        return audio;
    }
    return nowSeconds() - s->clockStart;
}

//-----------------------------------------------------------------------------
// Decode-one-frame helper — log-once error wrapper
//-----------------------------------------------------------------------------
static int logOnceDecodeError(VideoSession* s, mc2::czstring where, int ret)
{
    if (!s->loggedVideoDecodeError) {
        char errbuf[128] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        VIDEO_LOG("decode error at {}: {} (ret={:d})", mc2::fmt::safe(where), mc2::fmt::safe(errbuf), mc2::fmt::as_i32(ret));
        s->loggedVideoDecodeError = true;
    }
    return -1;
}

//-----------------------------------------------------------------------------
// Decode-one-frame helper
// Pulls packets and decodes until one video frame is produced in
// s->vFrame, or EOF. Returns: 1 = frame produced, -1 = EOF/error.
// Audio has its own demux cursor, so holding video cannot starve the mixer.
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
        }
        av_packet_unref(s->pkt);
    }
}

// Keep half a second queued, independently of video presentation or the sim tick.
static void bufferAudio(VideoSession* s)
{
    if (!s->audioStreamStarted || s->audioEOF || s->audioStallLogged) return;
    auto* snd = reinterpret_cast<SoundSystem*>(soundSystem);
    while (s->audioFramesQueued - snd->videoPCMSamplesConsumed() < s->aOutRate / 2) {
        int ret = avcodec_receive_frame(s->aCodec, s->aFrame);
        bool draining = ret == AVERROR_EOF;
        if (ret == 0 || draining) {
            auto* af = s->aFrame;
            if (ret == 0 && s->audioFramesQueued == 0 && af->best_effort_timestamp != AV_NOPTS_VALUE) {
                s->audioOrigin = af->best_effort_timestamp * av_q2d(s->aFmt->streams[s->aStream]->time_base);
                if (!s->audioSidecar) s->audioOrigin -= s->videoOrigin;
            }
            int count = draining ? 0 : af->nb_samples;
            int capacity = static_cast<int>(av_rescale_rnd(
                swr_get_delay(s->swr, s->aCodec->sample_rate) + count,
                s->aOutRate, s->aCodec->sample_rate, AV_ROUND_UP));
            if (capacity <= 0) { s->audioEOF = draining; if (draining) return; continue; }
            uint8_t* pcm = static_cast<uint8_t*>(av_malloc(capacity * s->aOutChannels * sizeof(int16_t)));
            int frames = swr_convert(s->swr, &pcm, capacity,
                draining ? nullptr : const_cast<const uint8_t**>(af->extended_data), count);
            if (frames < 0) { av_free(pcm); markAudioStalled(s, "swr_convert error"); return; }
            if (s->audioSidecar) {
                const float gain = (snd->getDigitalMasterVolume() / 256.0f) * (snd->getBettyVolume() / 256.0f);
                auto* samples = std::bit_cast<int16_t*>(pcm);
                for (int i = 0; i < frames * s->aOutChannels; ++i)
                    samples[i] = static_cast<int16_t>(samples[i] * gain);
            }
            snd->pushVideoPCMSamples(reinterpret_cast<int16_t*>(pcm), frames);
            if (frames > 0 && s->audioFramesQueued == 0)
                startup_phase("first_audio_queued");
            s->audioFramesQueued += frames;
            av_free(pcm);
            if (draining && frames == 0) { s->audioEOF = true; return; }
            continue;
        }
        if (ret != AVERROR(EAGAIN)) { markAudioStalled(s, "audio decode error"); return; }
        do {
            av_packet_unref(s->aPkt);
            ret = av_read_frame(s->aFmt, s->aPkt);
        } while (ret >= 0 && s->aPkt->stream_index != s->aStream);
        if (ret == AVERROR_EOF && !s->audioFlushing) {
            avcodec_send_packet(s->aCodec, nullptr);
            s->audioFlushing = true;
        } else if (ret < 0) {
            markAudioStalled(s, "audio demux error"); return;
        } else {
            ret = avcodec_send_packet(s->aCodec, s->aPkt);
            av_packet_unref(s->aPkt);
            if (ret < 0) { markAudioStalled(s, "audio packet error"); return; }
        }
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
    uint8_t* dst[4]     = { reinterpret_cast<uint8_t*>(td.pTexture), nullptr, nullptr, nullptr };
    int      dstLine[4] = { static_cast<int>((td.Width * 4)), 0, 0, 0 };
    sws_scale(s->sws,
              s->vFrame->data, s->vFrame->linesize,
              0, s->srcH,
              dst, dstLine);

    gos_UnLockTexture(s->texHandle);
    s->frameReady = true;
    if (s_videoTrace && s->decodedPTS >= s->nextSyncTrace) {
        double audioPTS = videoMasterClock(s);
        VIDEO_TRACE("sync: video_pts={:.6f} audio_pts={:.6f} offset_ms={:.3f}",
                    s->decodedPTS, audioPTS, (s->decodedPTS - audioPTS) * 1000.0);
        s->nextSyncTrace = s->decodedPTS + 1.0;
    }
}

//-----------------------------------------------------------------------------
// video_update — decode loop with frame-hold for future PTS + late-frame drop
//-----------------------------------------------------------------------------
bool video_update(VideoSession* s)
{
    if (!s || s->eof) return true;
    if (s->paused) return false;

    bufferAudio(s);
    double masterNow = videoMasterClock(s);

    // Future frames remain pending until their presentation time.
    if (s->pendingFrameValid) {
        if (s->pendingFramePTS > masterNow) {
            return false;
        }
        if (masterNow - s->pendingFramePTS <= 2.0 / s->fps) {
            uploadFrameToTexture(s);
            s->presentedPTS = s->pendingFramePTS;
        }
        s->pendingFrameValid = false;
    }

    // Decode ahead for presentation; severely late frames need no upload.
    for (;;) {
        int r = decodeNextVideoFrame(s);
        if (r < 0) {
            // Keep the final picture until its duration and the audio tail finish.
            if (masterNow < s->decodedPTS + 1.0 / s->fps ||
                (s->audioStreamStarted && !s->audioStallLogged &&
                 (!s->audioEOF || reinterpret_cast<SoundSystem*>(soundSystem)->videoPCMSamplesConsumed() < s->audioFramesQueued)))
                return false;
            s->eof = true;
            return true;
        }
        if (s->decodedPTS < 0.0)
            VIDEO_LOG("first_frame_decoded path={}", mc2::fmt::safe(s->fmt->url));
        s->objectiveReceipt.decoded();

        double pts = (s->vFrame->best_effort_timestamp == AV_NOPTS_VALUE)
                     ? (s->decodedPTS < 0.0 ? 0.0 : s->decodedPTS + 1.0 / s->fps)
                     : s->vFrame->best_effort_timestamp * s->timeBase - s->videoOrigin;
        s->decodedPTS = pts;
        masterNow = videoMasterClock(s);

        if (pts > masterNow) {
            s->pendingFrameValid = true;
            s->pendingFramePTS   = pts;
            return false;
        }

        // Severely late frames would prolong catch-up without improving playback.
        const double lateThreshold = 2.0 / (s->fps > 0 ? s->fps : 30.0);
        if (masterNow - pts > lateThreshold) {
            continue;
        }

        uploadFrameToTexture(s);
        s->presentedPTS = pts;
    }
}

unsigned int video_get_texture(VideoSession* s)
{
    if (!s || !s->frameReady || s->texHandle == 0)
        return 0;
    return static_cast<unsigned int>(s->texHandle);
}

bool video_get_layout(VideoSession* s,
                      float* qx0, float* qy0, float* qx1, float* qy1,
                      int* rx, int* ry, int* rw, int* rh)
{
    if (!s)
        return false;
    if (qx0) *qx0 = s->quadX0;
    if (qy0) *qy0 = s->quadY0;
    if (qx1) *qx1 = s->quadX1;
    if (qy1) *qy1 = s->quadY1;
    if (rx)  *rx  = s->rectX;
    if (ry)  *ry  = s->rectY;
    if (rw)  *rw  = s->rectW;
    if (rh)  *rh  = s->rectH;
    return true;
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
    const bool needsLetterbox = (s->quadX0 > static_cast<float>(s->rectX))
                             || (s->quadY0 > static_cast<float>(s->rectY))
                             || (s->quadX1 < static_cast<float>((s->rectX + s->rectW)))
                             || (s->quadY1 < static_cast<float>((s->rectY + s->rectH)));
    if (needsLetterbox) {
        gos_SetRenderState( gos_State_Texture, 0 );   // untextured
        gos_VERTEX bg[4];
        auto fillBg = [](gos_VERTEX& v, float x, float y) {
            v.x = x; v.y = y; v.z = 0.0f; v.rhw = 1.0f;
            v.argb = 0xFF000000;   // opaque black
            v.frgb = 0;
            v.u = 0.0f; v.v = 0.0f;
        };
        fillBg(bg[0], static_cast<float>(s->rectX),                      static_cast<float>(s->rectY));
        fillBg(bg[1], static_cast<float>((s->rectX + s->rectW)),         static_cast<float>(s->rectY));
        fillBg(bg[2], static_cast<float>((s->rectX + s->rectW)),         static_cast<float>((s->rectY + s->rectH)));
        fillBg(bg[3], static_cast<float>(s->rectX),                      static_cast<float>((s->rectY + s->rectH)));
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
    if (!s->loggedPresented) {
        VIDEO_LOG("first_frame_presented path={}", mc2::fmt::safe(s->fmt->url));
        s->loggedPresented = true;
    }
}
void video_pause(VideoSession* s, bool paused)
{
    if (!s || s->paused == paused) return;
    s->paused = paused;
    if (paused) {
        s->pausedAt = nowSeconds();
        if (s->audioStreamStarted) Mix_PauseMusic();
    } else {
        if (s->audioStreamStarted) Mix_ResumeMusic();
        if (s->audioTailWall >= 0.0) s->audioTailWall += nowSeconds() - s->pausedAt;
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
    if (s->aFmt) av_seek_frame(s->aFmt, -1, 0, AVSEEK_FLAG_BACKWARD);
    if (s->aCodec) avcodec_flush_buffers(s->aCodec);
    if (s->swr) { swr_close(s->swr); swr_init(s->swr); }
    s->decodedPTS = -1.0;
    s->nextSyncTrace = 0.0;
    s->audioFramesQueued = 0;
    s->audioOrigin = 0.0;
    s->audioTailWall = -1.0;
    s->audioEOF = s->audioFlushing = false;
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
            VIDEO_LOG("setRect: degenerate rect ignored (w={:d} h={:d})", mc2::fmt::as_i32(w), mc2::fmt::as_i32(h));
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
