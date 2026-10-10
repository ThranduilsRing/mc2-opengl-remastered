// Shared surface of the Objective translation units (Objective*.cpp).
//
// Objective.cpp keeps the condition/action lists, CObjective and CObjectives; the condition and action classes are split out.
#ifndef OBJECTIVE_INTERNAL_H
#define OBJECTIVE_INTERNAL_H

/*************************************************************************************************\
Objective.cpp			: Implementation of the Objective component.
\*************************************************************************************************/

#include "stdafx.h"
#include "terrain.h"

#include "Objective.h"
#include "ObjectiveDlg.h"
#include "ObjectivesDlg.h"
#include "ChooseUnitDlg.h"
#include "ChooseBuildingDlg.h"
#include "ResourceStringSelectionDlg.h"
#include "SingleValueDlg.h"
#include "BooleanFlagIsSetDialog.h"
#include "TargetAreaDlg.h"
#include "TextMessageDlg.h"
#include "MCLibResource.h"
#include "EditorInterface.h"
#include "../resource.h"/*this is mc2res.dll's header file*/

#include "EString.h"
#include "ECharString.h"

#ifndef INIFILE_H
#include "IniFile.h"
#endif

#include <assert.h>
#include "EditorResourceFallback.h"

// Globals defined in Objective.cpp (or elsewhere in the engine) that the split pieces share.

static bool ESLoadString(int resourceID, EString &targetStr) {
	char szTmp[16384/*max string length*/];
	EditorSafeLoadString( resourceID, szTmp, 16384/*max string length*/ );
	targetStr = szTmp;
	EString tmpStr;
	tmpStr.Format("mc2res.dll:%d Not defined", resourceID);
	if (0 == strcmp(tmpStr.Data(), szTmp)) {
		return (0);
	}
	return (!0);
}

static long sReadIdFloat(FitIniFile* missionFile, const char *varName, float &value) {
	long result = 0;
	float tmpFloat;
	result = missionFile->readIdFloat((char *)varName, tmpFloat);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpFloat;
	}
	return result;
}

static long sReadIdBoolean(FitIniFile* missionFile, const char *varName, bool &value) {
	long result = 0;
	bool tmpBool;
	result = missionFile->readIdBoolean((char *)varName, tmpBool);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpBool;
	}
	return result;
}

static long sReadIdWholeNum(FitIniFile* missionFile, const char *varName, int &value) {
	long result = 0;
	unsigned long tmpULong;
	result = missionFile->readIdULong((char *)varName, tmpULong);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpULong;
	}
	return result;
}

static long sReadIdString(FitIniFile* missionFile, const char *varName, ECharString &ECStr) {
	long result = 0;
	char buffer[2001/*buffer size*/]; buffer[0] = '\0';
	result = missionFile->readIdString((char *)varName, buffer, 2001/*buffer size*/ - 1);
	if ((NO_ERR != result) && (BUFFER_TOO_SMALL != result)) {
		//assert(false);
	} else {
		CString CStr = buffer;
		/*readIdString can't read in "\r\n"*/
		CStr.Replace("\n", "\r\n");
		ECStr = CStr.GetBuffer(0);
	}
	return result;
}

static long sWriteIdString(FitIniFile* missionFile, const char *varName, const char *szStr) {
	if (!szStr) { return !(NO_ERR); }
	long result = 0;
	CString CStr = szStr;
	/*readIdString can't read in "\r\n"*/
	CStr.Replace("\r\n", "\n");
	result = missionFile->writeIdString(varName, CStr.GetBuffer(0));
	return result;
}

static bool areCloseEnough(float f1, float f2)
{
	bool retval = false;
	float diff = fabs(f1 - f2);
	float bigger = fabs(f1);
	if (fabs(f2) > bigger) {
		bigger = fabs(f2);
	}
	float proportion_diff = 0.0;
	if (0.0 != bigger) {
		proportion_diff = diff / bigger;
	}
	if ((pow(10.0, -FLT_DIG) * 10.0/*arbitrary*/) > proportion_diff) {
		retval = true;
	}
	return retval;
}

#endif // OBJECTIVE_INTERNAL_H
