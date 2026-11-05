//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MC2MOVIE_H
#include"mc2movie.h"
#endif

#ifndef MC2_RENDER_SUBMISSIONS_H
#include"render_submissions.h"
#endif

#ifndef FILE_H
#include"file.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef PREFS_H
#include"prefs.h"
#endif

#include "mc2_czstring.h"
#include"mc2video.h"
#include "../GameOS/gameos/gos_smoke.h"
#include <cstdlib>

#include "../resource.h"
#include"gameos.hpp"
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "objective.h"

void VideoObjectiveReceipt::capture(mc2::czstring resolvedPath)
{
    if (auto* observation = objectiveObservation(); observation && observation->dispatch) {
        cause = observation->dispatch;
        missionHash = observation->missionHash;
        path = resolvedPath;
    }
}

void VideoObjectiveReceipt::decoded()
{
    if (!cause) { return; }
    if (auto* observation = objectiveObservation(); observation && observation->missionHash == missionHash) {
        observation->SinkResult("movie", "decoded_frame", "accepted", objectiveObservationQuote(path), cause);
    }
    cause = 0;
}

// Frame-rate averaging globals referenced by mechcmd2.cpp.
// Defined here to keep them co-located with the movie subsystem.
float averageFrameRate = 0.0f;
long  currentFrameNum  = 0;
float last30Frames[30] = {
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 0.0f, 0.0f, 0.0f
};

//-----------------------------------------------------------------------
MC2Movie::~MC2Movie (void)
{
    if (m_session) {
        video_close(m_session);
        m_session = nullptr;
    }
    if (waveName)  { waveName.reset();  waveName  = nullptr; }
    if (m_MC2Name) { m_MC2Name.reset(); m_MC2Name = nullptr; }
}

//-----------------------------------------------------------------------
// Class MC2Movie
void MC2Movie::init (mc2::czstring MC2Name, RECT mRect, bool useWaveFile)
{
    char shortName[1024];
    _splitpath(MC2Name, nullptr, nullptr, shortName, nullptr);

    m_MC2Name = std::make_unique_for_overwrite<char[]>(strlen(shortName) + 1);
    strcpy(m_MC2Name.get(), shortName);

    MC2Rect       = mRect;
    forceStop     = false;
    stillPlaying  = false;
    m_session     = nullptr;
    m_sessionDone = false;

    separateWAVE = useWaveFile && (prefs.DigitalMasterVolume != 0.0f);
    if (separateWAVE) {
        waveName = std::make_unique_for_overwrite<char[]>(strlen(shortName) + 1);
        strcpy(waveName.get(), shortName);
    } else {
        waveName = nullptr;
    }

    if (SmokeMode::state().enabled) {
        // Video capture can opt into decoding during a headless run.
        static const bool s_bootVideo = mc2::gate::get<"MC2_BOOT_VIDEO">();
        if (!s_bootVideo) {
            mc2::fmt::print_out("[VIDEO] init: smoke mode active, skipping movie '{}'\n", mc2::fmt::safe(shortName));
            stillPlaying  = false;
            m_sessionDone = true;
			objectiveObservationSink("movie", "decoder", "smoke_skip", MC2Name);
            return;
        }
        mc2::fmt::print_out("[BOOT_VIDEO] smoke-mode movie skip overridden for '{}'\n", mc2::fmt::safe(shortName));
        fflush(stdout);
    }

    if (!g_ffmpegAvailable) {
        mc2::fmt::print_out("[VIDEO] init: FFmpeg unavailable, skipping movie '{}'\n", mc2::fmt::safe(shortName));
        stillPlaying  = false;
        m_sessionDone = true;
		objectiveObservationSink("movie", "decoder", "unavailable", MC2Name);
        return;
    }

    // Decoder failure permits the next resolver candidate.
    const bool preferUpscaled = prefs.UseUpscaledVideos;
    for (int candidateIndex = 0; ; ++candidateIndex) {
        char resolvedPath[1024];
        if (!resolveVideoCandidate(shortName, preferUpscaled, candidateIndex,
                                   resolvedPath, sizeof(resolvedPath))) {
            break;  // chain exhausted
        }

        VideoOpenParams p = {};
        p.resolvedPath      = resolvedPath;
        p.destRectX         = MC2Rect.left;
        p.destRectY         = MC2Rect.top;
        p.destRectW         = MC2Rect.right  - MC2Rect.left;
        p.destRectH         = MC2Rect.bottom - MC2Rect.top;
        p.useWaveFile       = separateWAVE;
        p.waveFileShortName = waveName.get();

        VideoOpenResult r = {};
        m_session = video_open(p, &r);
        if (m_session) {
            stillPlaying = true;
			objectiveObservationSink("movie", "decoder", "accepted", resolvedPath);
            mc2::fmt::print_out("[VIDEO] init: opened {} ({:d}x{:d}, fps={:.2f}, audio={:d})\n", mc2::fmt::safe(resolvedPath), mc2::fmt::as_i32(r.srcW), mc2::fmt::as_i32(r.srcH), r.fps, mc2::fmt::as_i32(static_cast<int>(r.hasAudio)));
            return;
        }

        mc2::fmt::print_out("[VIDEO] init: open failed for '{}' (idx={:d}), trying next candidate\n", mc2::fmt::safe(resolvedPath), mc2::fmt::as_i32(candidateIndex));
    }

    mc2::fmt::print_out("[VIDEO] init: no playable candidate for '{}'\n", mc2::fmt::safe(shortName));
    stillPlaying  = false;
    m_sessionDone = true;
	objectiveObservationSink("movie", "decoder", "rejected", MC2Name);
}

