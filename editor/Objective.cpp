/***************************************************************
* FILENAME: Objective.cpp
* DESCRIPTION: Implements Editor objective data and objective behavior.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster comments and attribution header.
****************************************************************/

#define OBJECTIVE_CPP

#include "Objective_internal.h"



CObjectiveConditionList &CObjectiveConditionList::operator=(const CObjectiveConditionList &master) {
	Clear();
	CObjectiveConditionList::EConstIterator it = master.Begin();
	while (!it.IsDone()) {
		condition_species_type conditionSpecies = (*it)->Species();
		CObjectiveCondition *pTmpCondition = CObjective::new_CObjectiveCondition(conditionSpecies, (*it)->Alignment());
		(*pTmpCondition).CastAndCopy(*it);
		Append(pTmpCondition);
		it++;
	}
	return (*this);
}

bool CObjectiveConditionList::operator==(const CObjectiveConditionList &rhs) const {
	bool retval = true;
	if (Count() != rhs.Count()) {
		retval = false;
	} else {
		CObjectiveConditionList::EConstIterator it = Begin();
		CObjectiveConditionList::EConstIterator it2 = rhs.Begin();
		while (!it.IsDone()) {
			assert(!it2.IsDone()); assert(*it); assert(*it2);
			if (!((*(*it)) == (*(*it2)))) {
				retval = false;
				break;
			}
			it++;
			it2++;
		}
	}
	return retval;
}

void CObjectiveConditionList::Clear() {
	EIterator it = Begin();
	while (!it.IsDone()) {
		delete (*it);
		it++;
	}
	EList <CObjectiveCondition *, CObjectiveCondition *>::Clear();
}

CObjectiveActionList &CObjectiveActionList::operator=(const CObjectiveActionList &master) {
	CObjectiveActionList::EConstIterator it = master.Begin();
	while (!it.IsDone()) {
		action_species_type actionSpecies = (*it)->Species();
		CObjectiveAction *pTmpAction = CObjective::new_CObjectiveAction(actionSpecies, (*it)->Alignment());
		(*pTmpAction).CastAndCopy(*it);
		Append(pTmpAction);
		it++;
	}
	return (*this);
}

bool CObjectiveActionList::operator==(const CObjectiveActionList &rhs) const {
	bool retval = true;
	if (Count() != rhs.Count()) {
		retval = false;
	} else {
		CObjectiveActionList::EConstIterator it = Begin();
		CObjectiveActionList::EConstIterator it2 = rhs.Begin();
		while (!it.IsDone()) {
			assert(!it2.IsDone()); assert(*it); assert(*it2);
			if (!((*(*it)) == (*(*it2)))) {
				retval = false;
				break;
			}
			it++;
			it2++;
		}
	}
	return retval;
}

void CObjectiveActionList::Clear() {
	EIterator it = Begin();
	while (!it.IsDone()) {
		delete (*it);
		it++;
	}
	EList <CObjectiveAction *, CObjectiveAction *>::Clear();
}

CObjectiveCondition *CObjective::new_CObjectiveCondition(condition_species_type conditionSpecies, int alignment) {
	CObjectiveCondition *retval = 0;
	switch (conditionSpecies) {
	case DESTROY_ALL_ENEMY_UNITS: retval = new CDestroyAllEnemyUnits(alignment); break;
	case DESTROY_NUMBER_OF_ENEMY_UNITS: retval = new CDestroyNumberOfEnemyUnits(alignment); break;
	case DESTROY_SPECIFIC_ENEMY_UNIT: retval = new CDestroySpecificEnemyUnit(alignment); break;
	case DESTROY_SPECIFIC_STRUCTURE: retval = new CDestroySpecificStructure(alignment); break;
	case CAPTURE_OR_DESTROY_ALL_ENEMY_UNITS: retval = new CCaptureOrDestroyAllEnemyUnits(alignment); break;
	case CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS: retval = new CCaptureOrDestroyNumberOfEnemyUnits(alignment); break;
	case CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT: retval = new CCaptureOrDestroySpecificEnemyUnit(alignment); break;
	case CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE: retval = new CCaptureOrDestroySpecificStructure(alignment); break;
	case DEAD_OR_FLED_ALL_ENEMY_UNITS: retval = new CDeadOrFledAllEnemyUnits(alignment); break;
	case DEAD_OR_FLED_NUMBER_OF_ENEMY_UNITS: retval = new CDeadOrFledNumberOfEnemyUnits(alignment); break;
	case DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT: retval = new CDeadOrFledSpecificEnemyUnit(alignment); break;
	case CAPTURE_UNIT: retval = new CCaptureUnit(alignment); break;
	case CAPTURE_STRUCTURE: retval = new CCaptureStructure(alignment); break;
	case GUARD_SPECIFIC_UNIT: retval = new CGuardSpecificUnit(alignment); break;
	case GUARD_SPECIFIC_STRUCTURE: retval = new CGuardSpecificStructure(alignment); break;
	case MOVE_ANY_UNIT_TO_AREA: retval = new CMoveAnyUnitToArea(alignment); break;
	case MOVE_ALL_UNITS_TO_AREA: retval = new CMoveAllUnitsToArea(alignment); break;
	case MOVE_ALL_SURVIVING_UNITS_TO_AREA: retval = new CMoveAllSurvivingUnitsToArea(alignment); break;
	case MOVE_ALL_SURVIVING_MECHS_TO_AREA: retval = new CMoveAllSurvivingMechsToArea(alignment); break;
	case BOOLEAN_FLAG_IS_SET: retval = new CBooleanFlagIsSet(alignment); break;
	case ELAPSED_MISSION_TIME: retval = new CElapsedMissionTime(alignment); break;
	default:
		assert(false);
		retval = 0;
		break;
	}
	return retval;
}

