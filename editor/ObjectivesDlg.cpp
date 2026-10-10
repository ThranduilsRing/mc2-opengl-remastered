//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

// ObjectivesDlg.cpp : implementation file
//

#include "stdafx.h"
#include "resource.h"
#include "ObjectivesDlg.h"

#include "assert.h"
#include "EditorInterface.h"

#if 0 /*gos doesn't like this */
#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif
#endif /*gos doesn't like this */




ObjectivesDlg::ObjectivesDlg(CWnd* pParent /*=NULL*/)
	: CDialog(ObjectivesDlg::IDD, pParent)
{
	m_TeamEdit = 0;

	nSelectionIndex = -1;
}


void ObjectivesDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_OBJECTIVES_EDIT_BUTTON, m_EditButton);
	DDX_Control(pDX, IDC_OBJECTIVES_ADD_BUTTON, m_AddButton);
	DDX_Control(pDX, IDC_OBJECTIVES_LIST, m_List);
	DDX_Text(pDX, IDC_OBJECTIVES_TEAM_EDIT, m_TeamEdit);
}


BEGIN_MESSAGE_MAP(ObjectivesDlg, CDialog)
	ON_BN_CLICKED(IDC_OBJECTIVES_ADD_BUTTON, OnObjectivesAddButton)
	ON_BN_CLICKED(IDC_OBJECTIVES_ADD_FROM_TEMPLATE_BUTTON, OnObjectivesAddFromTemplateButton)
	ON_BN_CLICKED(IDC_OBJECTIVES_REMOVE_BUTTON, OnObjectivesRemoveButton)
	ON_BN_CLICKED(IDC_OBJECTIVES_EDIT_BUTTON, OnObjectivesEditButton)
	ON_BN_CLICKED(IDC_OBJECTIVES_COPY_BUTTON, OnObjectivesCopyButton)
	ON_BN_CLICKED(IDC_OBJECTIVES_MOVE_UP_BUTTON, OnObjectivesMoveUpButton)
	ON_BN_CLICKED(IDC_OBJECTIVES_MOVE_DOWN_BUTTON, OnObjectivesMoveDownButton)
END_MESSAGE_MAP()


// Human-readable preset labels. Order MUST match CObjectiveTemplateDlg::EPreset
// and the apply switch in OnObjectivesAddFromTemplateButton.
static const char *kObjectiveTemplatePresetLabels[] = {
	"Hidden trigger (flag-driven)",
	"Destroy N enemy units",
	"Move any unit to area (set coords later)",
};

CObjectiveTemplateDlg::CObjectiveTemplateDlg(CWnd* pParent /*=NULL*/)
	: CDialog(CObjectiveTemplateDlg::IDD, pParent)
{
	m_selectedPreset = -1;
}

void CObjectiveTemplateDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_OBJECTIVE_TEMPLATE_COMBO, m_PresetCombo);
}

BEGIN_MESSAGE_MAP(CObjectiveTemplateDlg, CDialog)
END_MESSAGE_MAP()

BOOL CObjectiveTemplateDlg::OnInitDialog()
{
	CDialog::OnInitDialog();
	m_PresetCombo.ResetContent();
	for (int i = 0; i < CObjectiveTemplateDlg::PRESET_COUNT; ++i) {
		m_PresetCombo.AddString(kObjectiveTemplatePresetLabels[i]);
	}
	m_PresetCombo.SetCurSel(0);
	return TRUE;
}

void CObjectiveTemplateDlg::OnOK()
{
	int sel = m_PresetCombo.GetCurSel();
	if ((CB_ERR == sel) || (sel < 0) || (sel >= CObjectiveTemplateDlg::PRESET_COUNT)) {
		// No valid preset chosen; keep the dialog open rather than committing junk.
		AfxMessageBox(_T("Select a preset first."));
		return;
	}
	m_selectedPreset = sel;
	CDialog::OnOK();
}


