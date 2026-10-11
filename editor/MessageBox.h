/***************************************************************
* FILENAME: MessageBox.h
* DESCRIPTION: Declares Editor message-box helper behavior.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster comments and attribution header.
****************************************************************/

#ifndef MESSAGEBOX_H
#define MESSAGEBOX_H
/*************************************************************************************************\
MessageBox.h			: quick helper function to call up a message box
\*************************************************************************************************/
#ifndef UTILITIES_H
#include "Utilities.h"
#endif

#include "EditorInterface.h"

#include "stdafx.h"
#include "EditorResourceFallback.h"

//*************************************************************************************************

/**************************************************************************************************
CLASS DESCRIPTION
MessageBox:
**************************************************************************************************/
extern HSTRRES gameResourceHandle;

// Smoke/headless mode (EditorMFC.cpp). When set, every editor modal is suppressed
// and answered IDOK -- a headless run has no one to click, and the save path in
// particular raises validation warnings (no units / no players) that would hang
// it forever. This is the single-point gate for all EMessageBox callers.
extern bool g_cliSuppressModals;

// Emit a suppressed-modal record so the smoke harness captures what a user would
// otherwise have had to click through. One line per suppressed modal, to BOTH the
// inherited stderr pipe and editor-startup.log. Defined in EditorMFC.cpp.
void EditorSmokeLogSuppressedModal(const char* text, const char* caption);

inline int EMessageBox(int MessageID, int CaptionID,DWORD dwS )
{
	char buffer[512];
	char bufferCaption[512];

	EditorSafeLoadString( MessageID, buffer, 512, gameResourceHandle );

	EditorSafeLoadString( CaptionID, bufferCaption, 512, gameResourceHandle );

	if (g_cliSuppressModals)
	{
		// Headless smoke: record the text, then answer the safe continue default.
		// These EMessageBox callers are all MB_OK / warning class -- IDOK is the
		// only and non-destructive response, so continuing is correct.
		EditorSmokeLogSuppressedModal( buffer, bufferCaption );
		return IDOK;
	}

	if (EditorInterface::instance() && EditorInterface::instance()->ThisIsInitialized())
	{
		return EditorInterface::instance()->MessageBoxA( buffer, bufferCaption, dwS );
	}
	else
	{
		/*note: this messagebox will not be modal wrt the application*/
		return ::MessageBoxA( nullptr, buffer, bufferCaption, dwS );
	}

}





//*************************************************************************************************
#endif  // end of file ( MessageBox.h )