//-----------------------------------------------------------------------
//Changes rect.
void MC2Movie::setRect (RECT vRect)
{
    MC2Rect = vRect;
    if (m_session) {
        video_setRect(m_session,
                      vRect.left, vRect.top,
                      vRect.right  - vRect.left,
                      vRect.bottom - vRect.top);
    }
}

//-----------------------------------------------------------------------
//Pause video playback.
void MC2Movie::pause (bool pauseState)
{
    if (m_session) video_pause(m_session, pauseState);
}

//-----------------------------------------------------------------------
//Immediately stops playback of MC2.
void MC2Movie::stop (void)
{
    forceStop = true;
    if (m_session) video_stop(m_session);
}

//-----------------------------------------------------------------------
//Restarts MC2 from beginning.  Can be called anytime.
void MC2Movie::restart (void)
{
    if (m_session) video_restart(m_session);
    forceStop     = false;
    stillPlaying  = true;
    m_sessionDone = false;
}

//-----------------------------------------------------------------------
//Handles tickling MC2 to make sure we keep playing back
bool MC2Movie::update (void)
{
    if (!stillPlaying || m_sessionDone) {
        return true;
    }

    if (forceStop) {
        if (m_session) video_stop(m_session);
        stillPlaying  = false;
        m_sessionDone = true;
        return true;
    }

    if (m_session && video_update(m_session)) {
        stillPlaying  = false;
        m_sessionDone = true;
        return true;
    }

    return false;
}

//-----------------------------------------------------------------------
//Actually draws the MC2 texture using gos_DrawTriangle.
void MC2Movie::render (void)
{
    if (m_session) video_render(m_session);
}

unsigned int MC2Movie::getTextureHandle (void)
{
    return m_session ? video_get_texture(m_session) : 0;
}

bool MC2Movie::getLayout (float& qx0, float& qy0, float& qx1, float& qy1,
                          long& rx, long& ry, long& rw, long& rh)
{
    int irx = 0, iry = 0, irw = 0, irh = 0;
    if (!video_get_layout(m_session, &qx0, &qy0, &qx1, &qy1,
                          &irx, &iry, &irw, &irh))
        return false;
    rx = irx; ry = iry; rw = irw; rh = irh;
    return true;
}
