#pragma once
//----------------------------------------------------------------------------
// mc2_dialog.h -- modal message boxes that never block an unattended run.
//
// A blocking MessageBox(NULL, ...) in an agent-driven run hangs the process
// forever and puts a dialog on the user's desktop. When no user is present
// (gos_NoUserPresentRequested(): MC2_WINDOW_OFFSCREEN or MC2_SMOKE_MODE) the
// message is logged and the caller's `unattendedResult` is returned instead.
//
// Pick `unattendedResult` as the answer that cannot loop: for a retry prompt
// (file missing, OK = try again) that is IDCANCEL, because nothing can fix the
// file while the run is unattended; for an informational or continue-anyway
// prompt it is IDOK. Interactive runs (neither variable set) are unchanged.
//
// Include after <windows.h> / the engine headers that declare MessageBox.
//----------------------------------------------------------------------------

#include "mc2_czstring.h"
#include <cstdio>

// Defined in GameOS/gameos/gameos_input.cpp (game and editor builds).
bool gos_NoUserPresentRequested();

inline int mc2MessageBox(mc2::czstring text, mc2::czstring title, unsigned type, int unattendedResult)
{
    if (gos_NoUserPresentRequested()) {
        std::fprintf(stderr, "[DIALOG_SUPPRESSED] title=\"%s\" result=%d text=\"%s\"\n",
                     title ? title : "", unattendedResult, text ? text : "");
        std::fflush(stderr);
        return unattendedResult;
    }
    return MessageBox(nullptr, text, title, type);
}
