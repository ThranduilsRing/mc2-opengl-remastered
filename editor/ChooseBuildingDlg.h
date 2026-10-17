#ifndef CHOOSEBUILDINGDLG_H
#define CHOOSEBUILDINGDLG_H
/*************************************************************************************************\
ChooseBuildingDlg.h		: Interface for the ChooseBuildingDlg component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "stdafx.h"
#include "resource.h"
#include <vector>
#include "Objective.h"
#include "EditorObjectMgr.h"


class ChooseBuildingDlg: public CDialog
{
public:
	using building_ptr_type = EditorObject *;

	ChooseBuildingDlg( building_ptr_type &buildingPtr );

	BOOL OnCommand(WPARAM wParam, LPARAM lParam); // called by child controls to inform of an event
	void OnUsePointer();
	void OnCancel();
	void OnOK();

	virtual ~ChooseBuildingDlg();

	BOOL OnInitDialog();

private:
	using inherited = CDialog;

	// suppressing these
	inline ChooseBuildingDlg();
	ChooseBuildingDlg& operator=( const ChooseBuildingDlg& lgBuildingPtr );

	building_ptr_type *m_pModifiedBuildingPtr;
	building_ptr_type *m_pBuildingPtr;
	CComboBox *m_pComboBox;
	std::vector<EditorObject*> m_buildingList;
	CButton *m_pUsingPointerButton;
	CButton *m_pCancelButton;
	CButton *m_pOKButton;
};


//*************************************************************************************************
#endif  // end of file ( ChooseBuildingDlg.h )
