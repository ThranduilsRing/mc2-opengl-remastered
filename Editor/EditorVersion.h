/***************************************************************
* FILENAME: EditorVersion.h
* DESCRIPTION: Declares Editor-owned SemVer constants and display helpers.
* AUTHOR: Methuselas
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/29/2026
* MODIFICATION: by Methuselas
* CHANGES: Bumped Editor SemVer to 0.14.0 for the EditorCamera terrain shader hookup.
****************************************************************/

#ifndef EDITOR_VERSION_H
#define EDITOR_VERSION_H

#define EDITOR_VERSION_MAJOR 0
#define EDITOR_VERSION_MINOR 14
#define EDITOR_VERSION_PATCH 0

#define EDITOR_VERSION_PRERELEASE "editor"
#define EDITOR_VERSION_BUILD ""

#define EDITOR_VERSION_SEMVER "0.14.0-editor"
#define EDITOR_VERSION_DISPLAY "MC2R Mission Editor 0.14.0"
#define EDITOR_VERSION_STARTUP_LINE "EditorVersion: 0.14.0"

const char* EditorVersion_GetSemVer();
const char* EditorVersion_GetDisplayName();
const char* EditorVersion_GetDisplayString();
const char* EditorVersion_GetApplicationName();
const char* EditorVersion_GetStartupLine();

#endif // EDITOR_VERSION_H
