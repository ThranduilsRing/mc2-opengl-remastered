//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MC2MOVIE_H
#include"mc2movie.h"
#endif

#ifndef TXMMGR_H
#include"txmmgr.h"
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

#include"mc2video.h"

#include "../resource.h"
#include"gameos.hpp"

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
    if (waveName)  { delete[] waveName;  waveName  = nullptr; }
    if (m_MC2Name) { delete[] m_MC2Name; m_MC2Name = nullptr; }
}

//-----------------------------------------------------------------------
// Class MC2Movie
void MC2Movie::init (const char *MC2Name, RECT mRect, bool useWaveFile)
{
    // Copy the short-name from the full path.
    char shortName[1024];
    _splitpath(MC2Name, NULL, NULL, shortName, NULL);

    m_MC2Name = new char[strlen(shortName) + 1];
    strcpy(m_MC2Name, shortName);

    MC2Rect       = mRect;
    forceStop     = false;
    stillPlaying  = false;
    m_session     = nullptr;
    m_sessionDone = false;

    separateWAVE = useWaveFile && (prefs.DigitalMasterVolume != 0.0f);
    if (separateWAVE) {
        waveName = new char[strlen(shortName) + 1];
        strcpy(waveName, shortName);
    } else {
        waveName = nullptr;
    }

    if (!g_ffmpegAvailable) {
        printf("[VIDEO] init: FFmpeg unavailable, skipping movie '%s'\n", shortName);
        stillPlaying  = false;
        m_sessionDone = true;
        return;
    }

    // Enumerate every candidate in the resolver's chain until one
    // opens successfully. Each failure fully tears down the failed
    // decoder state before the next attempt.
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
        p.waveFileShortName = waveName;

        VideoOpenResult r = {};
        m_session = video_open(p, &r);
        if (m_session) {
            stillPlaying = true;
            printf("[VIDEO] init: opened %s (%dx%d, fps=%.2f, audio=%d)\n",
                   resolvedPath, r.srcW, r.srcH, r.fps, (int)r.hasAudio);
            return;
        }

        printf("[VIDEO] init: open failed for '%s' (idx=%d), trying next candidate\n",
               resolvedPath, candidateIndex);
    }

    // All candidates exhausted.
    printf("[VIDEO] init: no playable candidate for '%s'\n", shortName);
    stillPlaying  = false;
    m_sessionDone = true;
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
