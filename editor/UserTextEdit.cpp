/***************************************************************
* FILENAME: UserTextEdit.cpp
* DESCRIPTION: Implements the Editor user text editing dialog.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster comments and attribution header.
****************************************************************/

// UserTextEdit.cpp : implementation file
//

#include "stdafx.h"
#include "resource.h"
#include "UserTextEdit.h"

#include "assert.h"
#include "TextMessageDlg.h"
#include "ResourceStringSelectionDlg.h"
#include "MC2Strings.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CUserTextEdit dialog


CUserTextEdit::CUserTextEdit(CWnd* pParent /*=NULL*/)
	: CDialog(CUserTextEdit::IDD, pParent)
{
	m_UseResourceString = false;
	m_ResourceStringID = 0;

	//{{AFX_DATA_INIT(CUserTextEdit)
	m_Edit = _T("");
	m_ResourceStringIDEdit = _T("");
	//}}AFX_DATA_INIT
}


void CUserTextEdit::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(CUserTextEdit)
	DDX_Text(pDX, IDC_USER_TEXT_EDIT_EDIT, m_Edit);
	DDX_Text(pDX, IDC_USER_TEXT_EDIT_RESOURCE_STRING_ID_EDIT, m_ResourceStringIDEdit);
	//}}AFX_DATA_MAP
}


BEGIN_MESSAGE_MAP(CUserTextEdit, CDialog)
	//{{AFX_MSG_MAP(CUserTextEdit)
	ON_BN_CLICKED(IDC_USER_TEXT_EDIT_ENTER_TEXT_BUTTON, OnUserTextEditEnterTextButton)
	ON_BN_CLICKED(IDC_USER_TEXT_EDIT_SELECT_RESOURCE_STRING_BUTTON, OnUserTextEditSelectResourceStringButton)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CUserTextEdit message handlers

static BOOL CSLoadString(int resourceID, CString &targetStr) {
	const char* text = MC2Strings::GetByLegacyId(resourceID);
	targetStr = text ? text : "";
	return text && text[0];
}

void CUserTextEdit::UpdateTextDisplay() {
	UpdateData(TRUE);
	if (m_UseResourceString) {
		m_ResourceStringIDEdit.Format("%d", m_ResourceStringID);
		int ret = CSLoadString(m_ResourceStringID, m_Edit);
		if (0 == ret) {
			m_Edit = _TEXT("");
			m_ResourceStringIDEdit += _TEXT(" (missing ID)");
		}
	} else {
		m_ResourceStringIDEdit = _TEXT("");
		m_Edit = m_UnlocalizedText;
	}
	UpdateData(FALSE);
}

void CUserTextEdit::OnUserTextEditEnterTextButton() 
{
	TextMessageDlg textMessageDlg;
	textMessageDlg.m_TextMessage = m_UnlocalizedText;
	int ret = textMessageDlg.DoModal();
	if (IDOK == ret) {
		m_UnlocalizedText = textMessageDlg.m_TextMessage;
		/*remove trailing "new line"s and "carriage return"s because the game doesn't like them*/
		while (("\n" == m_UnlocalizedText.Right(1)) || ("\r" == m_UnlocalizedText.Right(1))) {
			m_UnlocalizedText = m_UnlocalizedText.Left(m_UnlocalizedText.GetLength() - 1);
		}
		m_UseResourceString = false;
		UpdateTextDisplay();
	}
}

void CUserTextEdit::OnUserTextEditSelectResourceStringButton() 
{
	ResourceStringSelectionDlg resourceStringSelectionDlg;
	resourceStringSelectionDlg.m_SelectedResourceStringID = m_ResourceStringID;
	int ret = resourceStringSelectionDlg.DoModal();
	if (IDOK == ret) {
		m_ResourceStringID = resourceStringSelectionDlg.m_SelectedResourceStringID;
		m_UseResourceString = true;
		UpdateTextDisplay();
	}
}

BOOL CUserTextEdit::OnInitDialog() 
{
	CDialog::OnInitDialog();
	
	UpdateTextDisplay();
	
	return TRUE;  // return TRUE unless you set the focus to a control
	              // EXCEPTION: OCX Property Pages should return FALSE
}