static void syncObjectivesListWithListBox(const CObjectives *pObjectives, CListBox *pList) {
	pList->ResetContent();
	CObjectives::EConstIterator it = pObjectives->Begin();
	while (!it.IsDone())
	{
		EString tmpEStr;
		tmpEStr = _TEXT("[");
		if (1 == (*it)->Priority()) {
			tmpEStr += _TEXT("*");
		} else {
		}
		if ((*it)->PreviousPrimaryObjectiveMustBeComplete()) {
			tmpEStr += _TEXT("^");
		} else {
		}
		if ((*it)->AllPreviousPrimaryObjectivesMustBeComplete()) {
			tmpEStr += _TEXT("A");
		} else {
		}
		if ((*it)->IsHiddenTrigger()) {
			tmpEStr += _TEXT("H");
		} else {
		}
		if ((*it)->ActivateOnFlag()) {
			tmpEStr += _TEXT("F");
		} else {
		}
		tmpEStr += _TEXT("] ");
		tmpEStr += ((*it)->LocalizedTitle());
		// At-a-glance validation warnings (data-only; read-only logic).
		EString warnEStr;
		bool firstWarn = true;
		if ((*it)->Count() == 0) {
			if (!firstWarn) { warnEStr += _TEXT("; "); }
			warnEStr += _TEXT("no success condition");
			firstWarn = false;
		}
		if ((*it)->IsHiddenTrigger() && (*it)->DisplayMarker()) {
			if (!firstWarn) { warnEStr += _TEXT("; "); }
			warnEStr += _TEXT("hidden but has marker");
			firstWarn = false;
		}
		if ((*it)->ActivateOnFlag()) {
			const char *pActivateFlag = (*it)->ActivateFlagID().Data();
			if ((0 == pActivateFlag) || (0 == pActivateFlag[0])) {
				if (!firstWarn) { warnEStr += _TEXT("; "); }
				warnEStr += _TEXT("unnamed activate flag");
				firstWarn = false;
			}
		}
		if ((*it)->ResetStatusOnFlag()) {
			const char *pResetFlag = (*it)->ResetStatusFlagID().Data();
			if ((0 == pResetFlag) || (0 == pResetFlag[0])) {
				if (!firstWarn) { warnEStr += _TEXT("; "); }
				warnEStr += _TEXT("unnamed reset flag");
				firstWarn = false;
			}
		}
		if (!firstWarn) {
			tmpEStr += _TEXT("   <!> ");
			tmpEStr += warnEStr;
		}
		pList->AddString(tmpEStr.Data());
		it++;
	}
}

BOOL ObjectivesDlg::OnInitDialog() 
{
	CDialog::OnInitDialog();
	m_TeamEdit = m_ModifiedObjectives.Alignment() + 1;
	syncObjectivesListWithListBox(&m_ModifiedObjectives, &m_List);
	m_List.SetCurSel(nSelectionIndex);
	UpdateData(FALSE);
	
	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		if (CObjectivesEditState::ADD == EditorInterface::instance()->objectivesEditState.objectiveFunction) {
			PostMessage(WM_COMMAND, MAKEWPARAM(IDC_OBJECTIVES_ADD_BUTTON, BN_CLICKED), (LPARAM)((&m_AddButton)->m_hWnd));
		} else if (CObjectivesEditState::EDIT == EditorInterface::instance()->objectivesEditState.objectiveFunction) {
			PostMessage(WM_COMMAND, MAKEWPARAM(IDC_OBJECTIVES_EDIT_BUTTON, BN_CLICKED), (LPARAM)((&m_EditButton)->m_hWnd));
		} else { assert(false); }
	} else {
		EditorInterface::instance()->objectivesEditState.alignment = m_ModifiedObjectives.Alignment();
		EditorInterface::instance()->objectivesEditState.ModifiedObjectives = m_ModifiedObjectives;
		EditorInterface::instance()->objectivesEditState.nSelectionIndex = m_List.GetCurSel();
	}

	return TRUE;  // return TRUE unless you set the focus to a control
	              // EXCEPTION: OCX Property Pages should return FALSE
}

