//------------------------------------------------------------------
//
// Movie class
//
//Notes:
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MC2MOVIE_H
#define MC2MOVIE_H

//--------------------------------------------------------------------------
#ifndef DSTD_H
#include"dstd.h"
#endif

#ifndef HEAP_H
#include"heap.h"
#endif

#include"platform_windows.h"

struct VideoSession;  // forward decl from mc2video.h

//--------------------------------------------------------------------------
class MC2Movie
{
	public:
		MC2Movie (void)
		{
			MC2Rect.bottom = MC2Rect.top = MC2Rect.left = MC2Rect.right = 0;

			forceStop    = false;
			stillPlaying = false;
			separateWAVE = false;

			waveName     = nullptr;
			m_MC2Name    = nullptr;
			m_session    = nullptr;
			m_sessionDone = false;
		}

		~MC2Movie (void);

		//Movie name assumes path is correct.
		// Sets up the MC2 to be played.
		void init (const char *MC2Name, RECT mRect, bool useWaveFile);

		//Handles tickling MC2 to make sure we keep playing back
		// Returns true when MC2 is DONE playing!!
		bool update (void);

		//Actually draws the MC2 texture using gos_DrawTriangle.
		void render (void);

		//Immediately stops playback of MC2.
		void stop (void);

		//Pause video playback.
		void pause (bool pauseState);

		//Restarts MC2 from beginning.  Can be called anytime.
		void restart (void);

		//Changes rect.
		void setRect (RECT vRect);

		bool isPlaying (void)
		{
			return stillPlaying;
		}

		char *getMovieName (void)
		{
			return m_MC2Name;
		}

		// gos texture handle (0 == none) of the current decoded frame, for callers
		// that composite the video themselves (e.g. the defs UI layer) instead of
		// the built-in gos render().
		unsigned int getTextureHandle (void);

		// SMALLPANELS-IMGUI-SLICE-1: destination geometry of the built-in draw
		// (aspect-fit quad + letterbox dest rect, screen space) so an external
		// compositor can reproduce render() exactly. False when no session.
		bool getLayout (float& qx0, float& qy0, float& qx1, float& qy1,
						long& rx, long& ry, long& rw, long& rh);

	protected:

		RECT		MC2Rect;			//Physical Location on screen for MC2 movie.
		bool		forceStop;			//Should MC2 movie end now?
		bool		stillPlaying;		//Is MC2 movie over?
		bool		separateWAVE;		//Tells us if this MC2 movie has a separate soundtrack.
		char		*waveName;			//Name of the wavefile.
		char		*m_MC2Name;			//Name of the Movie.
		VideoSession*	m_session;
		bool		m_sessionDone;
};

using MC2MoviePtr = MC2Movie *;
//--------------------------------------------------------------------------
#endif