EString CObjective::DescriptionOfConditionSpecies(condition_species_type conditionSpecies) {
	CString CStr;
	CStr.LoadString(IDS_UNIMPLEMENTED_CONDITION);
	EString retval;
	retval = CStr.GetBuffer(0);
	CObjectiveCondition *tmpCondition = new_CObjectiveCondition(conditionSpecies, 0/*dummy alignment*/);
	if (0 != tmpCondition) {
		retval = tmpCondition->Description();
	}
	delete tmpCondition;
	return retval;
}

CObjectiveAction *CObjective::new_CObjectiveAction(action_species_type actionSpecies, int alignment) {
	CObjectiveAction *retval = 0;
	switch (actionSpecies) {
	case PLAY_BIK: retval = new CPlayBIK(alignment); break;
	case PLAY_WAV: retval = new CPlayWAV(alignment); break;
	case DISPLAY_TEXT_MESSAGE: retval = new CDisplayTextMessage(alignment); break;
	case DISPLAY_RESOURCE_TEXT_MESSAGE: retval = new CDisplayResourceTextMessage(alignment); break;
	case SET_BOOLEAN_FLAG: retval = new CSetBooleanFlag(alignment); break;
	case MAKE_NEW_TECHNOLOGY_AVAILABLE: retval = new CMakeNewTechnologyAvailable(alignment); break;
	default:
		assert(false);
		retval = 0;
		break;
	}
	return retval;
}

EString CObjective::DescriptionOfActionSpecies(action_species_type actionSpecies) {
	CString CStr;
	CStr.LoadString(IDS_UNIMPLEMENTED_ACTION);
	EString retval;
	retval = CStr.GetBuffer(0);
	CObjectiveAction *tmpAction = new_CObjectiveAction(actionSpecies, 0/*dummy alignment*/);
	if (0 != tmpAction) {
		retval = tmpAction->Description();
	}
	delete tmpAction;
	return retval;
}

void CObjective::Construct(int alignment) {
	m_alignment = alignment;
	Title(_TEXT(""));
	m_titleUseResourceString = false;
	m_titleResourceStringID = 0;
	Description(_TEXT(""));
	m_descriptionUseResourceString = false;
	m_descriptionResourceStringID = 0;
	m_priority = 1;
	m_resourcePoints = 0;
	m_previousPrimaryObjectiveMustBeComplete = false;
	m_allPreviousPrimaryObjectivesMustBeComplete = false;
	m_displayMarker = false;
	m_markerX = 0.0;
	m_markerY = 0.0;
	m_isHiddenTrigger = false;
	m_activateOnFlag = false;
	ActivateFlagID(_TEXT(""));
	m_resetStatusOnFlag = false;
	ResetStatusFlagID(_TEXT(""));
	m_baseColor = -1;
	m_highlightColor = -1;
	m_highlightColor2 = -1;
	m_modelID = -1;
}

CObjective &CObjective::operator=(const CObjective &master) {
	Clear();
	m_alignment = master.m_alignment;
	Title(master.Title());
	m_titleUseResourceString = master.m_titleUseResourceString;
	m_titleResourceStringID = master.m_titleResourceStringID;
	Description(master.Description());
	m_descriptionUseResourceString = master.m_descriptionUseResourceString;
	m_descriptionResourceStringID = master.m_descriptionResourceStringID;
	m_priority = master.m_priority;
	m_resourcePoints = master.m_resourcePoints;
	m_previousPrimaryObjectiveMustBeComplete = master.m_previousPrimaryObjectiveMustBeComplete;
	m_allPreviousPrimaryObjectivesMustBeComplete = master.m_allPreviousPrimaryObjectivesMustBeComplete;
	m_displayMarker = master.m_displayMarker;
	m_markerX = master.m_markerX;
	m_markerY = master.m_markerY;
	m_isHiddenTrigger = master.m_isHiddenTrigger;
	m_activateOnFlag = master.m_activateOnFlag;
	m_activateFlagID = master.m_activateFlagID;
	m_resetStatusOnFlag = master.m_resetStatusOnFlag;
	m_resetStatusFlagID = master.m_resetStatusFlagID;
	m_modelID = master.m_modelID;
	m_highlightColor = master.m_highlightColor;
	m_highlightColor2 = master.m_highlightColor2;
	m_baseColor = master.m_baseColor;

	CObjectiveConditionList::operator=(master);
	m_actionList = master.m_actionList;
	m_failureConditionList = master.m_failureConditionList;
	m_failureActionList = master.m_failureActionList;

	return (*this);
}

