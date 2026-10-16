/***************************************************************
* FILENAME: ObjectiveDlg.cpp
* DESCRIPTION: Implements the Editor objective dialog.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster comments and attribution header.
****************************************************************/

// ObjectiveDlg.cpp : implementation file
//

#include "stdafx.h"
#include "resource.h"
#include "ObjectiveDlg.h"
#include "mc2str.h"
#include "ObjectiveDlg_text_internal.h"

#include "assert.h"
#include "EditorInterface.h"
#include "ResourceStringSelectionDlg.h"
#include "UserTextEdit.h"
#include "ViewConditionOrActionDlg.h"
#include "EditorResourceFallback.h"

#if 0 /*gos doesn't like this */
#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif
#endif /*gos doesn't like this */



ObjectiveDlg::ObjectiveDlg(CWnd* pParent /*=NULL*/)
	: CDialog(ObjectiveDlg::IDD, pParent)
{
	m_TitleResourceStringID = 0;
	m_DescriptionResourceStringID = 0;

	m_PriorityRadioButton = -1;
	m_ResourcePointsEdit = 0;
	m_PreviousPrimaryObjectiveMustBeCompleteCheckButton = FALSE;
	m_AllPreviousPrimaryObjectivesMustBeCompleteCheckButton = FALSE;
	m_DisplayMarkerCheckButton = FALSE;
	m_XEdit = 0.0f;
	m_YEdit = 0.0f;
	m_HiddenTriggerCheckButton = FALSE;
	m_ActivateOnFlagCheckButton = FALSE;
	m_TitleEdit = _T("");
	m_DescriptionEdit = _T("");
	m_TitleResourceStringIDEdit = _T("");
	m_DescriptionResourceStringIDEdit = _T("");
	m_ActivateFlagIDEdit = _T("");
	m_ResetStatusOnFlagCheckButton = FALSE;
	m_ResetStatusFlagIDEdit = _T("");

	nConditionSpeciesSelectionIndex = -1;
	nActionSpeciesSelectionIndex = -1;
	nFailureConditionSpeciesSelectionIndex = -1;
	nFailureActionSpeciesSelectionIndex = -1;
}


void ObjectiveDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_OBJECTIVE_ADD_FAILURE_CONDITION_BUTTON, m_AddFailureConditionButton);
	DDX_Control(pDX, IDC_OBJECTIVE_FAILURE_ACTION_LIST, m_FailureActionList);
	DDX_Control(pDX, IDC_OBJECTIVE_FAILURE_CONDITION_LIST, m_FailureConditionList);
	DDX_Control(pDX, IDC_OBJECTIVE_TYPE_OF_FAILURE_ACTION_COMBOBOX, m_FailureActionComboBox);
	DDX_Control(pDX, IDC_OBJECTIVE_TYPE_OF_FAILURE_CONDITION_COMBOBOX, m_FailureConditionComboBox);
	DDX_Control(pDX, IDC_OBJECTIVEGROUP, m_modelGroup);
	DDX_Control(pDX, IDC_MECH2, m_Mech);
	DDX_Control(pDX, IDC_OBJECTIVE_ADD_CONDITION_BUTTON, m_AddButton);
	DDX_Control(pDX, IDC_OBJECTIVE_TYPE_OF_ACTION_COMBOBOX, m_ActionComboBox);
	DDX_Control(pDX, IDC_OBJECTIVE_TYPE_OF_CONDITION_COMBOBOX, m_ComboBox);
	DDX_Control(pDX, IDC_OBJECTIVE_ACTION_LIST, m_ActionList);
	DDX_Control(pDX, IDC_OBJECTIVE_CONDITION_LIST, m_List);
	DDX_Radio(pDX, IDC_OBJECTIVE_PRIMARY_RADIO, m_PriorityRadioButton);
	DDX_Text(pDX, IDC_OBJECTIVE_RESOURCE_POINTS_EDITBOX, m_ResourcePointsEdit);
	DDX_Check(pDX, IDC_OBJECTIVE_PREVIOUS_CHECK, m_PreviousPrimaryObjectiveMustBeCompleteCheckButton);
	DDX_Check(pDX, IDC_OBJECTIVE_ALL_PREVIOUS_CHECK, m_AllPreviousPrimaryObjectivesMustBeCompleteCheckButton);
	DDX_Check(pDX, IDC_OBJECTIVE_DISPLAY_MARKER_CHECK, m_DisplayMarkerCheckButton);
	DDX_Text(pDX, IDC_OBJECTIVE_X_EDITBOX, m_XEdit);
	DDX_Text(pDX, IDC_OBJECTIVE_Y_EDITBOX, m_YEdit);
	DDX_Check(pDX, IDC_OBJECTIVE_HIDDEN_TRIGGER_CHECK, m_HiddenTriggerCheckButton);
	DDX_Check(pDX, IDC_OBJECTIVE_ACTIVATE_ON_FLAG_CHECK, m_ActivateOnFlagCheckButton);
	DDX_Text(pDX, IDC_OBJECTIVE_TITLE_EDIT, m_TitleEdit);
	DDX_Text(pDX, IDC_OBJECTIVE_DESCRIPTION_EDIT, m_DescriptionEdit);
	DDX_Text(pDX, IDC_OBJECTIVE_TITLE_RESOURCE_STRING_ID_EDIT, m_TitleResourceStringIDEdit);
	DDX_Text(pDX, IDC_OBJECTIVE_DESCRIPTION_RESOURCE_STRING_ID_EDIT, m_DescriptionResourceStringIDEdit);
	DDX_Text(pDX, IDC_OBJECTIVE_ACTIVATE_FLAG_EDITBOX, m_ActivateFlagIDEdit);
	DDX_Check(pDX, IDC_OBJECTIVE_RESET_STATUS_ON_FLAG_CHECK, m_ResetStatusOnFlagCheckButton);
	DDX_Text(pDX, IDC_OBJECTIVE_RESET_STATUS_FLAG_EDITBOX, m_ResetStatusFlagIDEdit);
}


