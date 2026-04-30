/***************************************************************
* FILENAME: EditorVersion.cpp
* DESCRIPTION: Defines Editor-owned SemVer constants and display helpers.
* AUTHOR: Methuselas
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/29/2026
* MODIFICATION: by Methuselas
* CHANGES: Carried Editor SemVer 0.14.0 through the helper accessors.
****************************************************************/

#include "stdafx.h"
#include "EditorVersion.h"

const char* EditorVersion_GetSemVer()
{
    return EDITOR_VERSION_SEMVER;
}

const char* EditorVersion_GetDisplayName()
{
    return "MC2R Editor";
}

const char* EditorVersion_GetDisplayString()
{
    return EDITOR_VERSION_DISPLAY;
}

const char* EditorVersion_GetApplicationName()
{
    return EditorVersion_GetDisplayString();
}

const char* EditorVersion_GetStartupLine()
{
    return EDITOR_VERSION_STARTUP_LINE;
}
