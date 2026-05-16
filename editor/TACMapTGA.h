#ifndef TACMAPTGA_H
#define TACMAPTGA_H
/***************************************************************
* FILENAME: TACMapTGA.h
* DESCRIPTION: Declares the Editor tactical map TGA control.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Added Editor tacmap bitmap/display constants for the doubled UI size.
****************************************************************/

#ifndef TGAWND_H
#include "TGAWnd.h"
#endif

// by Methuselas: keep the saved/loaded tacmap packet at the legacy
// 128x128 size, but draw the Editor's floating tacmap window at 2x.
// This enlarges the UI without changing mission packet compatibility.
enum
{
	EDITOR_TACMAP_BITMAP_SIZE = 128,
	EDITOR_TACMAP_DISPLAY_SIZE = EDITOR_TACMAP_BITMAP_SIZE * 2
};

//*************************************************************************************************
/**************************************************************************************************
CLASS DESCRIPTION
TACMapTGA:
**************************************************************************************************/
class TacMapTGA: public TGAWnd
{
	public:

	afx_msg void OnPaint();
	void refreshBmp();
	DECLARE_MESSAGE_MAP()

	
};


//*************************************************************************************************
#endif  // end of file ( TACMapTGA.h )
