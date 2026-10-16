/***************************************************************
* FILENAME: ResourceStringSelectionDlg.cpp
* DESCRIPTION: Implements the Editor resource-string selection dialog.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster comments and attribution header.
****************************************************************/

// ResourceStringSelectionDlg.cpp : implementation file
//

#include "stdafx.h"
#include "resource.h"
#include "ResourceStringSelectionDlg.h"

#include "assert.h"
#include "MC2Strings.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// ResourceStringSelectionDlg dialog


ResourceStringSelectionDlg::ResourceStringSelectionDlg(CWnd* pParent /*=NULL*/)
	: CDialog(ResourceStringSelectionDlg::IDD, pParent)
{
	m_BottomOfIDRange = 0;
	m_TopOfIDRange = 65535;
	m_SelectedResourceStringID = -1;

	//{{AFX_DATA_INIT(ResourceStringSelectionDlg)
		// NOTE: the ClassWizard will add member initialization here
	//}}AFX_DATA_INIT
}


void ResourceStringSelectionDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(ResourceStringSelectionDlg)
	DDX_Control(pDX, IDC_RESOURCE_STRING_SELECTION_COMBO, m_Combo);
	//}}AFX_DATA_MAP
}


BEGIN_MESSAGE_MAP(ResourceStringSelectionDlg, CDialog)
	//{{AFX_MSG_MAP(ResourceStringSelectionDlg)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// ResourceStringSelectionDlg message handlers

static BOOL CSLoadString(int resourceID, CString &targetStr) {
	const char* text = MC2Strings::GetByLegacyId(resourceID);
	targetStr = text ? text : "";
	return text && text[0];
}

BOOL ResourceStringSelectionDlg::OnInitDialog() 
{
	CDialog::OnInitDialog();
	
	m_ResourceStringIDs.clear();
	m_Combo.ResetContent();
	unsigned long i;
	for (i = m_BottomOfIDRange; i <= (unsigned long)m_TopOfIDRange; i += 1) {
		CString tmpStr;
		BOOL result = CSLoadString(i, tmpStr);
		if (0 != result) {
			CString tmpStr2;
			tmpStr2.Format("%d: ", i);
			tmpStr2 += tmpStr;
			m_Combo.AddString(tmpStr2);
			m_ResourceStringIDs.push_back(i);
		}
	}
	for (i = 0; i < m_ResourceStringIDs.size(); i += 1) {
		if (m_SelectedResourceStringID == m_ResourceStringIDs[i]) {
			m_Combo.SetCurSel(i);
			break;
		}
	}
	if (m_ResourceStringIDs.size() == i) {
		m_Combo.SetCurSel(-1);
	}
	
	return TRUE;
}

void ResourceStringSelectionDlg::OnOK() 
{
	unsigned long selectionIndex = m_Combo.GetCurSel();
	if (CB_ERR != selectionIndex) {
		assert(m_ResourceStringIDs.size() > selectionIndex);
		m_SelectedResourceStringID = m_ResourceStringIDs[selectionIndex];
	}
	
	CDialog::OnOK();
}
