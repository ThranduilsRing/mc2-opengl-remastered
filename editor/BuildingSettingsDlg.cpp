//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

// BuildingSettingsDlg.cpp : implementation file
//

#include "stdafx.h"
#include "resource.h"
#include "BuildingSettingsDlg.h"
#include "EditorObjects.h"
#include "EditorObjectMgr.h"
#include "EditorInterface.h" // just for the undo manager

#include "../Code/unitdesg.h" /* just for definition of MIN_TERRAIN_PART_ID and MAX_MAP_CELL_WIDTH */

/////////////////////////////////////////////////////////////////////////////
// BuildingSettingsDlg dialog


BuildingSettingsDlg::BuildingSettingsDlg( std::vector<EditorObject*>& newList/*=NULL*/, ActionUndoMgr &undoMgr)
	: CDialog(BuildingSettingsDlg::IDD), units( newList )
{
	//{{AFX_DATA_INIT(BuildingSettingsDlg)
	m_Alignment = -1;
	m_x = 0.0f;
	m_y = 0.0f;
	m_partID = 0;
	m_forestName = _T("");
	m_bDamaged = FALSE;
	//}}AFX_DATA_INIT

	pUndoMgr = &undoMgr;
	pAction = nullptr;
}


void BuildingSettingsDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(BuildingSettingsDlg)
	DDX_Control(pDX, IDC_MECH, m_Mech);
	DDX_Control(pDX, IDC_GROUP, m_Group);
	DDX_Radio(pDX, IDC_ALIGN1, m_Alignment);
	DDX_Text(pDX, IDC_BS_X_EDIT, m_x);
	DDX_Text(pDX, IDC_BS_Y_EDIT, m_y);
	DDX_Text(pDX, IDC_BS_PARTID_EDIT, m_partID);
	DDX_Text(pDX, IDC_FOREST_NAME, m_forestName);
	DDX_Check(pDX, IDC_BS_DAMAGED_CHECK, m_bDamaged);
	//}}AFX_DATA_MAP
}


BEGIN_MESSAGE_MAP(BuildingSettingsDlg, CDialog)
	//{{AFX_MSG_MAP(BuildingSettingsDlg)
	ON_CBN_SELCHANGE(IDC_GROUP, OnSelchangeGroup)
	ON_CBN_SELCHANGE(IDC_MECH, OnSelchangeMech)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// BuildingSettingsDlg message handlers

void BuildingSettingsDlg::OnSelchangeGroup() 
{
		m_Mech.ResetContent();

		int group = m_Group.GetCurSel();
		group = m_Group.GetItemData( group );

		const char* MechNames[256];
		int count = 256;
			
		EditorObjectMgr::instance()->getBuildingNamesInGroup( group, MechNames, count );

		for ( int i = 0; i < count; ++i )
		{
			m_Mech.AddString( MechNames[i] );
		}


		m_Mech.SetCurSel( 0 );	
}

void BuildingSettingsDlg::applyChanges()
{
	// Preserve each selected object's world position across the appearance/shape
	// swaps below (setAppearance / setDamage can swap the mesh and otherwise shift
	// the object); restored + re-baked at the end of this function.
	std::vector<Stuff::Vector3D> savedPos;
	for ( auto pit = units.begin(); pit != units.end(); pit++ )
		savedPos.push_back( (*pit)->appearance() ? (*pit)->appearance()->position : Stuff::Vector3D() );

	// get the type info from the dlg box
	int index = m_Group.GetCurSel( );
	if ( index != -1 )
	{
		int group = m_Group.GetItemData( index );

		int indexInGroup = m_Mech.GetCurSel( );

		if ( indexInGroup != -1 )
		{
			for ( auto iter = units.begin(); iter != units.end(); iter++ )
			{
				(*iter)->setAppearance( group, indexInGroup );
			}
		}
	}

	// now set the alignment
	UpdateData( true );
	index = m_Alignment;

	if ( index != -1 )
	{
		for ( auto iter = units.begin(); iter != units.end(); iter++ )
		{
			(*iter)->setAlignment( index );
		}
	}

	// apply the damaged flag (UpdateData(true) above refreshed m_bDamaged).
	// setDamage() updates the appearance's damage state directly; the editor
	// render loop reads appearance()->damage each frame, so no extra refresh
	// is needed for the change to show.
	for ( auto iter = units.begin(); iter != units.end(); iter++ )
	{
		(*iter)->setDamage( m_bDamaged ? true : false );
	}

	unsigned long base=0, color1=0, color2=0;
	bool bBase = false;
	bool bColor1 = false; 
	bool bColor2 = false;

	// now figure out the colors
	CWnd* pWnd = GetDlgItem( IDC_BASE );
	if ( pWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		if ( tmpStr.GetLength() )
		{
			bBase = true;
		

			tmpStr.Replace( "0x", "" );
			sscanf( tmpStr, "%x", &base );
			base |= 0xff000000;
		}
		
	}

	pWnd = GetDlgItem( IDC_HIGHLIGHT1 );
	if ( pWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		if ( tmpStr.GetLength() )
		{
			bColor1 = true;
		

			tmpStr.Replace( "0x", "" );
			sscanf( tmpStr, "%x", &color1 );
			color1 |= 0xff000000;
		}
		
	}
	pWnd = GetDlgItem( IDC_HIGHLIGHT2 );
	if ( pWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		if ( tmpStr.GetLength() )
		{
			bColor2 = true;
		

			tmpStr.Replace( "0x", "" );
			sscanf( tmpStr, "%x", &color2 );
			color2 |= 0xff000000;
		}

	}

	// Restore the saved world positions + re-bake static recipes so toggling the
	// type/damaged state does not move the building.
	{
		size_t si = 0;
		for ( auto rit = units.begin(); rit != units.end() && si < savedPos.size(); rit++, ++si )
		{
			ObjectAppearance* pA = (*rit)->appearance();
			if ( pA )
			{
				pA->position = savedPos[si];
				pA->invalidateStaticRegistration();
				pA->update();
				pA->registerStatic();
			}
		}
	}
}