BEGIN_MESSAGE_MAP(ObjectiveDlg, CDialog)
	ON_BN_CLICKED(IDC_OBJECTIVE_ADD_CONDITION_BUTTON, &ObjectiveDlg::OnObjectiveAddConditionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_REMOVE_CONDITION_BUTTON, &ObjectiveDlg::OnObjectiveRemoveConditionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_ADD_ACTION_BUTTON, &ObjectiveDlg::OnObjectiveAddActionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_REMOVE_ACTION_BUTTON, &ObjectiveDlg::OnObjectiveRemoveActionButton)
	ON_CBN_SELCHANGE(IDC_OBJECTIVEGROUP, &ObjectiveDlg::OnSelchangeGroup)
	ON_BN_CLICKED(IDC_BASEEDIT2, &ObjectiveDlg::OnBaseedit2)
	ON_BN_CLICKED(IDC_HIGHILIGHT2EDIT2, &ObjectiveDlg::OnHighilight2edit2)
	ON_BN_CLICKED(IDC_HIGHLIGHT1EDIT, &ObjectiveDlg::OnHighlight1edit)
	ON_EN_CHANGE(IDC_HIGHLIGHT1, &ObjectiveDlg::OnChangeHighlight1)
	ON_EN_CHANGE(IDC_HIGHLIGHT2, &ObjectiveDlg::OnChangeHighlight2)
	ON_EN_CHANGE(IDC_BASE2, &ObjectiveDlg::OnChangeBase2)
	ON_WM_CTLCOLOR()
	ON_BN_CLICKED(IDC_OBJECTIVE_TITLE_EDIT_BUTTON, &ObjectiveDlg::OnObjectiveTitleEditButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_DESCRIPTION_EDIT_BUTTON, &ObjectiveDlg::OnObjectiveDescriptionEditButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_ADD_FAILURE_CONDITION_BUTTON, &ObjectiveDlg::OnObjectiveAddFailureConditionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_REMOVE_FAILURE_CONDITION_BUTTON, &ObjectiveDlg::OnObjectiveRemoveFailureConditionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_ADD_FAILURE_ACTION_BUTTON, &ObjectiveDlg::OnObjectiveAddFailureActionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_REMOVE_FAILURE_ACTION_BUTTON, &ObjectiveDlg::OnObjectiveRemoveFailureActionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_VIEW_CONDITION_BUTTON, &ObjectiveDlg::OnObjectiveViewConditionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_VIEW_ACTION_BUTTON, &ObjectiveDlg::OnObjectiveViewActionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_VIEW_FAILURE_CONDITION_BUTTON, &ObjectiveDlg::OnObjectiveViewFailureConditionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_VIEW_FAILURE_ACTION_BUTTON, &ObjectiveDlg::OnObjectiveViewFailureActionButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_CLEAR_LOGISTICS_MODEL_BUTTON, &ObjectiveDlg::OnObjectiveClearLogisticsModelButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_PICK_MARKER_BUTTON, &ObjectiveDlg::OnObjectivePickMarkerButton)
	ON_BN_CLICKED(IDC_OBJECTIVE_LOCATE_CONDITION_BUTTON, &ObjectiveDlg::OnObjectiveLocateConditionButton)
END_MESSAGE_MAP()


static void syncConditionsListWithListBox(const CObjective::condition_list_type *pConditionList, CListBox *pList) {
	pList->ResetContent();
	std::string tmpStr;
	auto it = pConditionList->entries.begin();
	while (it != pConditionList->entries.end())
	{
		tmpStr = objReadableCondition(*it);
		pList->AddString(tmpStr.c_str());
		it++;
	}
}

static void syncActionsListWithListBox(const CObjective::action_list_type *pActionList, CListBox *pList) {
	pList->ResetContent();
	std::string tmpStr;
	auto it = pActionList->entries.begin();
	while (it != pActionList->entries.end())
	{
		tmpStr = objReadableAction(*it);
		pList->AddString(tmpStr.c_str());
		it++;
	}
}

static BOOL CSLoadString(int resourceID, CString &targetStr) {
	char szTmp[16384/*max string length*/];
	EditorSafeLoadString( resourceID, szTmp, 16384/*max string length*/ );
	targetStr = szTmp;
	CString tmpStr;
	tmpStr.Format("mc2res.dll:%d Not defined", resourceID);
	if (0 == strcmp(tmpStr.GetBuffer(0), szTmp)) {
		return (0);
	}
	return (!0);
}

void ObjectiveDlg::UpdateTitleDisplay() {
	UpdateData(TRUE);
	if (m_TitleUseResourceString) {
		m_TitleResourceStringIDEdit.Format("%d", m_TitleResourceStringID);
		int ret = CSLoadString(m_TitleResourceStringID, m_TitleEdit);
		if (0 == ret) {
			m_TitleEdit = _TEXT("");
		}
	} else {
		m_TitleResourceStringIDEdit = _TEXT("");
		m_TitleEdit = m_TitleUnlocalizedText;
	}
	UpdateData(FALSE);
}

void ObjectiveDlg::UpdateDescriptionDisplay() {
	UpdateData(TRUE);
	if (m_DescriptionUseResourceString) {
		m_DescriptionResourceStringIDEdit.Format("%d", m_DescriptionResourceStringID);
		int ret = CSLoadString(m_DescriptionResourceStringID, m_DescriptionEdit);
		if (0 == ret) {
			m_DescriptionEdit = _TEXT("");
		}
	} else {
		m_DescriptionResourceStringIDEdit = _TEXT("");
		m_DescriptionEdit = m_DescriptionUnlocalizedText;
	}
	UpdateData(FALSE);
}

void ObjectiveDlg::LoadDialogValues() {
	(&m_ComboBox)->SetCurSel(nConditionSpeciesSelectionIndex);
	(&m_ActionComboBox)->SetCurSel(nActionSpeciesSelectionIndex);
	(&m_FailureConditionComboBox)->SetCurSel(nFailureConditionSpeciesSelectionIndex);
	(&m_FailureActionComboBox)->SetCurSel(nFailureActionSpeciesSelectionIndex);

	m_TitleUnlocalizedText = m_ModifiedObjective.Title().c_str();
	m_TitleUseResourceString = m_ModifiedObjective.TitleUseResourceString();
	m_TitleResourceStringID = m_ModifiedObjective.TitleResourceStringID();
	UpdateTitleDisplay();

	m_DescriptionUnlocalizedText = m_ModifiedObjective.Description().c_str();
	m_DescriptionUseResourceString = m_ModifiedObjective.DescriptionUseResourceString();
	m_DescriptionResourceStringID = m_ModifiedObjective.DescriptionResourceStringID();
	UpdateDescriptionDisplay();

	if (1 != m_ModifiedObjective.Priority()) {
		m_PriorityRadioButton = 1;
	} else {
		m_PriorityRadioButton = 0;
	}

	if (m_ModifiedObjective.PreviousPrimaryObjectiveMustBeComplete()) {
		m_PreviousPrimaryObjectiveMustBeCompleteCheckButton = TRUE;
	} else {
		m_PreviousPrimaryObjectiveMustBeCompleteCheckButton = FALSE;
	}

	if (m_ModifiedObjective.AllPreviousPrimaryObjectivesMustBeComplete()) {
		m_AllPreviousPrimaryObjectivesMustBeCompleteCheckButton = TRUE;
	} else {
		m_AllPreviousPrimaryObjectivesMustBeCompleteCheckButton = FALSE;
	}

	m_ResourcePointsEdit = m_ModifiedObjective.ResourcePoints();

	if (m_ModifiedObjective.DisplayMarker()) {
		m_DisplayMarkerCheckButton = TRUE;
	} else {
		m_DisplayMarkerCheckButton = FALSE;
	}

	m_XEdit = m_ModifiedObjective.MarkerX();
	m_YEdit = m_ModifiedObjective.MarkerY();

	if (m_ModifiedObjective.IsHiddenTrigger()) {
		m_HiddenTriggerCheckButton = TRUE;
	} else {
		m_HiddenTriggerCheckButton = FALSE;
	}

	if (m_ModifiedObjective.ActivateOnFlag()) {
		m_ActivateOnFlagCheckButton = TRUE;
	} else {
		m_ActivateOnFlagCheckButton = FALSE;
	}
	m_ActivateFlagIDEdit = m_ModifiedObjective.ActivateFlagID().c_str();

	if (m_ModifiedObjective.ResetStatusOnFlag()) {
		m_ResetStatusOnFlagCheckButton = TRUE;
	} else {
		m_ResetStatusOnFlagCheckButton = FALSE;
	}
	m_ResetStatusFlagIDEdit = m_ModifiedObjective.ResetStatusFlagID().c_str();

	syncConditionsListWithListBox((&m_ModifiedObjective.m_conditions), (&m_List));
	// Give Locate a target when success conditions exist.
	if ((m_List.GetCount() > 0) && (LB_ERR == m_List.GetCurSel())) {
		m_List.SetCurSel(0);
	}
	syncActionsListWithListBox(&(m_ModifiedObjective.m_actionList), (&m_ActionList));
	syncConditionsListWithListBox(&(m_ModifiedObjective.m_failureConditionList), (&m_FailureConditionList));
	syncActionsListWithListBox(&(m_ModifiedObjective.m_failureActionList), (&m_FailureActionList));

	int color = m_ModifiedObjective.BaseColor();
	char tmp[256];
	sprintf( tmp, "0x%x", color );
	GetDlgItem( IDC_BASE2 )->SetWindowText( tmp );

	color = m_ModifiedObjective.HighlightColor();
	sprintf( tmp, "0x%x", color );
	GetDlgItem( IDC_HIGHLIGHT1 )->SetWindowText( tmp );

	color = m_ModifiedObjective.HighlightColor2();
	sprintf( tmp, "0x%x", color );
	GetDlgItem( IDC_HIGHLIGHT2 )->SetWindowText( tmp );

	int modelID = m_ModifiedObjective.ModelID();

	if ( modelID != -1 )
	{
		unsigned long ulGroup, ulIndex;
		EditorObjectMgr::instance()->getBuildingFromID( modelID, ulGroup, ulIndex, true);
		m_modelGroup.SetCurSel( ulGroup );
		OnSelchangeGroup();
		m_Mech.SetCurSel( ulIndex );
	}

	UpdateData(FALSE);
}