void ObjectivesDlg::OnObjectivesAddButton() 
{
	CObjective *pNewObjective = new CObjective(m_ModifiedObjectives.Alignment());
	assert(pNewObjective);

	/* This call may set the editor into ObjectSelectOnlyMode (i.e.: set the  value of
	EditorInterface::instance()->ObjectSelectOnlyMode() to true) */
	bool result = pNewObjective->EditDialog();

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		/* close the dialog and enter ObjectSelectOnlyMode */
		UpdateData(TRUE);
		EditorInterface::instance()->objectivesEditState.objectiveFunction = CObjectivesEditState::ADD;
		EditorInterface::instance()->objectivesEditState.alignment = m_ModifiedObjectives.Alignment();
		EditorInterface::instance()->objectivesEditState.ModifiedObjectives = m_ModifiedObjectives;
		EditorInterface::instance()->objectivesEditState.nSelectionIndex = m_List.GetCurSel();
		delete pNewObjective; pNewObjective = 0;
		EndDialog(IDOK);
		return;
	} else {
		if (true == result) {
			m_ModifiedObjectives.Append(pNewObjective);
			m_List.SetCurSel(m_List.GetCount() - 1);
			syncObjectivesListWithListBox(&m_ModifiedObjectives, &m_List);
			nSelectionIndex = m_List.GetCurSel();
		} else {
			delete pNewObjective; pNewObjective = 0;
		}
		return;
	}
}

// Slice 1 "Add from Template..." -- pre-populate a CObjective for the chosen
// PICK-FREE preset, then ride the IDENTICAL ADD path: call EditDialog() and reuse
// the same ObjectSelectOnlyMode re-entry / append-on-true tail as
// OnObjectivesAddButton. No new objectiveFunction value, no new edit-state field;
// because the preset is fully applied BEFORE the first EditDialog(), this is a
// normal ADD that happens to start with content. None of the Slice 1 presets call
// a map-pick on construction, so they never enter ObjectSelectOnlyMode at all --
// but if the user triggers a pick from inside CObjective::EditDialog (e.g. the area
// "Pick center"), the existing ADD replay path carries the pre-populated objective
// through unchanged.
void ObjectivesDlg::OnObjectivesAddFromTemplateButton()
{
	// In ObjectSelectOnlyMode the dialog is mid-handoff (re-opened to replay an
	// in-progress ADD/EDIT); do not start a new template flow on top of it.
	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		return;
	}

	CObjectiveTemplateDlg templateDlg(this);
	if (IDOK != templateDlg.DoModal()) {
		return;
	}
	int preset = templateDlg.SelectedPreset();
	if ((preset < 0) || (preset >= CObjectiveTemplateDlg::PRESET_COUNT)) {
		return;
	}

	int alignment = m_ModifiedObjectives.Alignment();
	CObjective *pNewObjective = new CObjective(alignment);
	assert(pNewObjective);
	if (0 == pNewObjective) {
		return;
	}

	switch (preset) {
	case CObjectiveTemplateDlg::PRESET_HIDDEN_TRIGGER: {
		// Hidden, flag-driven trigger objective. Activation flag specifics left
		// for the user to tune; we provide a sensible default success condition
		// (flag0 is set) plus an action that sets a flag so the trigger does
		// something. The CBooleanFlagIsSet / CSetBooleanFlag ctors already default
		// to flag0/true.
		pNewObjective->Title(_TEXT("Hidden trigger"));
		pNewObjective->Description(_TEXT("Flag-driven hidden trigger. Set the activation/condition flags."));
		pNewObjective->IsHiddenTrigger(true);

		CObjectiveCondition *pCond =
			CObjective::new_CObjectiveCondition(BOOLEAN_FLAG_IS_SET, alignment);
		if (0 != pCond) {
			pNewObjective->Append(pCond);
		}
		CObjectiveAction *pAct =
			CObjective::new_CObjectiveAction(SET_BOOLEAN_FLAG, alignment);
		if (0 != pAct) {
			pNewObjective->m_actionList.Append(pAct);
		}
		break;
	}
	case CObjectiveTemplateDlg::PRESET_DESTROY_N_ENEMY: {
		// Destroy N enemy units. Primary priority; default N = 1.
		pNewObjective->Title(_TEXT("Destroy enemy units"));
		pNewObjective->Description(_TEXT("Destroy the required number of enemy units."));
		pNewObjective->Priority(1);

		CObjectiveCondition *pCond =
			CObjective::new_CObjectiveCondition(DESTROY_NUMBER_OF_ENEMY_UNITS, alignment);
		if (0 != pCond) {
			CNumberOfEnemyUnitsObjectiveCondition *pNum =
				dynamic_cast<CNumberOfEnemyUnitsObjectiveCondition *>(pCond);
			if (0 != pNum) {
				pNum->Num(1);
			}
			pNewObjective->Append(pCond);
		}
		break;
	}
	case CObjectiveTemplateDlg::PRESET_MOVE_TO_AREA: {
		// Move any unit to area. Center left at 0,0 (user sets later via the area
		// "Pick center"); default a non-zero radius so the area is not degenerate.
		pNewObjective->Title(_TEXT("Move to area"));
		pNewObjective->Description(_TEXT("Move any unit into the target area. Set the area center via Edit."));

		CObjectiveCondition *pCond =
			CObjective::new_CObjectiveCondition(MOVE_ANY_UNIT_TO_AREA, alignment);
		if (0 != pCond) {
			CAreaObjectiveCondition *pArea =
				dynamic_cast<CAreaObjectiveCondition *>(pCond);
			if (0 != pArea) {
				pArea->TargetCenterX(0.0f);
				pArea->TargetCenterY(0.0f);
				pArea->TargetRadius(256.0f);
			}
			pNewObjective->Append(pCond);
		}
		break;
	}
	default:
		// Unknown preset -- clean up and bail.
		delete pNewObjective; pNewObjective = 0;
		return;
	}

	bool result = pNewObjective->EditDialog();

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		/* close the dialog and enter ObjectSelectOnlyMode */
		UpdateData(TRUE);
		EditorInterface::instance()->objectivesEditState.objectiveFunction = CObjectivesEditState::ADD;
		EditorInterface::instance()->objectivesEditState.alignment = m_ModifiedObjectives.Alignment();
		EditorInterface::instance()->objectivesEditState.ModifiedObjectives = m_ModifiedObjectives;
		EditorInterface::instance()->objectivesEditState.nSelectionIndex = m_List.GetCurSel();
		delete pNewObjective; pNewObjective = 0;
		EndDialog(IDOK);
		return;
	} else {
		if (true == result) {
			m_ModifiedObjectives.Append(pNewObjective);
			m_List.SetCurSel(m_List.GetCount() - 1);
			syncObjectivesListWithListBox(&m_ModifiedObjectives, &m_List);
			nSelectionIndex = m_List.GetCurSel();
		} else {
			delete pNewObjective; pNewObjective = 0;
		}
		return;
	}
}