bool CObjective::operator==(const CObjective &rhs) const {
	bool retval = false;
	if ((m_alignment == rhs.m_alignment) &&
		(Title() == rhs.Title()) &&
		(m_titleUseResourceString == rhs.m_titleUseResourceString) &&
		(m_titleResourceStringID == rhs.m_titleResourceStringID) &&
		(Description() == rhs.Description()) &&
		(m_descriptionUseResourceString == rhs.m_descriptionUseResourceString) &&
		(m_descriptionResourceStringID == rhs.m_descriptionResourceStringID) &&
		(m_priority == rhs.m_priority) &&
		(m_resourcePoints == rhs.m_resourcePoints) &&
		(m_previousPrimaryObjectiveMustBeComplete == rhs.m_previousPrimaryObjectiveMustBeComplete) &&
		(m_allPreviousPrimaryObjectivesMustBeComplete == rhs.m_allPreviousPrimaryObjectivesMustBeComplete) &&
		(m_displayMarker == rhs.m_displayMarker) &&
		(m_markerX == rhs.m_markerX) &&
		(m_markerY == rhs.m_markerY) &&
		(m_isHiddenTrigger == rhs.m_isHiddenTrigger) &&
		(m_activateOnFlag == rhs.m_activateOnFlag) &&
		(m_activateFlagID == rhs.m_activateFlagID) &&
		(m_resetStatusOnFlag == rhs.m_resetStatusOnFlag) &&
		(m_resetStatusFlagID == rhs.m_resetStatusFlagID) &&
		(m_modelID == rhs.m_modelID) &&
		(m_highlightColor == rhs.m_highlightColor) &&
		(m_highlightColor2 == rhs.m_highlightColor2) &&
		(m_baseColor == rhs.m_baseColor)) {
		if ((inherited::operator==(rhs)) &&
			(m_actionList == rhs.m_actionList) &&
			(m_failureConditionList == rhs.m_failureConditionList) &&
			(m_failureActionList == rhs.m_failureActionList)) {
			retval = true;
		}
	}
	return retval;
}

void CObjective::Clear() {
	inherited::Clear();
	m_actionList.Clear();
	m_failureConditionList.Clear();
	m_failureActionList.Clear();
}

void CObjective::Alignment(int alignment) {
	m_alignment = alignment;
	{
		EIterator it = Begin();
		while (!it.IsDone()) {
			(*it)->Alignment(m_alignment);
			it++;
		}
	}

	{
		action_list_type::EIterator it = m_actionList.Begin();
		while (!it.IsDone()) {
			(*it)->Alignment(m_alignment);
			it++;
		}
	}

	{
		condition_list_type::EIterator it = m_failureConditionList.Begin();
		while (!it.IsDone()) {
			(*it)->Alignment(m_alignment);
			it++;
		}
	}

	{
		action_list_type::EIterator it = m_failureActionList.Begin();
		while (!it.IsDone()) {
			(*it)->Alignment(m_alignment);
			it++;
		}
	}
}

static condition_species_type ConditionSpeciesMap(const char *speciesString) {
	condition_species_type retval = DESTROY_ALL_ENEMY_UNITS;
	int i;
	for (i = 0; i < (int)NUM_CONDITION_SPECIES; i += 1) {
		if (0 == strcmp(speciesString, g_conditionSpeciesStringArray[i])) {
			retval = (condition_species_type)i;
			break;
		}
	}
	assert(i < (int)NUM_CONDITION_SPECIES);
	return retval;
}

static action_species_type ActionSpeciesMap(const char *speciesString) {
	action_species_type retval = PLAY_BIK;
	int i;
	for (i = 0; i < (int)NUM_ACTION_SPECIES; i += 1) {
		if (0 == strcmp(speciesString, g_actionSpeciesStringArray[i])) {
			retval = (action_species_type)i;
			break;
		}
	}
	assert(i < (int)NUM_ACTION_SPECIES);
	return retval;
}