void ObjectiveDlg::SaveDialogValues() {
	UpdateData(TRUE);

	m_ModifiedObjective.Title(mc2::str::from_c(m_TitleUnlocalizedText.GetBuffer(0)));
	m_ModifiedObjective.TitleUseResourceString(TRUE == m_TitleUseResourceString);
	m_ModifiedObjective.TitleResourceStringID(m_TitleResourceStringID);

	m_ModifiedObjective.Description(mc2::str::from_c(m_DescriptionUnlocalizedText.GetBuffer(0)));
	m_ModifiedObjective.DescriptionUseResourceString(TRUE == m_DescriptionUseResourceString);
	m_ModifiedObjective.DescriptionResourceStringID(m_DescriptionResourceStringID);

	if (0 == m_PriorityRadioButton) {
		m_ModifiedObjective.Priority(1);
	} else {
		m_ModifiedObjective.Priority(2);
	}

	m_ModifiedObjective.ResourcePoints(m_ResourcePointsEdit);

	if (m_PreviousPrimaryObjectiveMustBeCompleteCheckButton) {
		m_ModifiedObjective.PreviousPrimaryObjectiveMustBeComplete(true);
	} else {
		m_ModifiedObjective.PreviousPrimaryObjectiveMustBeComplete(false);
	}

	if (m_AllPreviousPrimaryObjectivesMustBeCompleteCheckButton) {
		m_ModifiedObjective.AllPreviousPrimaryObjectivesMustBeComplete(true);
	} else {
		m_ModifiedObjective.AllPreviousPrimaryObjectivesMustBeComplete(false);
	}

	if (m_DisplayMarkerCheckButton) {
		m_ModifiedObjective.DisplayMarker(true);
	} else {
		m_ModifiedObjective.DisplayMarker(false);
	}

	m_ModifiedObjective.MarkerX(m_XEdit);
	m_ModifiedObjective.MarkerY(m_YEdit);

	if (m_HiddenTriggerCheckButton) {
		m_ModifiedObjective.IsHiddenTrigger(true);
	} else {
		m_ModifiedObjective.IsHiddenTrigger(false);
	}

	if (m_ActivateOnFlagCheckButton) {
		m_ModifiedObjective.ActivateOnFlag(true);
	} else {
		m_ModifiedObjective.ActivateOnFlag(false);
	}
	m_ModifiedObjective.ActivateFlagID(mc2::str::from_c(m_ActivateFlagIDEdit.GetBuffer(0)));

	if (m_ResetStatusOnFlagCheckButton) {
		m_ModifiedObjective.ResetStatusOnFlag(true);
	} else {
		m_ModifiedObjective.ResetStatusOnFlag(false);
	}
	m_ModifiedObjective.ResetStatusFlagID(mc2::str::from_c(m_ResetStatusFlagIDEdit.GetBuffer(0)));

	long base, highlight1, highlight2;

	CWnd* pWnd = GetDlgItem( IDC_BASE2 );
	if ( pWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		if ( tmpStr.GetLength() )
		{
			tmpStr.Replace( "0x", "" );
			sscanf( tmpStr, "%x", &base );
			base |= 0xff000000;
			m_ModifiedObjective.BaseColor( base );
		}
		
	}

	pWnd = GetDlgItem( IDC_HIGHLIGHT1 );
	if ( pWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		if ( tmpStr.GetLength() )
		{
			tmpStr.Replace( "0x", "" );
			sscanf( tmpStr, "%x", &highlight1 );
			highlight1 |= 0xff000000;
			m_ModifiedObjective.HighlightColor( highlight1 );
		}
		
	}

	pWnd = GetDlgItem( IDC_HIGHLIGHT2 );
	if ( pWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		if ( tmpStr.GetLength() )
		{
			tmpStr.Replace( "0x", "" );
			sscanf( tmpStr, "%x", &highlight2 );
			highlight2 |= 0xff000000;
			m_ModifiedObjective.HighlightColor2( highlight2 );
		}
		
	}

	int group = m_modelGroup.GetCurSel();
	int index = m_Mech.GetCurSel();

	if ( group != -1  && index != -1 )
	{

		int ID = EditorObjectMgr::instance()->getID( group, index );
		int fitID = EditorObjectMgr::instance()->getFitID(ID);

		m_ModifiedObjective.ModelID( fitID );
	}
	else 
		m_ModifiedObjective.ModelID( -1 );
	
}

