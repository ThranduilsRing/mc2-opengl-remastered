/***************************************************************
* FILENAME: TerrainDlg.cpp
* DESCRIPTION: Implements the Editor terrain dialog.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster comments and attribution header.
****************************************************************/

#define TERRAINDLG_CPP
/*************************************************************************************************\
TerrainDlg.cpp		: Implementation of the TerrainDlg component.
\*************************************************************************************************/

#include "stdafx.h"
#include "mclib.h"
#include "TerrainDlg.h"

#include "resource.h"
#include "mclibresource.h"
#include "utilities.h"
#include "terrtxm2.h"
#include "EditorResourceFallback.h"

extern HSTRRES gameResourceHandle;		//Default handle must be used for mc2res.dll due to shared game/editor code

//----------------------------------------------------------------------
void TerrainDlg::Init()
{
	CListBox* pListBox = (CListBox*)GetDlgItem( IDC_TERRAINS );

	int numTerrains = TerrainColorMap::getNumTypes();
	for ( int i = 0; i < numTerrains; i++ )
	{
		
		char buffer[256];
		
		if ( !TerrainColorMap::getTextureNameID(i) ) // if we start to repeat, quit
			break;	
			
		EditorSafeLoadString( TerrainColorMap::getTextureNameID(i), buffer, 256 );
		int index = pListBox->AddString( buffer );
		pListBox->SetItemData( index, i );
	}

	pListBox->SetCurSel( terrain );
}

//----------------------------------------------------------------------
void TerrainDlg::OnOK()
{
	int index = ((CListBox*)GetDlgItem( IDC_TERRAINS ))->GetCurSel( );
	terrain = ((CListBox*)GetDlgItem( IDC_TERRAINS ))->GetItemData( index );

	CDialog::OnOK();
}

//----------------------------------------------------------------------
