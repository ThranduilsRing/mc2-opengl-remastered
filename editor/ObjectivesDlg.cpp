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
	//{{AFX_DATA_INIT(ObjectivesDlg)
	m_TeamEdit = 0;
	//}}AFX_DATA_INIT

	nSelectionIndex = -1;
}


void ObjectivesDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(ObjectivesDlg)
	DDX_Control(pDX, IDC_OBJECTIVES_EDIT_BUTTON, m_EditButton);
	DDX_Control(pDX, IDC_OBJECTIVES_ADD_BUTTON, m_AddButton);
	DDX_Control(pDX, IDC_OBJECTIVES_LIST, m_List);
	DDX_Text(pDX, IDC_OBJECTIVES_TEAM_EDIT, m_TeamEdit);
	//}}AFX_DATA_MAP
}


BEGIN_MESSAGE_MAP(ObjectivesDlg, CDialog)
	//{{AFX_MSG_MAP(ObjectivesDlg)
	ON_BN_CLICKED(IDC_OBJECTIVES_ADD_BUTTON, &ObjectivesDlg::OnObjectivesAddButton)
	ON_BN_CLICKED(IDC_OBJECTIVES_ADD_FROM_TEMPLATE_BUTTON, &ObjectivesDlg::OnObjectivesAddFromTemplateButton)
	ON_BN_CLICKED(IDC_OBJECTIVES_REMOVE_BUTTON, &ObjectivesDlg::OnObjectivesRemoveButton)
	ON_BN_CLICKED(IDC_OBJECTIVES_EDIT_BUTTON, &ObjectivesDlg::OnObjectivesEditButton)
	ON_BN_CLICKED(IDC_OBJECTIVES_COPY_BUTTON, &ObjectivesDlg::OnObjectivesCopyButton)
	ON_BN_CLICKED(IDC_OBJECTIVES_MOVE_UP_BUTTON, &ObjectivesDlg::OnObjectivesMoveUpButton)
	ON_BN_CLICKED(IDC_OBJECTIVES_MOVE_DOWN_BUTTON, &ObjectivesDlg::OnObjectivesMoveDownButton)
	//}}AFX_MSG_MAP
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
	auto it = pObjectives->entries.begin();
	while (it != pObjectives->entries.end())
	{
		std::string tmpEStr;
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
		std::string warnEStr;
		bool firstWarn = true;
		if ((*it)->m_conditions.entries.size() == 0) {
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
			if ((*it)->ActivateFlagID().empty()) {
				if (!firstWarn) { warnEStr += _TEXT("; "); }
				warnEStr += _TEXT("unnamed activate flag");
				firstWarn = false;
			}
		}
		if ((*it)->ResetStatusOnFlag()) {
			if ((*it)->ResetStatusFlagID().empty()) {
				if (!firstWarn) { warnEStr += _TEXT("; "); }
				warnEStr += _TEXT("unnamed reset flag");
				firstWarn = false;
			}
		}
		if (!firstWarn) {
			tmpEStr += _TEXT("   <!> ");
			tmpEStr += warnEStr;
		}
		pList->AddString(tmpEStr.c_str());
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

	// Editing may require a map selection outside the modal dialog.
	bool result = pNewObjective->EditDialog();

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
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
			m_ModifiedObjectives.entries.push_back(pNewObjective);
			m_List.SetCurSel(m_List.GetCount() - 1);
			syncObjectivesListWithListBox(&m_ModifiedObjectives, &m_List);
			nSelectionIndex = m_List.GetCurSel();
		} else {
			delete pNewObjective; pNewObjective = 0;
		}
		return;
	}
}