BOOL ObjectiveDlg::OnInitDialog() 
{
	CDialog::OnInitDialog();
	EditorObjectMgr* pMgr = EditorObjectMgr::instance();
	
	int groupCount = pMgr->getBuildingGroupCount();

	const char** pGroups = new const char*[groupCount];
		
	pMgr->getBuildingGroupNames(pGroups, groupCount);


	for ( int i = 0; i < groupCount; ++i )
	{
		m_modelGroup.AddString( pGroups[i] );
		m_modelGroup.SetItemData( i, (DWORD)i );
	}

	delete [] pGroups;

	{

		{
			comboBoxItems.clear();
			comboBoxItems.push_back(DESTROY_ALL_ENEMY_UNITS);
			comboBoxItems.push_back(DESTROY_NUMBER_OF_ENEMY_UNITS);
			comboBoxItems.push_back(DESTROY_SPECIFIC_ENEMY_UNIT);
			comboBoxItems.push_back(DESTROY_SPECIFIC_STRUCTURE);
			comboBoxItems.push_back(CAPTURE_OR_DESTROY_ALL_ENEMY_UNITS);
			comboBoxItems.push_back(CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS);
			comboBoxItems.push_back(CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT);
			comboBoxItems.push_back(CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE);
			comboBoxItems.push_back(DEAD_OR_FLED_ALL_ENEMY_UNITS);
			comboBoxItems.push_back(DEAD_OR_FLED_NUMBER_OF_ENEMY_UNITS);
			comboBoxItems.push_back(DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT);
			comboBoxItems.push_back(CAPTURE_UNIT);
			comboBoxItems.push_back(CAPTURE_STRUCTURE);
			comboBoxItems.push_back(GUARD_SPECIFIC_UNIT);
			comboBoxItems.push_back(GUARD_SPECIFIC_STRUCTURE);
			comboBoxItems.push_back(MOVE_ANY_UNIT_TO_AREA);
			comboBoxItems.push_back(MOVE_ALL_UNITS_TO_AREA);
			comboBoxItems.push_back(MOVE_ALL_SURVIVING_UNITS_TO_AREA);
			comboBoxItems.push_back(MOVE_ALL_SURVIVING_MECHS_TO_AREA);
			comboBoxItems.push_back(BOOLEAN_FLAG_IS_SET);
			comboBoxItems.push_back(ELAPSED_MISSION_TIME);
			std::string tmpEStr;
			for (const auto species : comboBoxItems) {
				tmpEStr = m_ModifiedObjective.DescriptionOfConditionSpecies(species);
				(&m_ComboBox)->AddString(tmpEStr.c_str());
			}
			(&m_ComboBox)->SetCurSel(0);
		}

		{
			actionComboBoxItems.clear();
			actionComboBoxItems.push_back(PLAY_BIK);
			actionComboBoxItems.push_back(PLAY_WAV);
			actionComboBoxItems.push_back(DISPLAY_TEXT_MESSAGE);
			actionComboBoxItems.push_back(DISPLAY_RESOURCE_TEXT_MESSAGE);
			actionComboBoxItems.push_back(SET_BOOLEAN_FLAG);
			actionComboBoxItems.push_back(MAKE_NEW_TECHNOLOGY_AVAILABLE);
			std::string tmpEStr;
			for (const auto species : actionComboBoxItems) {
				tmpEStr = m_ModifiedObjective.DescriptionOfActionSpecies(species);
				(&m_ActionComboBox)->AddString(tmpEStr.c_str());
			}
			(&m_ActionComboBox)->SetCurSel(0);
		}

		{
			failureConditionComboBoxItems.clear();
			failureConditionComboBoxItems.push_back(DESTROY_ALL_ENEMY_UNITS);
			failureConditionComboBoxItems.push_back(DESTROY_NUMBER_OF_ENEMY_UNITS);
			failureConditionComboBoxItems.push_back(DESTROY_SPECIFIC_ENEMY_UNIT);
			failureConditionComboBoxItems.push_back(DESTROY_SPECIFIC_STRUCTURE);
			failureConditionComboBoxItems.push_back(CAPTURE_OR_DESTROY_ALL_ENEMY_UNITS);
			failureConditionComboBoxItems.push_back(CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS);
			failureConditionComboBoxItems.push_back(CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT);
			failureConditionComboBoxItems.push_back(CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE);
			failureConditionComboBoxItems.push_back(DEAD_OR_FLED_ALL_ENEMY_UNITS);
			failureConditionComboBoxItems.push_back(DEAD_OR_FLED_NUMBER_OF_ENEMY_UNITS);
			failureConditionComboBoxItems.push_back(DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT);
			failureConditionComboBoxItems.push_back(CAPTURE_UNIT);
			failureConditionComboBoxItems.push_back(CAPTURE_STRUCTURE);
			failureConditionComboBoxItems.push_back(GUARD_SPECIFIC_UNIT);
			failureConditionComboBoxItems.push_back(GUARD_SPECIFIC_STRUCTURE);
			failureConditionComboBoxItems.push_back(MOVE_ANY_UNIT_TO_AREA);
			failureConditionComboBoxItems.push_back(MOVE_ALL_UNITS_TO_AREA);
			failureConditionComboBoxItems.push_back(MOVE_ALL_SURVIVING_UNITS_TO_AREA);
			failureConditionComboBoxItems.push_back(MOVE_ALL_SURVIVING_MECHS_TO_AREA);
			failureConditionComboBoxItems.push_back(BOOLEAN_FLAG_IS_SET);
			failureConditionComboBoxItems.push_back(ELAPSED_MISSION_TIME);
			std::string tmpEStr;
			for (const auto species : failureConditionComboBoxItems) {
				tmpEStr = m_ModifiedObjective.DescriptionOfConditionSpecies(species);
				(&m_FailureConditionComboBox)->AddString(tmpEStr.c_str());
			}
			(&m_FailureConditionComboBox)->SetCurSel(0);
		}

		{
			failureActionComboBoxItems.clear();
			failureActionComboBoxItems.push_back(PLAY_BIK);
			failureActionComboBoxItems.push_back(PLAY_WAV);
			failureActionComboBoxItems.push_back(DISPLAY_TEXT_MESSAGE);
			failureActionComboBoxItems.push_back(DISPLAY_RESOURCE_TEXT_MESSAGE);
			failureActionComboBoxItems.push_back(SET_BOOLEAN_FLAG);
			failureActionComboBoxItems.push_back(MAKE_NEW_TECHNOLOGY_AVAILABLE);
			std::string tmpEStr;
			for (const auto species : failureActionComboBoxItems) {
				tmpEStr = m_ModifiedObjective.DescriptionOfActionSpecies(species);
				(&m_FailureActionComboBox)->AddString(tmpEStr.c_str());
			}
			(&m_FailureActionComboBox)->SetCurSel(0);
		}
	}

	LoadDialogValues();

	// A delivered marker pick must not reopen a condition picker.
	{
		EditorInterface *pEditor = EditorInterface::instance();
		// Area picks belong to TargetAreaDlg; consuming one here would corrupt the marker.
		if ((0 != pEditor) && pEditor->objectivesEditState.pendingPickResultReady &&
		    (CObjectivesEditState::PICK_MARKER == pEditor->objectivesEditState.pendingPickTarget)) {
			m_XEdit = pEditor->objectivesEditState.pendingPickX;
			m_YEdit = pEditor->objectivesEditState.pendingPickY;
			m_ModifiedObjective.MarkerX(m_XEdit);
			m_ModifiedObjective.MarkerY(m_YEdit);
			// A picked marker should be visible.
			m_ModifiedObjective.DisplayMarker(true);
			m_DisplayMarkerCheckButton = TRUE;
			UpdateData(FALSE);

			pEditor->objectivesEditState.pendingPickResultReady = false;
			pEditor->objectivesEditState.pendingPickPoint = false;
			pEditor->objectivesEditState.pendingPickTarget = CObjectivesEditState::PICK_NONE;
			pEditor->ObjectSelectOnlyMode(false);
			return TRUE;
		}
	}

	// A cancelled marker pick leaves the normal dialog active; area picks remain pending.
	if (EditorInterface::instance() &&
	    EditorInterface::instance()->objectivesEditState.pendingPickPoint &&
	    !EditorInterface::instance()->objectivesEditState.pendingPickResultReady &&
	    (CObjectivesEditState::PICK_MARKER == EditorInterface::instance()->objectivesEditState.pendingPickTarget))
	{
		EditorInterface::instance()->objectivesEditState.pendingPickPoint = false;
		EditorInterface::instance()->objectivesEditState.pendingPickTarget = CObjectivesEditState::PICK_NONE;
		EditorInterface::instance()->ObjectSelectOnlyMode(false);
	}

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		if (CObjectivesEditState::SUCCESS_CONDITION == EditorInterface::instance()->objectivesEditState.listID) {
			PostMessage(WM_COMMAND, MAKEWPARAM(IDC_OBJECTIVE_ADD_CONDITION_BUTTON, BN_CLICKED), (LPARAM)((&m_AddButton)->m_hWnd));
		} else {
			PostMessage(WM_COMMAND, MAKEWPARAM(IDC_OBJECTIVE_ADD_FAILURE_CONDITION_BUTTON, BN_CLICKED), (LPARAM)((&m_AddFailureConditionButton)->m_hWnd));
		}
	} else {
		EditorInterface::instance()->objectivesEditState.ModifiedObjective = m_ModifiedObjective;
	}

	return TRUE;  // return TRUE unless you set the focus to a control
}

