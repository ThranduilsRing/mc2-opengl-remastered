/***************************************************************
* FILENAME: SingleValueDlg.cpp
* DESCRIPTION: Implements the Editor single-value terrain assignment dialog.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster comments and attribution header.
****************************************************************/

#include "stdafx.h"
/*************************************************************************************************\
HeightDlg.cpp			: Implementation of the AssignHeight component.  This thing lets you assign
							a particular elevation to selected vertices
\*************************************************************************************************/
#include "SingleValueDlg.h"
#include "resource.h"
#include "mclib.h"
#include "utilities.h"
#include "EditorResourceFallback.h"

extern HSTRRES gameResourceHandle;		//Default handle must be used for mc2res.dll due to shared game/editor code

SingleValueDlg::SingleValueDlg( int captionID, int staticTextID, int newVal)
: CDialog( IDD_VALUE )
{
	EditorSafeLoadString( captionID, caption, 256, gameResourceHandle );
	EditorSafeLoadString( staticTextID, staticText, 256, gameResourceHandle );
	
	val = newVal;
}

SingleValueDlg::~SingleValueDlg()
{
}
void SingleValueDlg::Init()
{
	char minTxt[256];
	itoa( val, minTxt, 10 );
	
	((CEdit*)GetDlgItem( IDC_HEIGHT ))->SetWindowText( minTxt );
	((CStatic*)GetDlgItem( IDC_TEXT ))->SetWindowText( staticText );
	SetWindowText( caption );

}

void SingleValueDlg::OnOK()
{
	char wcsMinTxt[256];
	((CEdit*)GetDlgItem( IDC_HEIGHT ))->GetWindowText( wcsMinTxt, 256 );
	val = atoi( wcsMinTxt );

	CDialog::OnOK();
}

/*void SingleValueDlg::OnCommand(Window *wnd,int nCommand)
{

	if (nCommand == BC_CLICKED)
	{
		wchar_t wcsMinTxt[256];
		((Entry*)GetDlgItem( IDC_HEIGHT ))->GetEntry( wcsMinTxt, 256 );
		char minTxt[256];
		WcsToStr( minTxt, wcsMinTxt );
		val = atoi( minTxt );
		szText = 0; // otherwise it will try and free this

		EndDialog(wnd->GetID());

		Parent()->OnCommand( wnd, ID_OTHER_SELECT );
	}
}*/

void SingleValueDlg::Update()
{
	char minTxt[256];
	itoa( val, minTxt, 10 );

	((CEdit*)GetDlgItem( IDC_HEIGHT ))->SetWindowText( minTxt );
	((CStatic*)GetDlgItem( IDC_TEXT ))->SetWindowText( staticText );


}



//*************************************************************************************************
// end of file ( HeightDlg.cpp )