void ObjectivesDlg::OnObjectivesRemoveButton()
{
	nSelectionIndex = m_List.GetCurSel();
	if ((0 <= nSelectionIndex) && (m_ModifiedObjectives.Count() > nSelectionIndex)) {
		delete *(m_ModifiedObjectives.Iterator(nSelectionIndex));
		m_ModifiedObjectives.Delete(nSelectionIndex);
		syncObjectivesListWithListBox(&m_ModifiedObjectives, &m_List);
		if (0 < m_List.GetCount()) {
			if (m_List.GetCount() <= (long)nSelectionIndex) {
				nSelectionIndex = m_List.GetCount() - 1;
			}
			m_List.SetCurSel(nSelectionIndex);
		}
	}
	nSelectionIndex = m_List.GetCurSel();
}

void ObjectivesDlg::OnObjectivesEditButton() 
{
	CObjective *pSelectedObjective = 0;
	if (!EditorInterface::instance()->ObjectSelectOnlyMode()) {
		nSelectionIndex = m_List.GetCurSel();
		if ((0 <= nSelectionIndex) && (((int)(m_ModifiedObjectives.Count())) > nSelectionIndex)) {
			pSelectedObjective = *(m_ModifiedObjectives.Iterator(nSelectionIndex));
		} else {
			return;
		}
	} else {
		assert(CObjectivesEditState::EDIT == EditorInterface::instance()->objectivesEditState.objectiveFunction);
		pSelectedObjective = *(m_ModifiedObjectives.Iterator(nSelectionIndex));
	}
	assert(0 != pSelectedObjective);
	assert(0 <= nSelectionIndex);

	/* This call may set the editor into ObjectSelectOnlyMode (i.e.: set the  value of
	EditorInterface::instance()->ObjectSelectOnlyMode() to true) */
	pSelectedObjective->EditDialog();

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		UpdateData(TRUE);
		EditorInterface::instance()->objectivesEditState.objectiveFunction = CObjectivesEditState::EDIT;
		EditorInterface::instance()->objectivesEditState.alignment = m_ModifiedObjectives.Alignment();
		EditorInterface::instance()->objectivesEditState.ModifiedObjectives = m_ModifiedObjectives;
		EditorInterface::instance()->objectivesEditState.nSelectionIndex = m_List.GetCurSel();
		EndDialog(IDOK);
		return;
	} else {
		nSelectionIndex = m_List.GetCurSel();
		syncObjectivesListWithListBox(&m_ModifiedObjectives, &m_List);
		if ((0 <= nSelectionIndex) && (m_List.GetCount() > nSelectionIndex)) {
			m_List.SetCurSel(nSelectionIndex);
		}
		nSelectionIndex = m_List.GetCurSel();
		return;
	}
}

