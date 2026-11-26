//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

// newsinglemission.cpp : implementation file
//

#include "stdafx.h"
#include "resource.h"
#include "newsinglemission.h"
#include "ModPicker.h"
#include "EditorRecent.h"
#include "LauncherMissions.h"
#include <filesystem>
#include <string>

std::string EditorInstallPath(const char* relative)
{
	char executable[32768] = {};
	const DWORD length = GetModuleFileNameA(nullptr, executable, sizeof(executable));
	if (!length || length >= sizeof(executable)) return {};
	return (std::filesystem::path(executable).parent_path() / relative).string();
}

// When the user picks a Recent mission and clicks Edit Mission, this holds the path
// so EditorInterface loads it directly instead of popping the file dialog. Empty =
// browse as before. Read+cleared by the ID_LOAD_MISSION branch in EditorInterface.cpp.
std::string g_editorPreselectMission;

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// NewSingleMission dialog


NewSingleMission::NewSingleMission(CWnd* pParent /*=NULL*/)
	: CDialog(NewSingleMission::IDD, pParent)
{
	//{{AFX_DATA_INIT(NewSingleMission)
		// NOTE: the ClassWizard will add member initialization here
	//}}AFX_DATA_INIT
}


void NewSingleMission::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(NewSingleMission)
		// NOTE: the ClassWizard will add DDX and DDV calls here
	//}}AFX_DATA_MAP
}


BEGIN_MESSAGE_MAP(NewSingleMission, CDialog)
	//{{AFX_MSG_MAP(NewSingleMission)
	ON_BN_CLICKED(ID_LOAD_MISSION, OnLoadMission)
	ON_BN_CLICKED(ID_NEWMISSION, OnNewmission)
	ON_BN_CLICKED(ID_MAPGENERATOR, OnMapGenerator)
	ON_BN_CLICKED(IDC_MI_STOCK_OPEN, OnStockMission)
	ON_BN_CLICKED(IDC_MI_RECENT_OPEN, OnRecentMission)
	ON_BN_CLICKED(IDC_MI_MOD_OPEN, OnModMission)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// NewSingleMission message handlers

