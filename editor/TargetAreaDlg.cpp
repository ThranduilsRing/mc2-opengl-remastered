

/*************************************************************************************************\
TargetAreaDlg.cpp			: Implementation of the TargetAreaDlg component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "stdafx.h"
#include "resource.h"

#include <stdlib.h>
#include <assert.h>
#include <format>
#include <string>

#include "TargetAreaDlg.h"
#include "Objective.h"

#include "EditorInterface.h"


TargetAreaDlg::TargetAreaDlg( float &targetCenterX, float &targetCenterY, float &targetRadius ):CDialog(IDD_TARGET_AREA)
{
	m_pTargetCenterX = &targetCenterX;
	m_pTargetCenterY = &targetCenterY;
	m_pTargetRadius = &targetRadius;
	m_pTargetCenterXEditBox = 0;
	m_pTargetCenterYEditBox = 0;
	m_pTargetRadiusEditBox = 0;
	m_pCancelButton = 0;
	m_pOKButton = 0;
	m_pPickCenterButton = 0;
}

BOOL TargetAreaDlg::OnInitDialog()
{
	m_pTargetCenterXEditBox = (CEdit *)GetDlgItem(IDC_TARGET_AREA_CENTER_X_EDIT);
	assert( m_pTargetCenterXEditBox );

	m_pTargetCenterYEditBox = (CEdit *)GetDlgItem(IDC_TARGET_AREA_CENTER_Y_EDIT);
	assert( m_pTargetCenterYEditBox );

	m_pTargetRadiusEditBox = (CEdit *)GetDlgItem(IDC_TARGET_AREA_RADIUS_EDIT);
	assert( m_pTargetRadiusEditBox );

	m_pCancelButton = (CButton *)GetDlgItem(IDCANCEL);
	assert( m_pCancelButton );

	m_pOKButton = (CButton *)GetDlgItem(IDOK);
	assert( m_pOKButton );

	m_pPickCenterButton = (CButton *)GetDlgItem(IDC_TARGET_AREA_PICK_CENTER_BUTTON);
	assert( m_pPickCenterButton );

	// Slice 2 -- area "Pick center" map-picker result delivery. If we just returned
	// from a terrain point pick tagged PICK_AREA, write the captured world XY into
	// the referenced center (radius untouched) BEFORE populating the edit boxes, so
	// the boxes show the picked values and OnOK persists them. Mirrors the marker
	// picker's OnInitDialog consumer. Discriminator gate ensures a marker pick is
	// NOT consumed here (and vice-versa in ObjectiveDlg::OnInitDialog).
	{
		EditorInterface *pEditor = EditorInterface::instance();
		if ((0 != pEditor) && pEditor->objectivesEditState.pendingPickResultReady &&
		    (CObjectivesEditState::PICK_AREA == pEditor->objectivesEditState.pendingPickTarget)) {
			(*m_pTargetCenterX) = pEditor->objectivesEditState.pendingPickX;
			(*m_pTargetCenterY) = pEditor->objectivesEditState.pendingPickY;

			// One-shot: clear the result + discriminator and leave object-select-only
			// mode so the normal modal accept/cancel flow resumes from here.
			pEditor->objectivesEditState.pendingPickResultReady = false;
			pEditor->objectivesEditState.pendingPickPoint = false;
			pEditor->objectivesEditState.pendingPickTarget = CObjectivesEditState::PICK_NONE;
			pEditor->ObjectSelectOnlyMode(false);
		}
		// Area-pick was armed but no point was delivered (user cancelled/ESC before
		// clicking the map). Disarm cleanly so the editor leaves select-only mode.
		else if ((0 != pEditor) && pEditor->objectivesEditState.pendingPickPoint &&
		         !pEditor->objectivesEditState.pendingPickResultReady &&
		         (CObjectivesEditState::PICK_AREA == pEditor->objectivesEditState.pendingPickTarget)) {
			pEditor->objectivesEditState.pendingPickPoint = false;
			pEditor->objectivesEditState.pendingPickTarget = CObjectivesEditState::PICK_NONE;
			pEditor->ObjectSelectOnlyMode(false);
		}
	}

	std::string tmpStr;

	tmpStr = std::format("{:.3f}", (*m_pTargetCenterX));
	m_pTargetCenterXEditBox->SetWindowText(tmpStr.c_str());

	tmpStr = std::format("{:.3f}", (*m_pTargetCenterY));
	m_pTargetCenterYEditBox->SetWindowText(tmpStr.c_str());

	tmpStr = std::format("{:.3f}", (*m_pTargetRadius));
	m_pTargetRadiusEditBox->SetWindowText(tmpStr.c_str());

	return 1;
}

BOOL TargetAreaDlg::OnCommand(WPARAM wParam, LPARAM lParam) // called by child controls to inform of an event
{
	assert( m_pCancelButton );
	assert( m_pOKButton );

	// Slice 2 -- route the "Pick center" button (no DECLARE_MESSAGE_MAP on this
	// dialog, so dispatch here, mirroring the OK/Cancel handling MFC does for us).
	if ((BN_CLICKED == HIWORD(wParam)) &&
	    (IDC_TARGET_AREA_PICK_CENTER_BUTTON == LOWORD(wParam))) {
		OnPickCenter();
		return TRUE;
	}

	return inherited::OnCommand(wParam, lParam);
}

void TargetAreaDlg::OnCancel()
{
	EndDialog(IDCANCEL);
}

void TargetAreaDlg::OnOK()
{
	CString tmpCStr;
	int result;
	float tmpFloat;

	m_pTargetCenterXEditBox->GetWindowText(tmpCStr);
	result = sscanf(tmpCStr.GetBuffer(0), "%f", &tmpFloat);
	if (1 == result) {
		(*m_pTargetCenterX) = tmpFloat;
	}

	m_pTargetCenterYEditBox->GetWindowText(tmpCStr);
	result = sscanf(tmpCStr.GetBuffer(0), "%f", &tmpFloat);
	if (1 == result) {
		(*m_pTargetCenterY) = tmpFloat;
	}

	m_pTargetRadiusEditBox->GetWindowText(tmpCStr);
	result = sscanf(tmpCStr.GetBuffer(0), "%f", &tmpFloat);
	if (1 == result) {
		(*m_pTargetRadius) = tmpFloat;
	}

	EndDialog(IDOK);
}

// Slice 2 -- area "Pick center" map-picker (PICKER-SELECT-POINT-RECON-1, Option B).
// Mirrors the marker picker (ObjectiveDlg::OnObjectivePickMarkerButton): save the
// in-progress dialog values back to the referenced center/radius, arm a one-shot
// terrain point pick tagged PICK_AREA, flip the editor into selection mode, then
// EndDialog(IDOK) so the whole nested-modal stack unwinds and the editor render
// loop runs. The next terrain left-click is consumed by
// EditorInterface::handleLeftButtonDown, which writes the picked world XY into
// objectivesEditState.pendingPickX/Y, sets pendingPickResultReady, and re-opens the
// objectives dialog chain (Team -> CObjectives::EditDialog -> ObjectivesDlg re-posts
// EDIT -> CObjective::EditDialog -> ObjectiveDlg re-posts ADD condition ->
// OnObjectiveAddConditionButton -> CAreaObjectiveCondition::EditDialog -> this
// dialog). OnInitDialog then loads the picked XY into the center exactly once.
void TargetAreaDlg::OnPickCenter()
{
	EditorInterface *pEditor = EditorInterface::instance();
	if (0 == pEditor) {
		return;
	}

	// Save the current control values back to the referenced center/radius so
	// nothing the user typed is lost across the unwind (same write-through as OnOK).
	{
		CString tmpCStr;
		int result;
		float tmpFloat;

		m_pTargetCenterXEditBox->GetWindowText(tmpCStr);
		result = sscanf(tmpCStr.GetBuffer(0), "%f", &tmpFloat);
		if (1 == result) {
			(*m_pTargetCenterX) = tmpFloat;
		}

		m_pTargetCenterYEditBox->GetWindowText(tmpCStr);
		result = sscanf(tmpCStr.GetBuffer(0), "%f", &tmpFloat);
		if (1 == result) {
			(*m_pTargetCenterY) = tmpFloat;
		}

		m_pTargetRadiusEditBox->GetWindowText(tmpCStr);
		result = sscanf(tmpCStr.GetBuffer(0), "%f", &tmpFloat);
		if (1 == result) {
			(*m_pTargetRadius) = tmpFloat;
		}
	}

	// Arm the one-shot terrain point pick, tagged PICK_AREA so the marker consumer
	// in ObjectiveDlg::OnInitDialog does NOT swallow it; this dialog's OnInitDialog
	// consumes it on re-entry. Reuses the shared pending-pick fields (only one pick
	// in flight at a time).
	pEditor->objectivesEditState.pendingPickPoint = true;
	pEditor->objectivesEditState.pendingPickResultReady = false;
	pEditor->objectivesEditState.pendingPickTarget = CObjectivesEditState::PICK_AREA;

	// Put the editor into selection mode and the objective-select-only handoff mode,
	// exactly like the unit/building pickers, so the re-open machinery fires.
	pEditor->SelectionMode();
	pEditor->ObjectSelectOnlyMode(true);

	EndDialog(IDOK);
}



TargetAreaDlg::~TargetAreaDlg()
{
}


