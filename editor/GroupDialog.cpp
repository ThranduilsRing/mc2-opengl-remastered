//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

// GroupDialog.cpp : implementation file
//

#include "stdafx.h"
#include "resource.h"
#include "GroupDialog.h"
#include "CampaignData.h"
#include "MissionDialog.h"
#include "../MCLib/Paths.h"
#include "assert.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif



CGroupDialog::CGroupDialog(CWnd* pParent /*=NULL*/)
	: CDialog(CGroupDialog::IDD, pParent)
{
	m_OperationFileEdit = _T("");
	m_PreVideoFileEdit = _T("");
	m_VideoFileEdit = _T("");
	m_LabelEdit = _T("");
	m_NumMissionsToCompleteEdit = 0;
}


void CGroupDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_GR_TUNE_COMBO, m_TuneComboControl);
	DDX_Control(pDX, IDC_GR_MISSION_LIST, m_MissionListControl);
	DDX_Text(pDX, IDC_GR_OPERATION_FILE_EDIT, m_OperationFileEdit);
	DDX_Text(pDX, IDC_GR_PREVIDEO_FILE_EDIT, m_PreVideoFileEdit);
	DDX_Text(pDX, IDC_GR_VIDEO_FILE_EDIT, m_VideoFileEdit);
	DDX_Text(pDX, IDC_GR_LABEL_EDIT, m_LabelEdit);
	DDX_Text(pDX, IDC_GR_NUM_MISSIONS_TO_COMPLETE_EDIT, m_NumMissionsToCompleteEdit);
}


BEGIN_MESSAGE_MAP(CGroupDialog, CDialog)
	ON_BN_CLICKED(IDC_GR_OPERATION_FILE_BROWSE_BUTTON, OnGrOperationFileBrowseButton)
	ON_BN_CLICKED(IDC_GR_PREVIDEO_FILE_BROWSE_BUTTON, OnGrPrevideoFileBrowseButton)
	ON_BN_CLICKED(IDC_GR_VIDEO_FILE_BROWSE_BUTTON, OnGrVideoFileBrowseButton)
	ON_BN_CLICKED(IDC_GR_ADD_BUTTON, OnGrAddButton)
	ON_BN_CLICKED(IDC_GR_REMOVE_BUTTON, OnGrRemoveButton)
	ON_BN_CLICKED(IDC_GR_EDIT_BUTTON, OnGrEditButton)
END_MESSAGE_MAP()


void CGroupDialog::OnGrOperationFileBrowseButton() {
	UpdateData(TRUE);
	while (true) {
		CFileDialog selectFileDialog(TRUE,_T("FIT"),_T("*.FIT"),
						 OFN_HIDEREADONLY|OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR,
						 _T("FIT File (*.FIT)|*.FIT|"));
		selectFileDialog.m_ofn.lpstrInitialDir = artPath;
		if( selectFileDialog.DoModal()==IDOK ) {
			CString operationPath = selectFileDialog.GetPathName();
			FitIniFile file;
			long result = 0;
			result = file.open( (char*)(const char*)operationPath );
			if (NO_ERR != result) {
				AfxMessageBox(IDS_COULDNT_OPEN_OPERATION_FILE);
			} else {
				long result = 0;
				result = file.seekBlock("Button0");
				if (NO_ERR != result)
				{
					AfxMessageBox(IDS_NOT_A_VALID_OPERATION_FILE);
				}
				file.close();
			}
			m_OperationFileEdit = selectFileDialog.GetFileTitle();
			break;
		} else {
			break;
		}
	}
	UpdateData(FALSE);
}

void CGroupDialog::OnGrPrevideoFileBrowseButton() {
	UpdateData(TRUE);
	while (true) {
		CFileDialog selectFileDialog(TRUE,_T("AVI"),_T("*.AVI"),
						 OFN_HIDEREADONLY|OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR,
						 _T("Movie (*.AVI)|*.AVI|"));
		selectFileDialog.m_ofn.lpstrInitialDir = moviePath;
		if( selectFileDialog.DoModal()==IDOK ) {
			m_PreVideoFileEdit = selectFileDialog.GetFileTitle();
			break;
		} else {
			break;
		}
	}
	UpdateData(FALSE);
}

void CGroupDialog::OnGrVideoFileBrowseButton() {
	UpdateData(TRUE);
	while (true) {
		CFileDialog selectFileDialog(TRUE,_T("AVI"),_T("*.AVI"),
						 OFN_HIDEREADONLY|OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR,
						 _T("Movie (*.AVI)|*.AVI|"));
		selectFileDialog.m_ofn.lpstrInitialDir = moviePath;
		if( selectFileDialog.DoModal()==IDOK ) {
			m_VideoFileEdit = selectFileDialog.GetFileTitle();
			break;
		} else {
			break;
		}
	}
	UpdateData(FALSE);
}