void BuildingSettingsDlg::OnOK()
{
	if (nullptr != pUndoMgr)
	{
		pUndoMgr->AddAction(pAction);
	}
	else
	{
		delete pAction;
	}
	pAction = nullptr;

	applyChanges();
	CDialog::OnOK();
}

BOOL BuildingSettingsDlg::OnInitDialog() 
{
	CDialog::OnInitDialog();

	pAction = new ModifyBuildingAction;

	EDITOROBJECT_LIST::iterator iter;

	for ( iter = units.begin(); iter != units.end(); iter++ )
	{
		pAction->addBuildingInfo(*(*iter));
	}

	updateMemberVariables();

	return TRUE;  // return TRUE unless you set the focus to a control
	              // EXCEPTION: OCX Property Pages should return FALSE
}

void BuildingSettingsDlg::OnSelchangeMech() 
{
	int group = m_Group.GetCurSel();
	group = m_Group.GetItemData( group );

	/*int indexInGroup =*/ m_Mech.GetCurSel();
}

void BuildingSettingsDlg::updateMemberVariables()
{

	long forest = -1;
	bool bForests = true;

	EditorObject* pEditorObject = units.front();

	m_Alignment = pEditorObject->getAlignment();
	if (m_Alignment == -1)
		m_Alignment = 8;			//Move it to the neutral select button

	for ( auto iter = units.begin(); iter != units.end(); iter++ )
	{
		if ( ((*iter)->getAlignment() != m_Alignment) && ((*iter)->getAlignment() != -1))
		{
			m_Alignment = -1;
			break;
		}

		if ( (*iter)->getForestID() != -1 )
		{
			if ( forest == -1 )
				forest = (*iter)->getForestID();
			else if ( forest != (*iter)->getForestID() )
				bForests = false;

		}
	}

	if ( forest != -1 )
	{
		const Forest* pForest = EditorObjectMgr::instance()->getForest( forest );

		if ( pForest )
		{
			m_forestName = pForest->getFileName();
		}
	}

	EditorObjectMgr* pMgr = EditorObjectMgr::instance();

	int groupCount = pMgr->getBuildingGroupCount();

	const char** pGroups = new const char*[groupCount];
	
	m_Group.ResetContent();
	
	pMgr->getBuildingGroupNames(pGroups, groupCount);

	int count = 0;
	for ( int i = 0; i < groupCount; ++i )
	{
		if ((4/*mech group*/ == i) || (6/*vehicle group*/ == i))
		{
			continue;
		}
		m_Group.AddString( pGroups[i] );
		m_Group.SetItemData( count, (DWORD)i );
		count += 1;
	}

	delete [] pGroups;

	// make sure all the units we are editing are in the same group
	int group = units.front()->getGroup();
	EDITOROBJECT_LIST::iterator iter;
	for ( iter = units.begin(); iter != units.end(); iter++ )
	{
		if ( (*iter)->getGroup() != group )
		{
			group = -1;
			break;
		}
	}

	if ( group != -1 ) // we found a valid group
	{
		const char* pGroupName = pMgr->getGroupName( group );
		
		int index = m_Group.FindString( -1, pGroupName );
		m_Group.SetCurSel( index );

		// OK, now fill in the index....
		const char* MechNames[256];
		int count = 256;

		m_Mech.ResetContent();
		pMgr->getBuildingNamesInGroup( group, MechNames, count );

		for ( int i = 0; i < count; ++i )
		{
			m_Mech.AddString( MechNames[i] );
		}

		// ok, now determine if all of the mechs are the same.
		int indexInGroup = units.front()->getIndexInGroup();

		for ( iter = units.begin(); iter != units.end(); iter++ )
		{
			if ( (*iter)->getIndexInGroup() != indexInGroup )
			{
				indexInGroup = -1;
				break;
			}
		}

		if ( indexInGroup != -1 )
		{
			const char* pName = units.front()->getDisplayName();
			index = m_Mech.FindString( -1, pName );

			if ( index != -1 )
			{
				m_Mech.SetCurSel( index );

			}
		}
	}

	// initialize the damaged checkbox from the building(s); if a multi-select
	// disagrees, leave it unchecked (applyChanges only writes the explicit state on OK)
	m_bDamaged = pEditorObject->getDamage() ? TRUE : FALSE;
	for ( iter = units.begin(); iter != units.end(); iter++ )
	{
		if ( ((*iter)->getDamage() ? TRUE : FALSE) != m_bDamaged )
		{
			m_bDamaged = FALSE;
			break;
		}
	}

	m_x = pEditorObject->getPosition().x;
	m_y = pEditorObject->getPosition().y;
	long row, column;
	pEditorObject->getCells(row, column);
	m_partID = MIN_TERRAIN_PART_ID + row * MAX_MAP_CELL_WIDTH + column;

	UpdateData( false );
}

void BuildingSettingsDlg::OnCancel() 
{
	pAction->undo();
	delete pAction;
	pAction = nullptr;
	
	CDialog::OnCancel();
}
