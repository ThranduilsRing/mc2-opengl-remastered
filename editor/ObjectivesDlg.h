//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#if !defined(AFX_OBJECTIVESDLG_H__2D45CD01_42FA_4FF0_AB18_BBBAA9B0E3A5__INCLUDED_)
#define AFX_OBJECTIVESDLG_H__2D45CD01_42FA_4FF0_AB18_BBBAA9B0E3A5__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000
// ObjectivesDlg.h : header file
//

#include "Objective.h"

/////////////////////////////////////////////////////////////////////////////
// CObjectiveTemplateDlg dialog -- tiny modal preset picker.
// Presets are PICK-FREE (no map pick). The selected preset index is read back
// via SelectedPreset() after DoModal() returns IDOK.

class CObjectiveTemplateDlg : public CDialog
{
public:
	// Preset enum. Keep in lockstep with the combo-box population order in
	// OnInitDialog and with the apply switch in ObjectivesDlg::OnObjectivesAddFromTemplateButton.
	enum EPreset {
		PRESET_HIDDEN_TRIGGER = 0,
		PRESET_DESTROY_N_ENEMY,
		PRESET_MOVE_TO_AREA,
		PRESET_COUNT
	};

	CObjectiveTemplateDlg(CWnd* pParent = nullptr);

	enum { IDD = IDD_OBJECTIVE_TEMPLATE };

	int SelectedPreset() const { return m_selectedPreset; }

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();

	CComboBox m_PresetCombo;
	int m_selectedPreset;

	DECLARE_MESSAGE_MAP()
};

/////////////////////////////////////////////////////////////////////////////
// ObjectivesDlg dialog

class ObjectivesDlg : public CDialog
{
// Construction
public:
	ObjectivesDlg(CWnd* pParent = nullptr);   // standard constructor

	CObjectives m_ModifiedObjectives;

	int nSelectionIndex;

// Dialog Data
	//{{AFX_DATA(ObjectivesDlg)
	enum { IDD = IDD_OBJECTIVES };
	CButton	m_EditButton;
	CButton	m_AddButton;
	CListBox	m_List;
	int		m_TeamEdit;
	//}}AFX_DATA


// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(ObjectivesDlg)
	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	//}}AFX_VIRTUAL

// Implementation
protected:

	// Generated message map functions
	//{{AFX_MSG(ObjectivesDlg)
	afx_msg void OnObjectivesAddButton();
	afx_msg void OnObjectivesAddFromTemplateButton();
	afx_msg void OnObjectivesRemoveButton();
	afx_msg void OnObjectivesEditButton();
	afx_msg void OnObjectivesCopyButton();
	afx_msg void OnObjectivesMoveUpButton();
	afx_msg void OnObjectivesMoveDownButton();
	virtual BOOL OnInitDialog();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_OBJECTIVESDLG_H__2D45CD01_42FA_4FF0_AB18_BBBAA9B0E3A5__INCLUDED_)
