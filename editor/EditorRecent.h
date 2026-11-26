//-------------------------------------------------------------------------------------------------
// EditorRecent.h -- most-recently-used mission list for the editor startup dialog.
//
// Persisted to editor_recent.txt beside the editor executable. One path per
// line, newest first, capped. Push() on every successful mission load; the startup
// dialog lists them so the user doesn't re-navigate a file dialog each time.
//-------------------------------------------------------------------------------------------------
#ifndef EDITOR_RECENT_H
#define EDITOR_RECENT_H

namespace EditorRecent
{
	static const int kMax = 8;

	// Record a freshly-loaded mission path (dedupes to front, caps to kMax, persists).
	void Push( const char* path );

	// Read access (loads the file on first use).
	int  Count();
	const char* Path( int index );   // index 0 = most recent; "" if out of range
}

#endif // EDITOR_RECENT_H