void ObjectivesDlg::OnObjectivesCopyButton() 
{
	nSelectionIndex = m_List.GetCurSel();
	if ((0 <= nSelectionIndex) && (m_ModifiedObjectives.Count() > nSelectionIndex)) {
		CObjective *pSelectedObjective = 0;
		pSelectedObjective = *(m_ModifiedObjectives.Iterator(nSelectionIndex));
		assert(0 != pSelectedObjective);

		CObjective *pNewObjective = new CObjective(pSelectedObjective->Alignment());
		pNewObjective->Init();
		*pNewObjective = *pSelectedObjective;
		m_ModifiedObjectives.Append(pNewObjective);
		syncObjectivesListWithListBox(&m_ModifiedObjectives, &m_List);
		m_List.SetCurSel(m_List.GetCount() - 1);
		nSelectionIndex = m_List.GetCurSel();
	}
}

void ObjectivesDlg::OnObjectivesMoveUpButton() 
{
	nSelectionIndex = m_List.GetCurSel();
	if ((1 <= nSelectionIndex) && (m_ModifiedObjectives.Count() > nSelectionIndex)) {
		CObjective *pSelectedObjective = 0;
		pSelectedObjective = *(m_ModifiedObjectives.Iterator(nSelectionIndex));
		assert(0 != pSelectedObjective);

		m_ModifiedObjectives.Delete(nSelectionIndex);
		m_ModifiedObjectives.Insert(pSelectedObjective, nSelectionIndex - 1);
		syncObjectivesListWithListBox(&m_ModifiedObjectives, &m_List);
		m_List.SetCurSel(nSelectionIndex - 1);
		nSelectionIndex = m_List.GetCurSel();
	}
}

void ObjectivesDlg::OnObjectivesMoveDownButton() 
{
	nSelectionIndex = m_List.GetCurSel();
	if ((0 <= nSelectionIndex) && (m_ModifiedObjectives.Count() - 1 > nSelectionIndex)) {
		CObjective *pSelectedObjective = 0;
		pSelectedObjective = *(m_ModifiedObjectives.Iterator(nSelectionIndex));
		assert(0 != pSelectedObjective);

		m_ModifiedObjectives.Delete(nSelectionIndex);
		if (m_ModifiedObjectives.Count() -1 == nSelectionIndex) {
			m_ModifiedObjectives.Append(pSelectedObjective);
		} else {
			m_ModifiedObjectives.Insert(pSelectedObjective, nSelectionIndex + 1);
		}
		syncObjectivesListWithListBox(&m_ModifiedObjectives, &m_List);
		m_List.SetCurSel(nSelectionIndex + 1);
		nSelectionIndex = m_List.GetCurSel();
	}
}