bool CObjective::Read( FitIniFile* missionFile, int objectiveNum, int version )
{
	long result = 0;
	ECharString tmpECStr;
	result = sReadIdString(missionFile, "Title", tmpECStr);
	Title(_T(tmpECStr.Data()));
	result = sReadIdBoolean(missionFile, "TitleUseResourceString", m_titleUseResourceString);
	result = sReadIdWholeNum(missionFile, "TitleResourceStringID", m_titleResourceStringID);
	result = sReadIdString(missionFile, "Description", tmpECStr);
	Description(_T(tmpECStr.Data()));
	result = sReadIdBoolean(missionFile, "DescriptionUseResourceString", m_descriptionUseResourceString);
	result = sReadIdWholeNum(missionFile, "DescriptionResourceStringID", m_descriptionResourceStringID);

	result = sReadIdWholeNum(missionFile, "Priority", m_priority);
	result = sReadIdWholeNum(missionFile, "ResourcePoints", m_resourcePoints);
	result = sReadIdBoolean(missionFile, "PreviousPrimaryObjectiveMustBeComplete", m_previousPrimaryObjectiveMustBeComplete);
	if (NO_ERR != result) {
		result = sReadIdBoolean(missionFile, "PreviousObjectiveMustBeComplete", m_previousPrimaryObjectiveMustBeComplete);
	}
	result = sReadIdBoolean(missionFile, "AllPreviousPrimaryObjectivesMustBeComplete", m_allPreviousPrimaryObjectivesMustBeComplete);
	result = sReadIdBoolean(missionFile, "DisplayMarker", m_displayMarker);
	result = sReadIdFloat(missionFile, "MarkerX", m_markerX);
	result = sReadIdFloat(missionFile, "MarkerY", m_markerY);
	result = sReadIdBoolean(missionFile, "HiddenTrigger", m_isHiddenTrigger);
	result = sReadIdBoolean(missionFile, "ActivateOnFlag", m_activateOnFlag);
	result = sReadIdString(missionFile, "ActivateFlagID", tmpECStr);
	if (NO_ERR != result) {
		/*for backward compatibility*/
		int flagIndex = 0;
		result = sReadIdWholeNum(missionFile, "ActivateFlagIndex", flagIndex);
		if (NO_ERR == result) {
			m_activateFlagID.Format("%d", flagIndex);
		}
	} else {
		m_activateFlagID = tmpECStr.Data();
	}
	result = sReadIdBoolean(missionFile, "ResetStatusOnFlag", m_resetStatusOnFlag);
	result = sReadIdString(missionFile, "ResetStatusFlagID", tmpECStr);
	if (NO_ERR == result) {
		m_resetStatusFlagID = tmpECStr.Data();
	}

	if ( NO_ERR != missionFile->readIdLong( "BaseColor", m_baseColor ) )
		m_baseColor = -1;

	if ( NO_ERR != missionFile->readIdLong( "HighlightColor", m_highlightColor ) )
		m_highlightColor = -1;

	if ( missionFile->readIdLong( "HighlightColor2", m_highlightColor2 ) )
		m_highlightColor2 = -1;

	int modelIDVersion = 0;
	result = sReadIdWholeNum(missionFile, "ModelID Version", modelIDVersion);
	if ((NO_ERR != result) || (1 >= modelIDVersion) )
	{
		m_modelID = -1;
	} else {
		if ( NO_ERR != missionFile->readIdLong( "ModelID", m_modelID ) )
			m_modelID = -1;
	}

	


	int numConditions = 0;
	result = sReadIdWholeNum(missionFile, "NumConditions", numConditions);
	if (NO_ERR != result) { return false; }
	int numActions = 0;
	result = sReadIdWholeNum(missionFile, "NumActions", numActions);
	int numFailureConditions = 0;
	result = sReadIdWholeNum(missionFile, "NumFailureConditions", numFailureConditions);
	int numFailureActions = 0;
	result = sReadIdWholeNum(missionFile, "NumFailureActions", numFailureActions);

	{
		int i;
		for (i = 0; i < numConditions; i += 1) {
			ECharString tmpStr;
			if (2 == version) {
				tmpStr.Format("Objective%dCondition%d", objectiveNum, i);
			} else {
				tmpStr.Format("Team%dObjective%dCondition%d", Alignment(), objectiveNum, i);
			}
			result = missionFile->seekBlock(tmpStr.Data());
			if (NO_ERR != result) { assert(false); continue; }
			condition_species_type species;
			result = sReadIdString(missionFile, "ConditionSpeciesString", tmpECStr);
			if (NO_ERR != result) {
				/* this is just for backward compatibility, this entry is depricated */
				int ispecies = 0;
				result = sReadIdWholeNum(missionFile, "ConditionSpecies", ispecies);
				if (NO_ERR != result) { continue; }
				species = (condition_species_type)ispecies;
			} else {
				species = ConditionSpeciesMap(tmpECStr.Data());
			}

			CObjectiveCondition *pNewObjectiveCondition = new_CObjectiveCondition(species, m_alignment);
			if (!pNewObjectiveCondition) { assert(false); delete pNewObjectiveCondition; continue; }
			bool bresult = pNewObjectiveCondition->Read(missionFile);
			if (true != bresult) { assert(false); delete pNewObjectiveCondition; continue; }
			Append(pNewObjectiveCondition);
		}
	}

	{
		int i;
		for (i = 0; i < numActions; i += 1) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dAction%d", Alignment(), objectiveNum, i);
			result = missionFile->seekBlock(tmpStr.Data());
			if (NO_ERR != result) { assert(false); continue; }
			action_species_type species;
			result = sReadIdString(missionFile, "ActionSpeciesString", tmpECStr);
			if (NO_ERR != result) { assert(false); continue; }
			species = ActionSpeciesMap(tmpECStr.Data());

			CObjectiveAction *pNewObjectiveAction = new_CObjectiveAction(species, m_alignment);
			if (!pNewObjectiveAction) { assert(false); delete pNewObjectiveAction; continue; }
			bool bresult = pNewObjectiveAction->Read(missionFile);
			if (true != bresult) { assert(false); delete pNewObjectiveAction; continue; }
			m_actionList.Append(pNewObjectiveAction);
		}
	}

	{
		int i;
		for (i = 0; i < numFailureConditions; i += 1) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dFailureCondition%d", Alignment(), objectiveNum, i);
			result = missionFile->seekBlock(tmpStr.Data());
			if (NO_ERR != result) { assert(false); continue; }
			condition_species_type species;
			result = sReadIdString(missionFile, "FailureConditionSpeciesString", tmpECStr);
			if (NO_ERR != result) { assert(false); continue; }
			species = ConditionSpeciesMap(tmpECStr.Data());

			CObjectiveCondition *pNewObjectiveFailureCondition = new_CObjectiveCondition(species, m_alignment);
			if (!pNewObjectiveFailureCondition) { assert(false); delete pNewObjectiveFailureCondition; continue; }
			bool bresult = pNewObjectiveFailureCondition->Read(missionFile);
			if (true != bresult) { assert(false); delete pNewObjectiveFailureCondition; continue; }
			m_failureConditionList.Append(pNewObjectiveFailureCondition);
		}
	}

	{
		int i;
		for (i = 0; i < numFailureActions; i += 1) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dFailureAction%d", Alignment(), objectiveNum, i);
			result = missionFile->seekBlock(tmpStr.Data());
			if (NO_ERR != result) { assert(false); continue; }
			action_species_type species;
			result = sReadIdString(missionFile, "FailureActionSpeciesString", tmpECStr);
			if (NO_ERR != result) { assert(false); continue; }
			species = ActionSpeciesMap(tmpECStr.Data());

			CObjectiveAction *pNewObjectiveFailureAction = new_CObjectiveAction(species, m_alignment);
			if (!pNewObjectiveFailureAction) { assert(false); delete pNewObjectiveFailureAction; continue; }
			bool bresult = pNewObjectiveFailureAction->Read(missionFile);
			if (true != bresult) { assert(false); delete pNewObjectiveFailureAction; continue; }
			m_failureActionList.Append(pNewObjectiveFailureAction);
		}
	}
	return true;
}

