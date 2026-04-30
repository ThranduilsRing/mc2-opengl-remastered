/***************************************************************
* FILENAME: EditorMFC.cpp
* DESCRIPTION: Implements the MFC application entry points for the Editor.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Logs Editor-owned SemVer at MFC startup.
****************************************************************/

#include <cstdio>

static void EarlyTrace(const char* msg)
{
    FILE* f = fopen("editor-startup.log", "a");
    if (f)
    {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

static void EarlyTraceBegin()
{
    // Open with "w" exactly once at true process start to clear any stale log,
    // then write the sentinel.  All subsequent writes use "a" (append).
    FILE* f = fopen("editor-startup.log", "w");
    if (f)
    {
        fprintf(f, "editor-startup.log begin\n");
        fclose(f);
    }
}

// EditorMFC.cpp : Defines the class behaviors for the application.
//

#include "stdafx.h"
#include "EditorMFC.h"

#include "MainFrm.h"
#include "editorinterface.h"
#include "EditorVersion.h"

// Forward declaration — defined in EditorGameOS.cpp.
// MFCPlatform.hpp is a legacy header that may not be present in all build
// configurations; declare the symbol directly to avoid the dependency.
void __stdcall InitGameOS(HINSTANCE hInstance, HWND hWindow, char* commandLine);

static EditorInterface* g_editorInterfaceWindow = NULL;

/////////////////////////////////////////////////////////////////////////////
// EditorMFCApp

BEGIN_MESSAGE_MAP(EditorMFCApp, CWinApp)
	//{{AFX_MSG_MAP(EditorMFCApp)
	ON_COMMAND(ID_APP_ABOUT, OnAppAbout)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// EditorMFCApp construction

EditorMFCApp::EditorMFCApp()
	: m_bPendingInitGameOS(false)
	, m_hEditorWnd(NULL)
{
}

/////////////////////////////////////////////////////////////////////////////
// The one and only EditorMFCApp object

EditorMFCApp theApp;

/////////////////////////////////////////////////////////////////////////////
// EditorMFCApp initialization

BOOL EditorMFCApp::InitInstance()
{
	EarlyTraceBegin();
	EarlyTrace("InitInstance: enter");
	EarlyTrace(EditorVersion_GetStartupLine());

#ifdef _AFXDLL
	// Enable3dControls() is deprecated and no-op on modern MSVC -- removed per C4996
#else
	Enable3dControlsStatic();	// Call this when linking to MFC statically
#endif

	SetRegistryKey(_T("Local AppWizard-Generated Applications"));

	MainFrame* pFrame = new MainFrame;
	m_pMainWnd = pFrame;
	EarlyTrace("InitInstance: m_pMainWnd set");

	// Create the real MFC frame first.  The EditorInterface view is normally
	// constructed from MainFrame::OnCreate; if that route fails, create one here.
	if (!pFrame->LoadFrame(IDR_EDITOR_MENU, WS_OVERLAPPEDWINDOW | FWS_ADDTOTITLE, NULL, NULL))
	{
		EarlyTrace("InitInstance: LoadFrame FAILED");
		delete pFrame;
		m_pMainWnd = NULL;
		return FALSE;
	}
	EarlyTrace("InitInstance: LoadFrame OK");

	HICON editorIco = LoadIcon(IDI_ICON1);
	if (editorIco)
	{
		pFrame->SetIcon(editorIco, TRUE);
		pFrame->SetIcon(editorIco, FALSE);
	}

	// Make sure the frame has a native MFC menu.  Some remastered startup paths
	// leave the frame alive but without a menu attached, which hides the editor UI.
	if (pFrame->GetMenu() == NULL)
	{
		CMenu* pMenu = new CMenu;
		if (pMenu->LoadMenu(IDR_EDITOR_MENU))
		{
			pFrame->SetMenu(pMenu);
			pMenu->Detach(); // ownership transferred to the frame HWND
			EarlyTrace("InitInstance: menu loaded and attached");
		}
		else
		{
			EarlyTrace("InitInstance: menu LoadMenu FAILED");
		}
		delete pMenu;
	}
	else
	{
		EarlyTrace("InitInstance: frame already has menu");
	}

	EditorInterface* editorInterface = EditorInterface::instance();
	if (!editorInterface)
	{
		EarlyTrace("InitInstance: EditorInterface missing after LoadFrame; creating fallback child");

		CRect clientRect;
		pFrame->GetClientRect(&clientRect);

		g_editorInterfaceWindow = new EditorInterface;
		if (!g_editorInterfaceWindow->Create(
			NULL,
			_T("EditorInterface"),
			WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
			clientRect,
			pFrame,
			AFX_IDW_PANE_FIRST,
			NULL))
		{
			EarlyTrace("InitInstance: fallback EditorInterface Create FAILED");
			delete g_editorInterfaceWindow;
			g_editorInterfaceWindow = NULL;
		}
		else
		{
			EarlyTrace("InitInstance: fallback EditorInterface Create OK");
			g_editorInterfaceWindow->ShowWindow(SW_SHOW);
			g_editorInterfaceWindow->UpdateWindow();
		}
	}
	else
	{
		EarlyTrace("InitInstance: EditorInterface exists after LoadFrame");
	}

	pFrame->ShowWindow(SW_SHOW);
	pFrame->UpdateWindow();
	pFrame->DrawMenuBar();

	HWND editorWnd = NULL;
	editorInterface = EditorInterface::instance();
	if (editorInterface && editorInterface->GetSafeHwnd())
	{
		editorWnd = editorInterface->GetSafeHwnd();
		EarlyTrace("InitInstance: EditorInterface hwnd valid");
	}
	else
	{
		EarlyTrace("InitInstance: EditorInterface hwnd NULL before InitGameOS");
	}

	// SDL_CreateWindow on Windows requires an active Win32 message pump on the
	// calling thread: WGL calls CreateWindowEx internally, which sends WM_CREATE
	// synchronously, and DWM/driver setup needs the pump to dispatch those messages.
	// Calling SDL_CreateWindow here (before CWinApp::Run() enters the pump) causes
	// a deadlock or driver crash that kills the process a few seconds in.
	//
	// Defer InitGameOS to the first OnIdle tick, at which point Run() has entered
	// the pump and the thread is fully message-capable.
	m_hEditorWnd          = editorWnd;
	m_sCmdLine            = m_lpCmdLine ? m_lpCmdLine : _T("");
	m_bPendingInitGameOS  = true;
	EarlyTrace("InitInstance: InitGameOS deferred to first OnIdle");

	if (editorInterface && editorInterface->GetSafeHwnd())
	{
		editorInterface->SetFocus();
	}
	else
	{
		pFrame->SetFocus();
	}

	EarlyTrace("InitInstance: return TRUE");
	return TRUE;
}

/////////////////////////////////////////////////////////////////////////////
// EditorMFCApp message handlers





/////////////////////////////////////////////////////////////////////////////
// CAboutDlg dialog used for App About

class CAboutDlg : public CDialog
{
public:
	CAboutDlg();

// Dialog Data
	//{{AFX_DATA(CAboutDlg)
	enum { IDD = IDD_FOG };
	//}}AFX_DATA

	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(CAboutDlg)
	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	//}}AFX_VIRTUAL

// Implementation
protected:
	//{{AFX_MSG(CAboutDlg)
		// No message handlers
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialog(CAboutDlg::IDD)
{
	//{{AFX_DATA_INIT(CAboutDlg)
	//}}AFX_DATA_INIT
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(CAboutDlg)
	//}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialog)
	//{{AFX_MSG_MAP(CAboutDlg)
		// No message handlers
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

// App command to run the dialog
void EditorMFCApp::OnAppAbout()
{
	CAboutDlg aboutDlg;
	aboutDlg.DoModal();
}

/////////////////////////////////////////////////////////////////////////////
// EditorMFCApp message handlers


BOOL EditorMFCApp::OnIdle(LONG lCount) 
{
	CWinApp::OnIdle(lCount);

	// First-tick deferred initialisation: the MFC message pump is now live so
	// SDL_CreateWindow (which sends WM_CREATE synchronously via CreateWindowEx
	// and needs the pump to dispatch DWM/WGL messages) is safe to call.
	if (m_bPendingInitGameOS)
	{
		m_bPendingInitGameOS = false;
		EarlyTrace("OnIdle: deferred InitGameOS: enter");
		InitGameOS(m_hInstance, m_hEditorWnd, (char*)(LPCSTR)m_sCmdLine);
		EarlyTrace("OnIdle: deferred InitGameOS: complete");
	}

	EditorInterface* pEditor = EditorInterface::instance();
	if (pEditor && ::IsWindow(pEditor->m_hWnd))
	{
		// The 3D viewport is an embedded child window.  The old foreground-window
		// check only repainted when the MFC frame itself was foreground, which
		// starves the viewport when focus is inside a child/control.
		::InvalidateRect(pEditor->m_hWnd, NULL, FALSE);
	}

	Sleep(2/*milliseconds*/); /* limits the framerate to 500fps */
	return 1;
}


int EditorMFCApp::ExitInstance() 
{
	{
		Environment.TerminateGameEngine();
		gos_PushCurrentHeap(0); // TerminateGameEngine() forgets to do this
	}
	if (false) {
		ExitGameOS();
		if (!EditorInterface::instance()->m_hWnd)
		{
			/* ExitGameOS() shuts down directX which has the side effect of killing the
			EditorInterface window, so we recreate the window here. The editor window
			may not be referenced after this function is executed, but this is not the correct
			place for the EditorInterface window to be destroyed. */
			EditorInterface::instance()->Create(NULL, NULL, AFX_WS_DEFAULT_VIEW | WS_VSCROLL | WS_HSCROLL,
				CRect(0, 0, 0, 0), m_pMainWnd, AFX_IDW_PANE_FIRST, NULL);
		}
	}

	delete m_pMainWnd;
	m_pMainWnd = 0;

	return CWinApp::ExitInstance();
}

