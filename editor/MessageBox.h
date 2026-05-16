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

inline int EMessageBox(int MessageID, int CaptionID,DWORD dwS )
{
	char buffer[512];
	char bufferCaption[512];

	EditorSafeLoadString( MessageID, buffer, 512, gameResourceHandle );

	EditorSafeLoadString( CaptionID, bufferCaption, 512, gameResourceHandle );
	
	if (EditorInterface::instance() && EditorInterface::instance()->ThisIsInitialized())
	{
		return EditorInterface::instance()->MessageBoxA( buffer, bufferCaption, dwS );
	}
	else
	{
		/*note: this messagebox will not be modal wrt the application*/
		return ::MessageBoxA( NULL, buffer, bufferCaption, dwS );
	}

}





//*************************************************************************************************
#endif  // end of file ( MessageBox.h )
