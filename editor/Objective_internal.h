// Shared scalar reads and localized strings keep objective defaults consistent
// across the objective, condition and action translation units.
#ifndef OBJECTIVE_INTERNAL_H
#define OBJECTIVE_INTERNAL_H

#include "Objective.h"
#include "mc2str.h"
#include "inifile.h"
#include <cstring>
#include <format>
#include "EditorResourceFallback.h"

inline bool LoadObjectiveString(int resourceID, std::string &targetStr) {
	char szTmp[16384/*max string length*/];
	EditorSafeLoadString( resourceID, szTmp, 16384/*max string length*/ );
	targetStr = mc2::str::from_c(szTmp);
	std::string tmpStr;
	tmpStr = std::format("mc2res.dll:{} Not defined", resourceID);
	if (0 == strcmp(tmpStr.c_str(), szTmp)) {
		return (0);
	}
	return (!0);
}

inline long sReadIdFloat(FitIniFile* missionFile, const char *varName, float &value) {
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

inline long sReadIdBoolean(FitIniFile* missionFile, const char *varName, bool &value) {
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

inline long sReadIdWholeNum(FitIniFile* missionFile, const char *varName, int &value) {
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

#endif // OBJECTIVE_INTERNAL_H