bool CObjective::Save( FitIniFile* file, int objectiveNum )
{
	sWriteIdString( file, "Title", Title().Data());
	file->writeIdBoolean( "TitleUseResourceString", m_titleUseResourceString );
	file->writeIdULong( "TitleResourceStringID", m_titleResourceStringID );
	sWriteIdString( file, "Description", Description().Data());
	file->writeIdBoolean( "DescriptionUseResourceString", m_descriptionUseResourceString );
	file->writeIdULong( "DescriptionResourceStringID", m_descriptionResourceStringID );
	file->writeIdULong( "Priority", m_priority );
	file->writeIdULong( "ResourcePoints", m_resourcePoints );
	file->writeIdBoolean( "PreviousPrimaryObjectiveMustBeComplete", m_previousPrimaryObjectiveMustBeComplete );
	file->writeIdBoolean( "AllPreviousPrimaryObjectivesMustBeComplete", m_allPreviousPrimaryObjectivesMustBeComplete );
	file->writeIdBoolean( "DisplayMarker", m_displayMarker );
	file->writeIdFloat( "MarkerX", m_markerX );
	file->writeIdFloat( "MarkerY", m_markerY );
	file->writeIdBoolean( "HiddenTrigger", m_isHiddenTrigger );
	file->writeIdBoolean( "ActivateOnFlag", m_activateOnFlag );
	file->writeIdString( "ActivateFlagID", m_activateFlagID );
	file->writeIdBoolean( "ResetStatusOnFlag", m_resetStatusOnFlag );
	file->writeIdString( "ResetStatusFlagID", m_resetStatusFlagID.Data() );

	file->writeIdULong( "NumConditions", Count() );
	file->writeIdULong( "NumActions", m_actionList.Count() );
	file->writeIdULong( "NumFailureConditions", m_failureConditionList.Count() );
	file->writeIdULong( "NumFailureActions", m_failureActionList.Count() );

	const char* pName = "";
	int type = 0;
	float scale = 1.0;
	if ( m_modelID != -1 )
	{
		unsigned long ulGroup, ulIndex;
		// getBuildingFromID returns false when m_modelID references a building not
		// present in the editor object catalog (e.g. a mission authored against a
		// different/mod object set). On failure it leaves ulGroup past the end of the
		// groups array, so getID()/getFileName()/getScale() would index out of bounds
		// and read a garbage fileName pointer (AV at Objective.cpp). Guard it: skip the
		// model fields and serialize defaults rather than crash.
		if ( EditorObjectMgr::instance()->getBuildingFromID( m_modelID, ulGroup, ulIndex, true) )
		{
			int ID = EditorObjectMgr::instance()->getID( ulGroup, ulIndex );
			const char* pFileName = EditorObjectMgr::instance()->getFileName( ID );
			pName = pFileName ? pFileName : "";
			type = (ID >> 24) & 0xff;
			scale = EditorObjectMgr::instance()->getScale( ID );
		}
		else
		{
			char warnBuf[160];
			sprintf( warnBuf, "CObjective::Save: objective model ID %ld not found in editor object catalog; writing default model\n", (long)m_modelID );
			OutputDebugString( warnBuf );
		}
	}
	file->writeIdString( "ObjectiveModel", pName );
	file->writeIdLong( "ModelType", type );
	file->writeIdLong( "BaseColor", m_baseColor );
	file->writeIdLong( "HighlightColor", m_highlightColor );
	file->writeIdLong( "HighlightColor2", m_highlightColor2 );
	file->writeIdULong( "ModelID Version", 2 );
	file->writeIdLong( "ModelID", m_modelID );
	file->writeIdFloat( "ModelScale", scale );

	

	{
		int i = 0;
		EIterator it = Begin();
		while (!it.IsDone()) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dCondition%d", Alignment(), objectiveNum, i);
			file->writeBlock( tmpStr.Data() );
			file->writeIdULong( "ConditionSpecies", (unsigned long)(*it)->Species());
			file->writeIdString( "ConditionSpeciesString", g_conditionSpeciesStringArray[(int)(*it)->Species()]);
			(*it)->Save(file);
			i += 1;
			it++;
		}
	}

	{
		int i = 0;
		action_list_type::EIterator it = m_actionList.Begin();
		while (!it.IsDone()) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dAction%d", Alignment(), objectiveNum, i);
			file->writeBlock( tmpStr.Data() );
			file->writeIdULong( "ActionSpecies", (unsigned long)(*it)->Species());
			file->writeIdString( "ActionSpeciesString", g_actionSpeciesStringArray[(int)(*it)->Species()]);
			(*it)->Save(file);
			i += 1;
			it++;
		}
	}

	{
		int i = 0;
		condition_list_type::EIterator it = m_failureConditionList.Begin();
		while (!it.IsDone()) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dFailureCondition%d", Alignment(), objectiveNum, i);
			file->writeBlock( tmpStr.Data() );
			file->writeIdULong( "FailureConditionSpecies", (unsigned long)(*it)->Species());
			file->writeIdString( "FailureConditionSpeciesString", g_conditionSpeciesStringArray[(int)(*it)->Species()]);
			(*it)->Save(file);
			i += 1;
			it++;
		}
	}

	{
		int i = 0;
		action_list_type::EIterator it = m_failureActionList.Begin();
		while (!it.IsDone()) {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjective%dFailureAction%d", Alignment(), objectiveNum, i);
			file->writeBlock( tmpStr.Data() );
			file->writeIdULong( "FailureActionSpecies", (unsigned long)(*it)->Species());
			file->writeIdString( "FailureActionSpeciesString", g_actionSpeciesStringArray[(int)(*it)->Species()]);
			(*it)->Save(file);
			i += 1;
			it++;
		}
	}

	return true;
}

