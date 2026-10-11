#ifndef POINTERSELECTOBJECTDLG_H
#define POINTERSELECTOBJECTDLG_H
/*************************************************************************************************\
PointerSelectObjectDlg.h		: Interface for the PointerSelectObjectDlg component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "stdafx.h"
#include "resource.h"
#include "Objective.h"


class PointerSelectObjectDlg: public CDialog
{
private:
	bool m_bTimerIsReset;
public:
	PointerSelectObjectDlg();
	PointerSelectObjectDlg(int x, int y);
	~PointerSelectObjectDlg() override;

	BOOL OnCommand(WPARAM wParam, LPARAM lParam) override; // called by child controls to inform of an event
	BOOL OnWndMsg( UINT message, WPARAM wParam, LPARAM lParam, LRESULT* pResult ) override;
	BOOL OnInitDialog() override;
	UINT OnNcHitTest( CPoint point );

private:
	using inherited = CDialog;

	// suppressing these
	//inline PointerSelectObjectDlg();
	PointerSelectObjectDlg& operator=( const PointerSelectObjectDlg& lgUnitPtr );

	CButton *m_pButton;
};


//*************************************************************************************************
#endif  // end of file ( PointerSelectObjectDlg.h )