// Marker XY map-picker.
// Mirrors the ObjectSelectOnlyMode handoff used by the condition object pickers
// (OnObjectiveAddConditionButton): save the in-progress objective into the
// editor singleton, flip the editor into selection mode, then EndDialog so the
// whole nested-modal stack unwinds and the editor render loop runs. The next
// terrain left-click is consumed by EditorInterface::handleLeftButtonDown, which
// writes the picked world XY into objectivesEditState.pendingPickX/Y, sets
// pendingPickResultReady, and re-opens the objectives dialog chain (Team ->
// CObjectives::EditDialog -> ObjectivesDlg re-posts EDIT -> CObjective::EditDialog
// -> this dialog). OnInitDialog then loads the picked XY exactly once.
void ObjectiveDlg::OnObjectivePickMarkerButton()
{
	EditorInterface *pEditor = EditorInterface::instance();
	if (0 == pEditor) {
		return;
	}

	// Capture the current control state (incl. the X/Y editboxes and the marker
	// checkbox) into m_ModifiedObjective so nothing the user typed is lost.
	SaveDialogValues();

	pEditor->objectivesEditState.ModifiedObjective = m_ModifiedObjective;
	// Restore the same combo selections the ordinary object-pick handoff stashes,
	// so the re-opened dialog comes back to the same condition/action species.
	// NOTE: objectiveFunction (ADD vs EDIT) is intentionally NOT set here -- the
	// parent ObjectivesDlg::OnObjectives{Add,Edit}Button handler that opened this
	// ObjectiveDlg sets it during its own ObjectSelectOnlyMode unwind (see
	// OnObjectivesAddButton/OnObjectivesEditButton), so forcing it here would break
	// the ADD re-open path.
	pEditor->objectivesEditState.listID = CObjectivesEditState::SUCCESS_CONDITION;
	pEditor->objectivesEditState.nConditionSpeciesSelectionIndex = (&m_ComboBox)->GetCurSel();
	pEditor->objectivesEditState.nActionSpeciesSelectionIndex = (&m_ActionComboBox)->GetCurSel();
	pEditor->objectivesEditState.nFailureConditionSpeciesSelectionIndex = (&m_FailureConditionComboBox)->GetCurSel();
	pEditor->objectivesEditState.nFailureActionSpeciesSelectionIndex = (&m_FailureActionComboBox)->GetCurSel();

	// Arm the one-shot terrain point pick. Result delivery clears pendingPickPoint
	// and sets pendingPickResultReady (consumed by OnInitDialog). Tag as a MARKER
	// pick so the OnInitDialog consumer (which shares the pick fields with the
	// area-center pick) only fires for a marker pick.
	pEditor->objectivesEditState.pendingPickPoint = true;
	pEditor->objectivesEditState.pendingPickResultReady = false;
	pEditor->objectivesEditState.pendingPickTarget = CObjectivesEditState::PICK_MARKER;

	// Put the editor into selection mode and the objective-select-only handoff
	// mode, exactly like the unit/building pickers, so the re-open machinery fires.
	pEditor->SelectionMode();
	pEditor->ObjectSelectOnlyMode(true);

	EndDialog(IDOK);
}

// Locate returns to the map before moving the camera so the modal dialog cannot block it.
void ObjectiveDlg::OnObjectiveLocateConditionButton()
{
	EditorInterface *pEditor = EditorInterface::instance();
	if (0 == pEditor) {
		return;
	}

	int nSelectionIndex = m_List.GetCurSel();
	if ((LB_ERR == nSelectionIndex) || (0 > nSelectionIndex) ||
	    (m_ModifiedObjective.m_conditions.entries.size() <= nSelectionIndex)) {
		AfxMessageBox(_T("Select a condition first, then Locate."));
		return;
	}
	auto it = m_ModifiedObjective.m_conditions.entries.begin();
	for (int i = 0; i < nSelectionIndex && it != m_ModifiedObjective.m_conditions.entries.end(); ++i) it++;
	if (it == m_ModifiedObjective.m_conditions.entries.end()) {
		return;
	}
	CObjectiveCondition *pCond = *it;
	if (0 == pCond) {
		return;
	}

	// Extract target XY + (guarded) live object ptr. Default: not locatable.
	bool          haveTarget = false;
	float         locX = 0.0f, locY = 0.0f;
	EditorObject *pLocObj = 0;

	if (CSpecificUnitObjectiveCondition *pU =
			dynamic_cast<CSpecificUnitObjectiveCondition *>(pCond)) {
		// NotedX/Y are always valid (plain floats restored at load/save).
		locX = pU->NotedX();
		locY = pU->NotedY();
		haveTarget = true;
		Unit *pUnit = pU->UnitPtr();   // may be null (stale/deleted)
		// Only forward the live ptr if it is safe to deref AND select: appearance()
		// must be non-null (getPosition derefs appearance()->position unguarded).
		if (pUnit && ((EditorObject *)pUnit)->appearance()) {
			pLocObj = (EditorObject *)pUnit;
			const Stuff::Vector3D &pos = pLocObj->getPosition();
			locX = pos.x;
			locY = pos.y;
		}
	} else if (CSpecificStructureObjectiveCondition *pS =
			dynamic_cast<CSpecificStructureObjectiveCondition *>(pCond)) {
		locX = pS->NotedX();
		locY = pS->NotedY();
		haveTarget = true;
		EditorObject *pBld = pS->BuildingPtr();   // may be null (stale/deleted)
		if (pBld && pBld->appearance()) {
			pLocObj = pBld;
			const Stuff::Vector3D &pos = pLocObj->getPosition();
			locX = pos.x;
			locY = pos.y;
		}
	} else if (CAreaObjectiveCondition *pA =
			dynamic_cast<CAreaObjectiveCondition *>(pCond)) {
		// Area targets have a location without a selectable object.
		locX = pA->TargetCenterX();
		locY = pA->TargetCenterY();
		haveTarget = true;
	}

	if (!haveTarget && (0 != m_ModifiedObjective.DisplayMarker())) {
		// The objective marker supplies a location when the condition has none.
		locX = m_ModifiedObjective.MarkerX();
		locY = m_ModifiedObjective.MarkerY();
		haveTarget = true;
	}

	if (!haveTarget) {
		AfxMessageBox(_T("This condition type has no map location to locate."));
		return;
	}

	// Keep dialog edits while Locate temporarily returns control to the map.
	SaveDialogValues();
	pEditor->objectivesEditState.ModifiedObjective = m_ModifiedObjective;
	pEditor->objectivesEditState.listID = CObjectivesEditState::SUCCESS_CONDITION;
	pEditor->objectivesEditState.nConditionSpeciesSelectionIndex = (&m_ComboBox)->GetCurSel();
	pEditor->objectivesEditState.nActionSpeciesSelectionIndex = (&m_ActionComboBox)->GetCurSel();
	pEditor->objectivesEditState.nFailureConditionSpeciesSelectionIndex = (&m_FailureConditionComboBox)->GetCurSel();
	pEditor->objectivesEditState.nFailureActionSpeciesSelectionIndex = (&m_FailureActionComboBox)->GetCurSel();

	// Locate runs after the modal dialog returns, when the map is available.
	pEditor->objectivesEditState.pendingLocate = true;
	pEditor->objectivesEditState.pendingLocateX = locX;
	pEditor->objectivesEditState.pendingLocateY = locY;
	pEditor->objectivesEditState.pendingLocateObj = pLocObj;   // may be null -> camera only

	pEditor->SelectionMode();
	pEditor->ObjectSelectOnlyMode(true);

	EndDialog(IDOK);
}