bool CObjective::EditDialog() {
	ObjectiveDlg ObjectiveDialog;

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		ObjectiveDialog.m_ModifiedObjective = EditorInterface::instance()->objectivesEditState.ModifiedObjective;
		ObjectiveDialog.nConditionSpeciesSelectionIndex = EditorInterface::instance()->objectivesEditState.nConditionSpeciesSelectionIndex;
		ObjectiveDialog.nActionSpeciesSelectionIndex = EditorInterface::instance()->objectivesEditState.nActionSpeciesSelectionIndex;
		ObjectiveDialog.nFailureConditionSpeciesSelectionIndex = EditorInterface::instance()->objectivesEditState.nFailureConditionSpeciesSelectionIndex;
		ObjectiveDialog.nFailureActionSpeciesSelectionIndex = EditorInterface::instance()->objectivesEditState.nFailureActionSpeciesSelectionIndex;
	} else {
		ObjectiveDialog.m_ModifiedObjective = (*this);
		ObjectiveDialog.nConditionSpeciesSelectionIndex = -1;
		ObjectiveDialog.nActionSpeciesSelectionIndex = -1;
		ObjectiveDialog.nFailureConditionSpeciesSelectionIndex = -1;
		ObjectiveDialog.nFailureActionSpeciesSelectionIndex = -1;
	}

	int ret = ObjectiveDialog.DoModal();

	if (!EditorInterface::instance()->ObjectSelectOnlyMode()) {
		if (IDOK == ret) {
			(*this) = ObjectiveDialog.m_ModifiedObjective;
		}
	}
	return (IDOK == ret);
}