BOOL NewSingleMission::OnInitDialog()
{
	CDialog::OnInitDialog();
	g_editorPreselectMission.clear();
	// Background harnesses must also keep this modal off the visible desktop.
	extern bool EditorBackgroundLaunch();
	if (EditorBackgroundLaunch())
		SetWindowPos(nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
	CComboBox* stock = (CComboBox*)GetDlgItem(IDC_MI_STOCK_COMBO);
	stock->ResetContent();
	for (const auto& name : editor::launcher::campaignMissions(EditorInstallPath("data/missions")))
		stock->AddString(name.c_str());
	stock->SetCurSel(0);
	if (!stock->GetCount())
		SetDlgItemText(IDC_MI_STOCK_HINT, "No loose stock missions. Add mc2_01.pak + mc2_01.fit to data/missions.");

	// Populate the Mod combo: "None (stock)" + every folder under mods/. Pick the
	// mod BEFORE loading a mission so its data/* shadows base data/* (mod missions'
	// appearance assets resolve); default None keeps stock editing clean.
	CComboBox* combo = (CComboBox*)GetDlgItem( IDC_MI_MOD_COMBO );
	if ( combo )
	{
		ModPicker::ScanMods();
		combo->ResetContent();
		int noneIdx = combo->AddString( "None (stock)" );
		combo->SetItemData( noneIdx, (DWORD_PTR)-1 );
		int sel = noneIdx;
		const char* active = ModPicker::ActiveMod();
		for ( int i = 0; i < ModPicker::ModCount(); ++i )
		{
			int idx = combo->AddString( ModPicker::ModName( i ) );
			combo->SetItemData( idx, (DWORD_PTR)i );
			if ( active && active[0] && strcmp( active, ModPicker::ModName( i ) ) == 0 )
				sel = idx;
		}
		combo->SetCurSel( sel );
	}

	// Recent missions are listed newest first.
	CComboBox* recent = (CComboBox*)GetDlgItem( IDC_MI_RECENT_COMBO );
	if ( recent )
	{
		recent->ResetContent();
		for ( int i = 0; i < EditorRecent::Count(); ++i )
		{
			const char* p = EditorRecent::Path( i );
			recent->AddString(p);
		}
		recent->SetCurSel( 0 );
		recent->SetDroppedWidth(680);
		GetDlgItem(IDC_MI_RECENT_OPEN)->EnableWindow(recent->GetCount() > 0);
	}
	return TRUE;
}

// Read the combo and activate that mod (or stock) before the dialog's load path runs.
void NewSingleMission::applySelectedMod()
{
	CComboBox* combo = (CComboBox*)GetDlgItem( IDC_MI_MOD_COMBO );
	if ( !combo ) return;
	int cur = combo->GetCurSel();
	if ( cur < 0 ) return;
	int modIdx = (int)(INT_PTR)combo->GetItemData( cur );
	if ( modIdx < 0 ) ModPicker::Activate( "" );                       // None (stock)
	else              ModPicker::Activate( ModPicker::ModName( modIdx ) );
}

void NewSingleMission::OnLoadMission()
{
	applySelectedMod();
	g_editorPreselectMission.clear();
	EndDialog(ID_LOAD_MISSION);
}

void NewSingleMission::OnRecentMission()
{
	const int selected = ((CComboBox*)GetDlgItem(IDC_MI_RECENT_COMBO))->GetCurSel();
	if (selected < 0) return;
	const char* path = EditorRecent::Path(selected);
	if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
	{
		AfxMessageBox("This recent mission has moved or been removed. Use Edit Mission to locate it.");
		return;
	}
	applySelectedMod();
	g_editorPreselectMission = path;
	EndDialog(ID_LOAD_MISSION);
}

void NewSingleMission::OnModMission()
{
	CComboBox* combo = (CComboBox*)GetDlgItem(IDC_MI_MOD_COMBO);
	const int selected = combo->GetCurSel();
	if (selected < 0 || (INT_PTR)combo->GetItemData(selected) < 0)
	{
		AfxMessageBox("Choose a mod first.");
		return;
	}
	const int mod = (int)(INT_PTR)combo->GetItemData(selected);
	const std::string folder = EditorInstallPath((std::string("mods/") + ModPicker::ModName(mod) + "/data/missions/").c_str());
	CFileDialog browser(TRUE, "pak", (folder + "\\").c_str(),
		OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR, "Mission maps (*.pak)|*.pak||", this);
	browser.m_ofn.lpstrInitialDir = folder.c_str();
	if (browser.DoModal() != IDOK) return;
	applySelectedMod();
	g_editorPreselectMission = (const char*)browser.GetPathName();
	EndDialog(ID_LOAD_MISSION);
}

void NewSingleMission::OnStockMission()
{
	CString selected;
	CComboBox* stock = (CComboBox*)GetDlgItem(IDC_MI_STOCK_COMBO);
	if (stock->GetCurSel() < 0)
	{
		AfxMessageBox("No loose campaign missions in this install. Put matching mc2_01.pak and mc2_01.fit (optionally mc2_01.abl) in data/missions. Packed mission.fst is not used by this entry point.");
		return;
	}
	stock->GetLBText(stock->GetCurSel(), selected);
	std::error_code error;
	const auto copy = editor::launcher::copyCampaign(EditorInstallPath(""), (const char*)selected, error);
	if (error || copy.empty())
	{
		AfxMessageBox("Could not create an editable mission copy. Check the loose mission files and user-missions folder permissions.");
		return;
	}
	ModPicker::Activate("");
	g_editorPreselectMission = copy.string();
	EndDialog(ID_LOAD_MISSION);
}

void NewSingleMission::OnNewmission()
{
	applySelectedMod();
	EndDialog( ID_NEWMISSION );

}

void NewSingleMission::OnMapGenerator()
{
	applySelectedMod();
	EndDialog( ID_MAPGENERATOR );
}

void NewSingleMission::OnCancel() 
{
	// TODO: Add extra cleanup here
	
	CDialog::OnCancel();
}