// Templates provide initial objective content before the ordinary edit dialog opens.
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
			pNewObjective->m_conditions.entries.push_back(pCond);
		}
		CObjectiveAction *pAct =
			CObjective::new_CObjectiveAction(SET_BOOLEAN_FLAG, alignment);
		if (0 != pAct) {
			pNewObjective->m_actionList.entries.push_back(pAct);
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
			pNewObjective->m_conditions.entries.push_back(pCond);
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
			pNewObjective->m_conditions.entries.push_back(pCond);
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
			m_ModifiedObjectives.entries.push_back(pNewObjective);
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
	if ((0 <= nSelectionIndex) && (m_ModifiedObjectives.entries.size() > nSelectionIndex)) {
		delete m_ModifiedObjectives.entries[nSelectionIndex];
		m_ModifiedObjectives.entries.erase(m_ModifiedObjectives.entries.begin() + nSelectionIndex);
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
		if ((0 <= nSelectionIndex) && (((int)(m_ModifiedObjectives.entries.size())) > nSelectionIndex)) {
			pSelectedObjective = m_ModifiedObjectives.entries[nSelectionIndex];
		} else {
			return;
		}
	} else {
		assert(CObjectivesEditState::EDIT == EditorInterface::instance()->objectivesEditState.objectiveFunction);
		pSelectedObjective = m_ModifiedObjectives.entries[nSelectionIndex];
	}
	assert(0 != pSelectedObjective);
	assert(0 <= nSelectionIndex);

	// Editing may require a map selection outside the modal dialog.
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
	if ((0 <= nSelectionIndex) && (m_ModifiedObjectives.entries.size() > nSelectionIndex)) {
		CObjective *pSelectedObjective = 0;
		pSelectedObjective = m_ModifiedObjectives.entries[nSelectionIndex];
		assert(0 != pSelectedObjective);

		CObjective *pNewObjective = new CObjective(pSelectedObjective->Alignment());
		pNewObjective->Init();
		*pNewObjective = *pSelectedObjective;
		m_ModifiedObjectives.entries.push_back(pNewObjective);
		syncObjectivesListWithListBox(&m_ModifiedObjectives, &m_List);
		m_List.SetCurSel(m_List.GetCount() - 1);
		nSelectionIndex = m_List.GetCurSel();
	}
}

void ObjectivesDlg::OnObjectivesMoveUpButton() 
{
	nSelectionIndex = m_List.GetCurSel();
	if ((1 <= nSelectionIndex) && (m_ModifiedObjectives.entries.size() > nSelectionIndex)) {
		CObjective *pSelectedObjective = 0;
		pSelectedObjective = m_ModifiedObjectives.entries[nSelectionIndex];
		assert(0 != pSelectedObjective);

		m_ModifiedObjectives.entries.erase(m_ModifiedObjectives.entries.begin() + nSelectionIndex);
		m_ModifiedObjectives.entries.insert(m_ModifiedObjectives.entries.begin() + nSelectionIndex - 1, pSelectedObjective);
		syncObjectivesListWithListBox(&m_ModifiedObjectives, &m_List);
		m_List.SetCurSel(nSelectionIndex - 1);
		nSelectionIndex = m_List.GetCurSel();
	}
}

void ObjectivesDlg::OnObjectivesMoveDownButton() 
{
	nSelectionIndex = m_List.GetCurSel();
	if ((0 <= nSelectionIndex) && (m_ModifiedObjectives.entries.size() - 1 > nSelectionIndex)) {
		CObjective *pSelectedObjective = 0;
		pSelectedObjective = m_ModifiedObjectives.entries[nSelectionIndex];
		assert(0 != pSelectedObjective);

		m_ModifiedObjectives.entries.erase(m_ModifiedObjectives.entries.begin() + nSelectionIndex);
		if (m_ModifiedObjectives.entries.size() -1 == nSelectionIndex) {
			m_ModifiedObjectives.entries.push_back(pSelectedObjective);
		} else {
			m_ModifiedObjectives.entries.insert(m_ModifiedObjectives.entries.begin() + nSelectionIndex + 1, pSelectedObjective);
		}
		syncObjectivesListWithListBox(&m_ModifiedObjectives, &m_List);
		m_List.SetCurSel(nSelectionIndex + 1);
		nSelectionIndex = m_List.GetCurSel();
	}
}