bool CObjective::NoteThePositionsOfObjectsReferenced() {
	bool retval = true;
	{
		EIterator it = Begin();
		while (!it.IsDone()) {
			bool res = (*it)->NoteThePositionsOfObjectsReferenced();
			if (false == res) {
				retval = false;
			}
			it++;
		}
	}
	{
		condition_list_type::EIterator it = m_failureConditionList.Begin();
		while (!it.IsDone()) {
			bool res = (*it)->NoteThePositionsOfObjectsReferenced();
			if (false == res) {
				retval = false;
			}
			it++;
		}
	}
	return retval;
}

bool CObjective::RestoreObjectPointerReferencesFromNotedPositions() {
	bool retval = true;
	{
		EIterator it = Begin();
		while (!it.IsDone()) {
			bool res = (*it)->RestoreObjectPointerReferencesFromNotedPositions();
			if (false == res) {
				retval = false;
			}
			it++;
		}
	}
	{
		condition_list_type::EIterator it = m_failureConditionList.Begin();
		while (!it.IsDone()) {
			bool res = (*it)->RestoreObjectPointerReferencesFromNotedPositions();
			if (false == res) {
				retval = false;
			}
			it++;
		}
	}
	return retval;
}

static BOOL CSLoadString(int resourceID, CString &targetStr) {
	char szTmp[16384/*max string length*/];
	EditorSafeLoadString( resourceID, szTmp, 16384/*max string length*/ );
	targetStr = szTmp;
	CString tmpStr;
	tmpStr.Format("mc2res.dll:%d Not defined", resourceID);
	if (0 == strcmp(tmpStr.GetBuffer(0), szTmp)) {
		return (0);
	}
	return (!0);
}

EString CObjective::LocalizedTitle() const {
	EString retval;
	if (TitleUseResourceString()) {
		CString CStr;
		CSLoadString(TitleResourceStringID(), CStr);
		retval = CStr.GetBuffer(0);
	} else {
		retval = Title();
	}
	return retval;
}

EString CObjective::LocalizedDescription() const {
	EString retval;
	if (DescriptionUseResourceString()) {
		CString CStr;
		CSLoadString(DescriptionResourceStringID(), CStr);
		retval = CStr.GetBuffer(0);
	} else {
		retval = Description();
	}
	return retval;
}

CObjectives &CObjectives::operator=(const CObjectives &master) {
	m_alignment = master.m_alignment;

	Clear();
	CObjectives::EConstIterator it = master.Begin();
	while (!it.IsDone()) {
		//CObjective *tmpObjective = new CObjective(*(*it));
		CObjective *tmpObjective = new CObjective(m_alignment);
		(*tmpObjective) = (*(*it));
		Append(tmpObjective);
		it++;
	}
	return (*this);
}

bool CObjectives::operator==(const CObjectives &rhs) const {
	bool retval = true;
	if (m_alignment != rhs.m_alignment) {
		retval = false;
	} else if (Count() != rhs.Count()) {
		retval = false;
	} else {
		EConstIterator it = Begin();
		EConstIterator it2 = rhs.Begin();
		while (!it.IsDone()) {
			if (!((*(*it)) == (*(*it2)))) {
				retval = false;
				break;
			}
			it2++;
			it++;
		}
	}
	return retval;
}

void CObjectives::Clear() {
	EIterator it = Begin();
	while (!it.IsDone()) {
		delete (*it);
		it++;
	}
	inherited::Clear();
}

void CObjectives::Alignment(int alignment) {
	m_alignment = alignment;
	EIterator it = Begin();
	while (!it.IsDone()) {
		(*it)->Alignment(m_alignment);
		it++;
	}
}

bool CObjectives::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = missionFile->seekBlock("Objectives Version");
	if (NO_ERR != result) { assert(false); }
	int objectivesVersion = 0;
	result = sReadIdWholeNum(missionFile, "Version", objectivesVersion);
	if (result != NO_ERR) {
		assert(false); return false;
	} else {
		if (2 == objectivesVersion) {
			if (0 != Alignment()) {
				// assign version 2 objectives to team1 only
				return true;
			}
			result = missionFile->seekBlock("Objectives");
		} else {
			ECharString tmpStr;
			tmpStr.Format("Team%dObjectives", Alignment());
			result = missionFile->seekBlock(tmpStr.Data());
		}
		if (NO_ERR != result) { assert(false); }
		int numObjectives = 0;
		result = sReadIdWholeNum(missionFile, "NumObjectives", numObjectives);
		if (NO_ERR != result) { return false; }
		int i;
		for (i = 0; i < numObjectives; i += 1) {
			ECharString tmpStr;
			if (2 == objectivesVersion) {
				tmpStr.Format("Objective%d", i);
			} else {
				tmpStr.Format("Team%dObjective%d", Alignment(), i);
			}
			result = missionFile->seekBlock(tmpStr.Data());
			if (NO_ERR != result) { assert(false); continue; }
			CObjective *pNewObjective = new CObjective(m_alignment);
			bool bresult = pNewObjective->Read(missionFile, i, objectivesVersion);
			if (true != bresult) { assert(false); delete pNewObjective; continue; }
			Append(pNewObjective);
		}
	}
	return true;
}

