//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MAINMENU_H
#define MAINMENU_H

// MainMenu was previously built on the wlib Window/Menu framework which is
// no longer part of this codebase. The Editor uses MFC (EditorInterface)
// for all UI. This stub preserves the class declaration and extern so that
// EditorInterface.h continues to compile without modification.

class EditorInterface;

class MainMenu
{
public:
	MainMenu(EditorInterface* pEditorInterface) : m_pEditorInterface(pEditorInterface) {}

	void DoModal() {}
	void OnCommand(void* wnd, int nCommand) {}

protected:
	EditorInterface* m_pEditorInterface;
};

extern MainMenu* pMainMenu;

#endif