static void setMissionListBoxValues(CListBox &MissionListBox, const CMissionList &MissionList) {
	MissionListBox.ResetContent();
	for (const CMissionData& mission : MissionList) {
		CString tmpCStr;
		tmpCStr = _TEXT("[");
		tmpCStr += mission.m_MissionFile;
		tmpCStr += _TEXT("] ");
		MissionListBox.AddString(tmpCStr);
	}
}

BOOL CGroupDialog::OnInitDialog() 
{
	CDialog::OnInitDialog();

	setMissionListBoxValues(m_MissionListControl, m_MissionList);
	m_TuneComboControl.SetCurSel(m_TuneNumber);
	
	return TRUE;  // return TRUE unless you set the focus to a control
}

static void setMissionDialogValues(CMissionDialog &missionDialog, const CMissionData &missionData) {
	missionDialog.m_MissionFileEdit = missionData.m_MissionFile;
	missionDialog.m_PurchaseFileEdit = missionData.m_PurchaseFile;
	missionDialog.m_LogisticsCheck = missionData.m_LogisticsEnabled;
	missionDialog.m_MandatoryCheck = missionData.m_IsMandatory;
	missionDialog.m_PilotPromotionCheck = missionData.m_PilotPromotionEnabled;
	missionDialog.m_PurchasingCheck = missionData.m_PurchasingEnabled;
	missionDialog.m_SalvageCheck = missionData.m_SalvageEnabled;
}

static void setMissionDataValues(CMissionData &missionData, const CMissionDialog &missionDialog) {
	missionData.m_MissionFile = missionDialog.m_MissionFileEdit;
	missionData.m_PurchaseFile = missionDialog.m_PurchaseFileEdit;
	missionData.m_LogisticsEnabled = missionDialog.m_LogisticsCheck;
	missionData.m_IsMandatory = missionDialog.m_MandatoryCheck;
	missionData.m_PilotPromotionEnabled = missionDialog.m_PilotPromotionCheck;
	missionData.m_PurchasingEnabled = missionDialog.m_PurchasingCheck;
	missionData.m_SalvageEnabled = missionDialog.m_SalvageCheck;
}

void CGroupDialog::OnGrAddButton() 
{
	CMissionData missionData;
	CMissionDialog missionDialog;
	setMissionDialogValues(missionDialog, missionData);
	int ret = missionDialog.DoModal();
	if (IDOK == ret) {
		setMissionDataValues(missionData, missionDialog);
		m_MissionList.push_back(missionData);
		UpdateData(TRUE);
		setMissionListBoxValues(m_MissionListControl, m_MissionList);
		m_MissionListControl.SetCurSel(m_MissionListControl.GetCount() - 1);
		UpdateData(FALSE);
	}
}

void CGroupDialog::OnGrRemoveButton() 
{
	unsigned long selectedItemIndex = m_MissionListControl.GetCurSel();
	if ((0 <= selectedItemIndex) && (m_MissionList.size() > selectedItemIndex)) {
		m_MissionList.erase(m_MissionList.begin() + selectedItemIndex);
		UpdateData(TRUE);
		int selectedItemIndex = m_MissionListControl.GetCurSel();
		setMissionListBoxValues(m_MissionListControl, m_MissionList);
		if ((LB_ERR != selectedItemIndex) && (0 < m_MissionListControl.GetCount())) {
			if (m_MissionListControl.GetCount() <= (long)selectedItemIndex) {
				selectedItemIndex = m_MissionListControl.GetCount() - 1;
			}
			m_MissionListControl.SetCurSel(selectedItemIndex);
		}
		UpdateData(FALSE);
	}
}

void CGroupDialog::OnGrEditButton() 
{
	unsigned long selectedItemIndex = m_MissionListControl.GetCurSel();
	if ((0 <= selectedItemIndex) && (m_MissionList.size() > selectedItemIndex)) {
		CMissionData &missionDataRef = m_MissionList[selectedItemIndex];
		CMissionDialog missionDialog;
		setMissionDialogValues(missionDialog, missionDataRef);
		if (IDOK == missionDialog.DoModal()) {
			setMissionDataValues(missionDataRef, missionDialog);
			UpdateData(TRUE);
			setMissionListBoxValues(m_MissionListControl, m_MissionList);
			UpdateData(FALSE);
		}
	}
}

void CGroupDialog::OnOK() 
{
	int tmpInt = m_TuneComboControl.GetCurSel();
	if ((CB_ERR != tmpInt) && (0 <= tmpInt)) {
		m_TuneNumber = m_TuneComboControl.GetCurSel();
	}
	
	CDialog::OnOK();
}