bool CObjectives::Save( FitIniFile* file )
{
	ECharString tmpStr;
	tmpStr.Format("Team%dObjectives", Alignment());
	file->writeBlock( tmpStr.Data() );
	file->writeIdULong( "NumObjectives", Count() );
	int i = 0;
	EIterator it = Begin();
	while (!it.IsDone()) {
		ECharString tmpStr;
		tmpStr.Format("Team%dObjective%d", Alignment(), i);
		file->writeBlock( tmpStr.Data() );
		(*it)->Save(file, i);
		i += 1;
		it++;
	}

	return true;
}

bool CObjectives::EditDialog() {
	ObjectivesDlg ObjectivesDialog;

	if (EditorInterface::instance()->ObjectSelectOnlyMode()) {
		ObjectivesDialog.m_ModifiedObjectives = EditorInterface::instance()->objectivesEditState.ModifiedObjectives;
		ObjectivesDialog.nSelectionIndex = EditorInterface::instance()->objectivesEditState.nSelectionIndex;
	} else {
		ObjectivesDialog.m_ModifiedObjectives = (*this);
		ObjectivesDialog.nSelectionIndex = -1;
	}

	int ret = ObjectivesDialog.DoModal();

	if (!EditorInterface::instance()->ObjectSelectOnlyMode()) {
		if (IDOK == ret) {
			(*this) = ObjectivesDialog.m_ModifiedObjectives;
		}
	}
	return (IDOK == ret);
}

bool CObjectives::WriteMissionScript(const char *Name, const char *OutputPath)
{
	return true;
}

void CObjectives::handleObjectInvalidation(const EditorObject *pObj) {
	/* if we cared about performance, we could maintain a mapping */
	if (Count() <= 0) {
		return;
	}
	EIterator it = Begin();
	while (!it.IsDone()) {
		CObjective *pObjective = *it;
		if (pObjective->Count() > 0) {
			/*do success conditions*/
			CObjective::EIterator it2 = pObjective->End();
			while (!it2.IsDone()) {
				if (true == (*it2)->RefersTo(pObj)) {
					CObjective::EIterator it3 = it2;
					it2--;
					bool weAreDeletingTheLastCondition = false;
					if (it2.IsDone()) {
						weAreDeletingTheLastCondition = true;
					}
					delete (*it3);
					pObjective->Delete(it3);
					if (weAreDeletingTheLastCondition) {
						/* after deleting the last element in the EList, it seems that any iterators
						of that EList (eg. it2) are no longer valid, so we have to break out of the
						loop here so that we don't try refer to it2 again (i.e. evaluating it2.IsDone()). */
						break;
					}
				} else {
					it2--;
				}
			}
		}
		if (pObjective->m_failureConditionList.Count() > 0) {
			/*do failure conditions*/
			CObjective::EIterator it2 = pObjective->m_failureConditionList.End();
			while (!it2.IsDone()) {
				if (true == (*it2)->RefersTo(pObj)) {
					CObjective::EIterator it3 = it2;
					it2--;
					bool weAreDeletingTheLastCondition = false;
					if (it2.IsDone()) {
						weAreDeletingTheLastCondition = true;
					}
					delete (*it3);
					pObjective->m_failureConditionList.Delete(it3);
					if (weAreDeletingTheLastCondition) {
						/* after deleting the last element in the EList, it seems that any iterators
						of that EList (eg. it2) are no longer valid, so we have to break out of the
						loop here so that we don't try refer to it2 again (i.e. evaluating it2.IsDone()). */
						break;
					}
				} else {
					it2--;
				}
			}
		}
		it++;
	}
}

bool CObjectives::NoteThePositionsOfObjectsReferenced() {
	bool retval = true;
	EIterator it = Begin();
	while (!it.IsDone()) {
		bool res = (*it)->NoteThePositionsOfObjectsReferenced();
		if (false == res) {
			retval = false;
		}
		it++;
	}
	return retval;
}

bool CObjectives::RestoreObjectPointerReferencesFromNotedPositions() {
	bool retval = true;
	EIterator it = Begin();
	while (!it.IsDone()) {
		bool res = (*it)->RestoreObjectPointerReferencesFromNotedPositions();
		if (false == res) {
			retval = false;
		}
		it++;
	}
	return retval;
}

bool CObjectives::ThereAreObjectivesWithNoConditions() {
	bool retval = false;
	EIterator it = Begin();
	while (!it.IsDone()) {
		bool res = (*it)->ThisObjectiveHasNoConditions();
		if (true == res) {
			retval = true;
			break;
		}
		it++;
	}
	return retval;
}