void ObjectiveDlg::OnObjectiveAddConditionButton()
{
	{
		DWORD comboboxSelection = (&m_ComboBox)->GetCurSel();
		if ((0 <= comboboxSelection) && (comboBoxItems.size() > comboboxSelection)) {
			condition_species_type species = comboBoxItems[comboboxSelection];
			pNewCondition = CObjective::new_CObjectiveCondition(species, m_ModifiedObjective.Alignment());
		} else {
			return;
		}
	}
	assert(pNewCondition);

	// Editing a condition or action may require a map selection.
	bool result = pNewCondition->EditDialog();

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		SaveDialogValues();
		EditorInterface::instance()->objectivesEditState.ModifiedObjective = m_ModifiedObjective;
		EditorInterface::instance()->objectivesEditState.listID = CObjectivesEditState::SUCCESS_CONDITION;
		EditorInterface::instance()->objectivesEditState.nConditionSpeciesSelectionIndex = (&m_ComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nActionSpeciesSelectionIndex = (&m_ActionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureConditionSpeciesSelectionIndex = (&m_FailureConditionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureActionSpeciesSelectionIndex = (&m_FailureActionComboBox)->GetCurSel();
		delete pNewCondition; pNewCondition = 0;
		EndDialog(IDOK);
		return;
	} else {
		if (true == result) {
			m_ModifiedObjective.m_conditions.entries.push_back(pNewCondition);
			m_List.SetCurSel(m_List.GetCount() - 1);
			syncConditionsListWithListBox((&m_ModifiedObjective.m_conditions), (&m_List));
		} else {
			delete pNewCondition; pNewCondition = 0;
		}
		return;
	}
}

void ObjectiveDlg::OnObjectiveRemoveConditionButton() 
{
	int nSelectionIndex = (&m_List)->GetCurSel();
	if ((0 <= nSelectionIndex) && (m_ModifiedObjective.m_conditions.entries.size() > nSelectionIndex)) {
		delete m_ModifiedObjective.m_conditions.entries[nSelectionIndex];
		m_ModifiedObjective.m_conditions.entries.erase(m_ModifiedObjective.m_conditions.entries.begin() + nSelectionIndex);
		syncConditionsListWithListBox((&m_ModifiedObjective.m_conditions), (&m_List));
		if (0 < m_List.GetCount()) {
			if (m_List.GetCount() <= (long)nSelectionIndex) {
				nSelectionIndex = m_List.GetCount() - 1;
			}
			m_List.SetCurSel(nSelectionIndex);
		}
	}
}

void ObjectiveDlg::OnObjectiveAddActionButton() 
{
	{
		DWORD actionComboboxSelection = (&m_ActionComboBox)->GetCurSel();
		if ((0 <= actionComboboxSelection) && (actionComboBoxItems.size() > actionComboboxSelection)) {
			action_species_type species = actionComboBoxItems[actionComboboxSelection];
			pNewAction = CObjective::new_CObjectiveAction(species, m_ModifiedObjective.Alignment());
		} else {
			return;
		}
	}
	assert(pNewAction);

	// Editing a condition or action may require a map selection.
	bool result = pNewAction->EditDialog();

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		SaveDialogValues();
		EditorInterface::instance()->objectivesEditState.ModifiedObjective = m_ModifiedObjective;
		EditorInterface::instance()->objectivesEditState.nConditionSpeciesSelectionIndex = (&m_ComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nActionSpeciesSelectionIndex = (&m_ActionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureConditionSpeciesSelectionIndex = (&m_FailureConditionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureActionSpeciesSelectionIndex = (&m_FailureActionComboBox)->GetCurSel();
		delete pNewAction; pNewAction = 0;
		EndDialog(IDOK);
		return;
	} else {
		if (true == result) {
			m_ModifiedObjective.m_actionList.entries.push_back(pNewAction);
			m_ActionList.SetCurSel(m_ActionList.GetCount() - 1);
			syncActionsListWithListBox(&(m_ModifiedObjective.m_actionList), (&m_ActionList));
		} else {
			delete pNewAction; pNewAction = 0;
		}
		return;
	}
}

void ObjectiveDlg::OnObjectiveRemoveActionButton() 
{
	int nSelectionIndex = (&m_ActionList)->GetCurSel();
	if ((0 <= nSelectionIndex) && (m_ModifiedObjective.m_actionList.entries.size() > nSelectionIndex)) {
		delete m_ModifiedObjective.m_actionList.entries[nSelectionIndex];
		m_ModifiedObjective.m_actionList.entries.erase(m_ModifiedObjective.m_actionList.entries.begin() + nSelectionIndex);
		syncActionsListWithListBox(&(m_ModifiedObjective.m_actionList), (&m_ActionList));
		if (0 < m_ActionList.GetCount()) {
			if (m_ActionList.GetCount() <= (long)nSelectionIndex) {
				nSelectionIndex = m_ActionList.GetCount() - 1;
			}
			m_ActionList.SetCurSel(nSelectionIndex);
		}
	}
}

void ObjectiveDlg::OnOK() 
{
	SaveDialogValues();
	CDialog::OnOK();
}

void ObjectiveDlg::OnSelchangeGroup() 
{
	m_Mech.ResetContent();

	int group = m_modelGroup.GetCurSel();
	group = m_modelGroup.GetItemData( group );

	const char* MechNames[256];
	int count = 256;
		
	EditorObjectMgr::instance()->getBuildingNamesInGroup( group, MechNames, count );

	for ( int i = 0; i < count; ++i )
	{
		m_Mech.AddString( MechNames[i] );
	}


	m_Mech.SetCurSel( 0 );	
	
}

void ObjectiveDlg::OnBaseedit2() 
{
	CWnd* pWnd = GetDlgItem( IDC_BASE2 );
	DoColorBox( pWnd );

	
}

void ObjectiveDlg::OnHighilight2edit2() 
{
	CWnd* pWnd = GetDlgItem( IDC_HIGHLIGHT2 );
	DoColorBox( pWnd );

	
}

void ObjectiveDlg::OnHighlight1edit() 
{
	CWnd* pWnd = GetDlgItem( IDC_HIGHLIGHT1 );
	DoColorBox( pWnd );
	
}

void ObjectiveDlg::DoEditColorChange( long ID )
{
	CString text;
	GetDlgItem( ID )->GetWindowText( text );

	bool bChanged = false;
	int i = 0;
	
	if ( text.GetLength() > 1 && (text[0] == '0' && (text[1] == 'x' || text[i] == 'X' )) )
		i = 2;

	for ( ; i < text.GetLength(); ++i )
	{
		if ( !isxdigit( text[i] ) )
		{
			text.Remove( text[i] );
			bChanged = true;
		}
	}

	if ( bChanged )
		GetDlgItem( ID )->SetWindowText( text );

	GetDlgItem( ID )->RedrawWindow( );
	
}

void ObjectiveDlg::OnChangeHighlight1() 
{
	DoEditColorChange( IDC_HIGHLIGHT1 );
	
}

void ObjectiveDlg::OnChangeHighlight2() 
{
	DoEditColorChange( IDC_HIGHLIGHT2 );
	
}

void ObjectiveDlg::OnChangeBase2() 
{
	DoEditColorChange( IDC_BASE2 );
	
}

void ObjectiveDlg::DoColorBox( CWnd* pWnd )
{
	if ( pWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		tmpStr.Replace( "0x", "" );

		long base;
		sscanf( tmpStr, "%x", &base );
		base &= 0x00ffffff;


		CColorDialog dlg( reverseRGB(base), NULL, this );
		if (IDOK == dlg.DoModal() )
		{
			base = reverseRGB( dlg.GetColor() );

			tmpStr.Format( "0x%x", base );
			pWnd->SetWindowText( tmpStr );
		}
	}
}



HBRUSH ObjectiveDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) 
{
	HBRUSH hbr = CDialog::OnCtlColor(pDC, pWnd, nCtlColor);
	
	if ( GetDlgItem( IDC_BASE2 )->m_hWnd == pWnd->m_hWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		tmpStr.Replace( "0x", "" );

		long base;
		sscanf( tmpStr, "%x", &base );
		base &= 0x00ffffff;
		base = reverseRGB( base );
		
		if ( baseBrush.m_hObject )
			baseBrush.DeleteObject();

		baseBrush.CreateSolidBrush( base );
		pDC->SetBkColor( base );

		
		if ( ((base & 0xff) + ( (base & 0xff00)>>8 ) + ( (base & 0xff0000)>>16 ))/3 < 85 )
			pDC->SetTextColor( 0x00ffffff );

		return (HBRUSH)baseBrush.m_hObject;
	}

	if ( GetDlgItem( IDC_HIGHLIGHT1 )->m_hWnd == pWnd->m_hWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );

		tmpStr.Replace( "0x", "" );

		long base;
		sscanf( tmpStr, "%x", &base );
		base &= 0x00ffffff;
		base = reverseRGB( base );

		if ( brush1.m_hObject )
			brush1.DeleteObject();

		brush1.CreateSolidBrush( base );
		pDC->SetBkColor( base );

		if ( ((base & 0xff) + ( (base & 0xff00)>>8 ) + ( (base & 0xff0000)>>16 ))/3 < 85 )
			pDC->SetTextColor( 0x00ffffff );
		return (HBRUSH)brush1.m_hObject;
	}

	if ( GetDlgItem( IDC_HIGHLIGHT2 )->m_hWnd == pWnd->m_hWnd )
	{
		CString tmpStr;
		pWnd->GetWindowText( tmpStr );
		
		tmpStr.Replace( "0x", "" );

		long base;
		sscanf( tmpStr, "%x", &base );
		base &= 0x00ffffff;
		base = reverseRGB( base );

		
		if ( brush2.m_hObject )
			brush2.DeleteObject();

		brush2.CreateSolidBrush( base );
		pDC->SetBkColor( base );
		
		if ( ((base & 0xff) + ( (base & 0xff00)>>8 ) + ( (base & 0xff0000)>>16 ))/3 < 85 )
			pDC->SetTextColor( 0x00ffffff );

		return (HBRUSH)brush2.m_hObject;
	}

		
	
	return hbr;
}

void ObjectiveDlg::OnObjectiveTitleEditButton() 
{
	CUserTextEdit userTextEditDialog;
	userTextEditDialog.m_UnlocalizedText = m_TitleUnlocalizedText;
	userTextEditDialog.m_UseResourceString = m_TitleUseResourceString;
	userTextEditDialog.m_ResourceStringID = m_TitleResourceStringID;
	int ret = userTextEditDialog.DoModal();
	if (IDOK == ret) {
		m_TitleUnlocalizedText = userTextEditDialog.m_UnlocalizedText;
		m_TitleUseResourceString = userTextEditDialog.m_UseResourceString;
		m_TitleResourceStringID = userTextEditDialog.m_ResourceStringID;
		UpdateTitleDisplay();
	}
}

void ObjectiveDlg::OnObjectiveDescriptionEditButton() 
{
	CUserTextEdit userTextEditDialog;
	userTextEditDialog.m_UnlocalizedText = m_DescriptionUnlocalizedText;
	userTextEditDialog.m_UseResourceString = m_DescriptionUseResourceString;
	userTextEditDialog.m_ResourceStringID = m_DescriptionResourceStringID;
	int ret = userTextEditDialog.DoModal();
	if (IDOK == ret) {
		m_DescriptionUnlocalizedText = userTextEditDialog.m_UnlocalizedText;
		m_DescriptionUseResourceString = userTextEditDialog.m_UseResourceString;
		m_DescriptionResourceStringID = userTextEditDialog.m_ResourceStringID;
		UpdateDescriptionDisplay();
	}
}

void ObjectiveDlg::OnObjectiveAddFailureConditionButton() 
{
	{
		DWORD comboboxSelection = (&m_FailureConditionComboBox)->GetCurSel();
		if ((0 <= comboboxSelection) && (failureConditionComboBoxItems.size() > comboboxSelection)) {
			condition_species_type species = failureConditionComboBoxItems[comboboxSelection];
			pNewCondition = CObjective::new_CObjectiveCondition(species, m_ModifiedObjective.Alignment());
		} else {
			return;
		}
	}
	assert(pNewCondition);

	// Editing a condition or action may require a map selection.
	bool result = pNewCondition->EditDialog();

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		SaveDialogValues();
		EditorInterface::instance()->objectivesEditState.ModifiedObjective = m_ModifiedObjective;
		EditorInterface::instance()->objectivesEditState.listID = CObjectivesEditState::FAILURE_CONDITION;
		EditorInterface::instance()->objectivesEditState.nConditionSpeciesSelectionIndex = (&m_ComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nActionSpeciesSelectionIndex = (&m_ActionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureConditionSpeciesSelectionIndex = (&m_FailureConditionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureActionSpeciesSelectionIndex = (&m_FailureActionComboBox)->GetCurSel();
		delete pNewCondition; pNewCondition = 0;
		EndDialog(IDOK);
		return;
	} else {
		if (true == result) {
			m_ModifiedObjective.m_failureConditionList.entries.push_back(pNewCondition);
			m_FailureConditionList.SetCurSel(m_FailureConditionList.GetCount() - 1);
			syncConditionsListWithListBox(&(m_ModifiedObjective.m_failureConditionList), (&m_FailureConditionList));
		} else {
			delete pNewCondition; pNewCondition = 0;
		}
		return;
	}
}

void ObjectiveDlg::OnObjectiveRemoveFailureConditionButton() 
{
	int nSelectionIndex = (&m_FailureConditionList)->GetCurSel();
	if ((0 <= nSelectionIndex) && (m_ModifiedObjective.m_failureConditionList.entries.size() > nSelectionIndex)) {
		delete m_ModifiedObjective.m_failureConditionList.entries[nSelectionIndex];
		m_ModifiedObjective.m_failureConditionList.entries.erase(m_ModifiedObjective.m_failureConditionList.entries.begin() + nSelectionIndex);
		syncConditionsListWithListBox(&(m_ModifiedObjective.m_failureConditionList), (&m_FailureConditionList));
		if (0 < m_FailureConditionList.GetCount()) {
			if (m_FailureConditionList.GetCount() <= (long)nSelectionIndex) {
				nSelectionIndex = m_FailureConditionList.GetCount() - 1;
			}
			m_FailureConditionList.SetCurSel(nSelectionIndex);
		}
	}
}

void ObjectiveDlg::OnObjectiveAddFailureActionButton() 
{
	{
		DWORD actionComboboxSelection = (&m_FailureActionComboBox)->GetCurSel();
		if ((0 <= actionComboboxSelection) && (failureActionComboBoxItems.size() > actionComboboxSelection)) {
			action_species_type species = failureActionComboBoxItems[actionComboboxSelection];
			pNewAction = CObjective::new_CObjectiveAction(species, m_ModifiedObjective.Alignment());
		} else {
			return;
		}
	}
	assert(pNewAction);

	// Editing a condition or action may require a map selection.
	bool result = pNewAction->EditDialog();

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		SaveDialogValues();
		EditorInterface::instance()->objectivesEditState.ModifiedObjective = m_ModifiedObjective;
		EditorInterface::instance()->objectivesEditState.nConditionSpeciesSelectionIndex = (&m_ComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nActionSpeciesSelectionIndex = (&m_FailureActionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureConditionSpeciesSelectionIndex = (&m_FailureConditionComboBox)->GetCurSel();
		EditorInterface::instance()->objectivesEditState.nFailureActionSpeciesSelectionIndex = (&m_FailureActionComboBox)->GetCurSel();
		delete pNewAction; pNewAction = 0;
		EndDialog(IDOK);
		return;
	} else {
		if (true == result) {
			m_ModifiedObjective.m_failureActionList.entries.push_back(pNewAction);
			m_FailureActionList.SetCurSel(m_FailureActionList.GetCount() - 1);
			syncActionsListWithListBox(&(m_ModifiedObjective.m_failureActionList), (&m_FailureActionList));
		} else {
			delete pNewAction; pNewAction = 0;
		}
		return;
	}
}

void ObjectiveDlg::OnObjectiveRemoveFailureActionButton() 
{
	int nSelectionIndex = (&m_FailureActionList)->GetCurSel();
	if ((0 <= nSelectionIndex) && (m_ModifiedObjective.m_failureActionList.entries.size() > nSelectionIndex)) {
		delete m_ModifiedObjective.m_failureActionList.entries[nSelectionIndex];
		m_ModifiedObjective.m_failureActionList.entries.erase(m_ModifiedObjective.m_failureActionList.entries.begin() + nSelectionIndex);
		syncActionsListWithListBox(&(m_ModifiedObjective.m_failureActionList), (&m_FailureActionList));
		if (0 < m_FailureActionList.GetCount()) {
			if (m_FailureActionList.GetCount() <= (long)nSelectionIndex) {
				nSelectionIndex = m_FailureActionList.GetCount() - 1;
			}
			m_FailureActionList.SetCurSel(nSelectionIndex);
		}
	}
}

void ObjectiveDlg::OnObjectiveViewConditionButton() 
{
	int nSelectionIndex = m_List.GetCurSel();
	if ((LB_ERR != nSelectionIndex) && (0 <= nSelectionIndex) && (m_ModifiedObjective.m_conditions.entries.size() > nSelectionIndex)) {
		auto it = m_ModifiedObjective.m_conditions.entries.begin();
		for (int i = 0; i < nSelectionIndex && it != m_ModifiedObjective.m_conditions.entries.end(); ++i) it++;
		if (it != m_ModifiedObjective.m_conditions.entries.end()) {
			std::string tmpEStr = objReadableCondition(*it);
			ViewConditionOrActionDlg viewDlg;
			viewDlg.m_Edit = tmpEStr.c_str();
			viewDlg.DoModal();
		}
	}
}

void ObjectiveDlg::OnObjectiveViewActionButton() 
{
	int nSelectionIndex = m_ActionList.GetCurSel();
	if ((LB_ERR != nSelectionIndex) && (0 <= nSelectionIndex) && (m_ModifiedObjective.m_actionList.entries.size() > nSelectionIndex)) {
		auto it = m_ModifiedObjective.m_actionList.entries.begin();
		for (int i = 0; i < nSelectionIndex && it != m_ModifiedObjective.m_actionList.entries.end(); ++i) it++;
		if (it != m_ModifiedObjective.m_actionList.entries.end()) {
			std::string tmpEStr = objReadableAction(*it);
			ViewConditionOrActionDlg viewDlg;
			viewDlg.m_Edit = tmpEStr.c_str();
			viewDlg.DoModal();
		}
	}
}

void ObjectiveDlg::OnObjectiveViewFailureConditionButton() 
{
	int nSelectionIndex = m_FailureConditionList.GetCurSel();
	if ((LB_ERR != nSelectionIndex) && (0 <= nSelectionIndex) && (m_ModifiedObjective.m_failureConditionList.entries.size() > nSelectionIndex)) {
		auto it = m_ModifiedObjective.m_failureConditionList.entries.begin();
		for (int i = 0; i < nSelectionIndex && it != m_ModifiedObjective.m_failureConditionList.entries.end(); ++i) it++;
		if (it != m_ModifiedObjective.m_failureConditionList.entries.end()) {
			std::string tmpEStr = objReadableCondition(*it);
			ViewConditionOrActionDlg viewDlg;
			viewDlg.m_Edit = tmpEStr.c_str();
			viewDlg.DoModal();
		}
	}
}

void ObjectiveDlg::OnObjectiveViewFailureActionButton() 
{
	int nSelectionIndex = m_FailureActionList.GetCurSel();
	if ((LB_ERR != nSelectionIndex) && (0 <= nSelectionIndex) && (m_ModifiedObjective.m_failureActionList.entries.size() > nSelectionIndex)) {
		auto it = m_ModifiedObjective.m_failureActionList.entries.begin();
		for (int i = 0; i < nSelectionIndex && it != m_ModifiedObjective.m_failureActionList.entries.end(); ++i) it++;
		if (it != m_ModifiedObjective.m_failureActionList.entries.end()) {
			std::string tmpEStr = objReadableAction(*it);
			ViewConditionOrActionDlg viewDlg;
			viewDlg.m_Edit = tmpEStr.c_str();
			viewDlg.DoModal();
		}
	}
}

void ObjectiveDlg::OnObjectiveClearLogisticsModelButton() 
{
	m_Mech.ResetContent();
	m_modelGroup.SetCurSel(-1);
}
