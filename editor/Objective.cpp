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

#include "stdafx.h"
#include "Objective_internal.h"
#include "ObjectiveDlg.h"
#include "ObjectivesDlg.h"
#include "EditorInterface.h"
#include "EditorObjects.h"
#include "EditorObjectMgr.h"
#include "MCLibResource.h"
#include "../resource.h"
#include <cassert>



CObjectiveConditionList &CObjectiveConditionList::operator=(const CObjectiveConditionList &master) {
	Clear();
	auto it = master.entries.begin();
	while (it != master.entries.end()) {
		condition_species_type conditionSpecies = (*it)->Species();
		CObjectiveCondition *pTmpCondition = CObjective::new_CObjectiveCondition(conditionSpecies, (*it)->Alignment());
		(*pTmpCondition).CastAndCopy(*it);
		entries.push_back(pTmpCondition);
		it++;
	}
	return (*this);
}

bool CObjectiveConditionList::operator==(const CObjectiveConditionList &rhs) const {
	bool retval = true;
	if (entries.size() != rhs.entries.size()) {
		retval = false;
	} else {
		auto it = entries.begin();
		auto it2 = rhs.entries.begin();
		while (it != entries.end()) {
			assert(it2 != rhs.entries.end()); assert(*it); assert(*it2);
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
	auto it = entries.begin();
	while (it != entries.end()) {
		delete (*it);
		it++;
	}
	entries.clear();
}

CObjectiveActionList &CObjectiveActionList::operator=(const CObjectiveActionList &master) {
	auto it = master.entries.begin();
	while (it != master.entries.end()) {
		action_species_type actionSpecies = (*it)->Species();
		CObjectiveAction *pTmpAction = CObjective::new_CObjectiveAction(actionSpecies, (*it)->Alignment());
		(*pTmpAction).CastAndCopy(*it);
		entries.push_back(pTmpAction);
		it++;
	}
	return (*this);
}

bool CObjectiveActionList::operator==(const CObjectiveActionList &rhs) const {
	bool retval = true;
	if (entries.size() != rhs.entries.size()) {
		retval = false;
	} else {
		auto it = entries.begin();
		auto it2 = rhs.entries.begin();
		while (it != entries.end()) {
			assert(it2 != rhs.entries.end()); assert(*it); assert(*it2);
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
	auto it = entries.begin();
	while (it != entries.end()) {
		delete (*it);
		it++;
	}
	entries.clear();
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

std::string CObjective::DescriptionOfConditionSpecies(condition_species_type conditionSpecies) {
	CString CStr;
	CStr.LoadString(IDS_UNIMPLEMENTED_CONDITION);
	std::string retval;
	retval = mc2::str::from_c(CStr.GetBuffer(0));
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

std::string CObjective::DescriptionOfActionSpecies(action_species_type actionSpecies) {
	CString CStr;
	CStr.LoadString(IDS_UNIMPLEMENTED_ACTION);
	std::string retval;
	retval = mc2::str::from_c(CStr.GetBuffer(0));
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
	m_modelDefinition = master.m_modelDefinition;
	m_highlightColor = master.m_highlightColor;
	m_highlightColor2 = master.m_highlightColor2;
	m_baseColor = master.m_baseColor;

	m_conditions = master.m_conditions;
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
		(mc2::str::iequals(m_activateFlagID, rhs.m_activateFlagID)) &&
		(m_resetStatusOnFlag == rhs.m_resetStatusOnFlag) &&
		(mc2::str::iequals(m_resetStatusFlagID, rhs.m_resetStatusFlagID)) &&
		(m_modelID == rhs.m_modelID) &&
		(m_modelDefinition == rhs.m_modelDefinition) &&
		(m_highlightColor == rhs.m_highlightColor) &&
		(m_highlightColor2 == rhs.m_highlightColor2) &&
		(m_baseColor == rhs.m_baseColor)) {
		if ((m_conditions == rhs.m_conditions) &&
			(m_actionList == rhs.m_actionList) &&
			(m_failureConditionList == rhs.m_failureConditionList) &&
			(m_failureActionList == rhs.m_failureActionList)) {
			retval = true;
		}
	}
	return retval;
}

void CObjective::Clear() {
	m_conditions.Clear();
	m_actionList.Clear();
	m_failureConditionList.Clear();
	m_failureActionList.Clear();
}

void CObjective::Alignment(int alignment) {
	m_alignment = alignment;
	{
		auto it = m_conditions.entries.begin();
		while (it != m_conditions.entries.end()) {
			(*it)->Alignment(m_alignment);
			it++;
		}
	}

	{
		auto it = m_actionList.entries.begin();
		while (it != m_actionList.entries.end()) {
			(*it)->Alignment(m_alignment);
			it++;
		}
	}

	{
		auto it = m_failureConditionList.entries.begin();
		while (it != m_failureConditionList.entries.end()) {
			(*it)->Alignment(m_alignment);
			it++;
		}
	}

	{
		auto it = m_failureActionList.entries.begin();
		while (it != m_failureActionList.entries.end()) {
			(*it)->Alignment(m_alignment);
			it++;
		}
	}
}

bool CObjective::Read( FitIniFile* missionFile, int objectiveNum, int version )
{
	long result = 0;
	objective_text::Fields text;
	text.title = m_title;
	text.description = m_description;
	text.activateFlagID = m_activateFlagID;
	// An absent activation ID preserves its value so existing flag references survive an edit.
	text.resetStatusFlagID = m_resetStatusFlagID;
	std::string value;
	const auto titleStatus = EditorFit::readText(*missionFile, "Title", value);
	if (titleStatus == EditorFit::TextStatus::Read) text.title = value;
	else if (titleStatus == EditorFit::TextStatus::Absent) text.title.clear();
	const auto descriptionStatus = EditorFit::readText(*missionFile, "Description", value);
	if (descriptionStatus == EditorFit::TextStatus::Read) text.description = value;
	else if (descriptionStatus == EditorFit::TextStatus::Absent) text.description = text.title;
	if (auto flag = EditorFit::flagID(*missionFile, "ActivateFlagID", "ActivateFlagIndex")) text.activateFlagID = std::move(*flag);
	EditorFit::readText(*missionFile, "ResetStatusFlagID", text.resetStatusFlagID);
	Title(std::move(text.title));
	result = sReadIdBoolean(missionFile, "TitleUseResourceString", m_titleUseResourceString);
	result = sReadIdWholeNum(missionFile, "TitleResourceStringID", m_titleResourceStringID);
	Description(std::move(text.description));
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
	m_activateFlagID = std::move(text.activateFlagID);
	result = sReadIdBoolean(missionFile, "ResetStatusOnFlag", m_resetStatusOnFlag);
	m_resetStatusFlagID = std::move(text.resetStatusFlagID);

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
	auto* reader = dynamic_cast<EditorFit::MissionTextReader*>(missionFile);
	if (!reader) return EditorFit::reject("objective model reader");
	if (!EditorFit::readObjectiveModel(*reader, m_modelDefinition).accepted) return false;
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
			std::string tmpStr;
			if (2 == version) {
				tmpStr = std::format("Objective{}Condition{}", objectiveNum, i);
			} else {
				tmpStr = std::format("Team{}Objective{}Condition{}", Alignment(), objectiveNum, i);
			}
			result = missionFile->seekBlock(tmpStr.c_str());
			if (NO_ERR != result) { assert(false); continue; }
			const auto speciesIndex = EditorFit::species(*missionFile, "ConditionSpeciesString", g_conditionSpeciesStringArray, "ConditionSpecies");
			if (!speciesIndex) continue;
			const auto species = static_cast<condition_species_type>(*speciesIndex);

			CObjectiveCondition *pNewObjectiveCondition = new_CObjectiveCondition(species, m_alignment);
			if (!pNewObjectiveCondition) { continue; }
			bool bresult = pNewObjectiveCondition->Read(missionFile);
			if (true != bresult) {
				fprintf(stderr, "[EDITOR_DOCUMENT v1] mission=%s block=%s species=%s reason=condition_read_failed rejected=1\n",
					reader->missionName().c_str(), tmpStr.c_str(), g_conditionSpeciesStringArray[*speciesIndex]);
				delete pNewObjectiveCondition; return false;
			}
			m_conditions.entries.push_back(pNewObjectiveCondition);
		}
	}

	{
		int i;
		for (i = 0; i < numActions; i += 1) {
			std::string tmpStr;
			tmpStr = std::format("Team{}Objective{}Action{}", Alignment(), objectiveNum, i);
			result = missionFile->seekBlock(tmpStr.c_str());
			if (NO_ERR != result) { assert(false); continue; }
			const auto speciesIndex = EditorFit::species(*missionFile, "ActionSpeciesString", g_actionSpeciesStringArray);
			if (!speciesIndex) continue;
			const auto species = static_cast<action_species_type>(*speciesIndex);

			CObjectiveAction *pNewObjectiveAction = new_CObjectiveAction(species, m_alignment);
			if (!pNewObjectiveAction) { continue; }
			bool bresult = pNewObjectiveAction->Read(missionFile);
			if (true != bresult) { assert(false); delete pNewObjectiveAction; continue; }
			m_actionList.entries.push_back(pNewObjectiveAction);
		}
	}

	{
		int i;
		for (i = 0; i < numFailureConditions; i += 1) {
			std::string tmpStr;
			tmpStr = std::format("Team{}Objective{}FailureCondition{}", Alignment(), objectiveNum, i);
			result = missionFile->seekBlock(tmpStr.c_str());
			if (NO_ERR != result) { assert(false); continue; }
			const auto speciesIndex = EditorFit::species(*missionFile, "FailureConditionSpeciesString", g_conditionSpeciesStringArray);
			if (!speciesIndex) continue;
			const auto species = static_cast<condition_species_type>(*speciesIndex);

			CObjectiveCondition *pNewObjectiveFailureCondition = new_CObjectiveCondition(species, m_alignment);
			if (!pNewObjectiveFailureCondition) { continue; }
			bool bresult = pNewObjectiveFailureCondition->Read(missionFile);
			if (true != bresult) {
				fprintf(stderr, "[EDITOR_DOCUMENT v1] mission=%s block=%s species=%s reason=condition_read_failed rejected=1\n",
					reader->missionName().c_str(), tmpStr.c_str(), g_conditionSpeciesStringArray[*speciesIndex]);
				delete pNewObjectiveFailureCondition; return false;
			}
			m_failureConditionList.entries.push_back(pNewObjectiveFailureCondition);
		}
	}

	{
		int i;
		for (i = 0; i < numFailureActions; i += 1) {
			std::string tmpStr;
			tmpStr = std::format("Team{}Objective{}FailureAction{}", Alignment(), objectiveNum, i);
			result = missionFile->seekBlock(tmpStr.c_str());
			if (NO_ERR != result) { assert(false); continue; }
			const auto speciesIndex = EditorFit::species(*missionFile, "FailureActionSpeciesString", g_actionSpeciesStringArray);
			if (!speciesIndex) continue;
			const auto species = static_cast<action_species_type>(*speciesIndex);

			CObjectiveAction *pNewObjectiveFailureAction = new_CObjectiveAction(species, m_alignment);
			if (!pNewObjectiveFailureAction) { continue; }
			bool bresult = pNewObjectiveFailureAction->Read(missionFile);
			if (true != bresult) { assert(false); delete pNewObjectiveFailureAction; continue; }
			m_failureActionList.entries.push_back(pNewObjectiveFailureAction);
		}
	}
	return true;
}

void CObjective::ModelID(int newID)
{
	if (newID == m_modelID) return;
	const long previous = m_modelID;
	m_modelID = newID;
	if (!refreshModelFromCatalog()) m_modelID = previous;
}

bool CObjective::refreshModelFromCatalog()
{
	if (m_modelID == -1)
	{
		m_modelDefinition = {};
		return true;
	}
	unsigned long group = 0, index = 0;
	if (!EditorObjectMgr::instance()->getBuildingFromID(m_modelID, group, index, true))
	{
		fprintf(stderr, "[EDITOR_DOCUMENT v1] catalog refresh skipped unknown model %ld\n", m_modelID);
		return false;
	}
	const int id = EditorObjectMgr::instance()->getID(group, index);
	const char* name = EditorObjectMgr::instance()->getFileName(id);
	m_modelDefinition = {name ? name : "", (id >> 24) & 0xff, EditorObjectMgr::instance()->getScale(id)};
	return true;
}

bool CObjective::Save( FitIniFile* file, int objectiveNum )
{
	objective_text::write(*file, "Title", Title());
	file->writeIdBoolean( "TitleUseResourceString", m_titleUseResourceString );
	file->writeIdULong( "TitleResourceStringID", m_titleResourceStringID );
	objective_text::write(*file, "Description", Description());
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
	file->writeIdString("ActivateFlagID", m_activateFlagID.c_str());
	file->writeIdBoolean( "ResetStatusOnFlag", m_resetStatusOnFlag );
	file->writeIdString("ResetStatusFlagID", m_resetStatusFlagID.c_str());

	file->writeIdULong( "NumConditions", static_cast<unsigned long>(m_conditions.entries.size()) );
	file->writeIdULong( "NumActions", static_cast<unsigned long>(m_actionList.entries.size()) );
	file->writeIdULong( "NumFailureConditions", static_cast<unsigned long>(m_failureConditionList.entries.size()) );
	file->writeIdULong( "NumFailureActions", static_cast<unsigned long>(m_failureActionList.entries.size()) );

	file->writeIdString( "ObjectiveModel", m_modelDefinition.name.c_str() );
	file->writeIdLong( "ModelType", m_modelDefinition.type );
	file->writeIdLong( "BaseColor", m_baseColor );
	file->writeIdLong( "HighlightColor", m_highlightColor );
	file->writeIdLong( "HighlightColor2", m_highlightColor2 );
	file->writeIdULong( "ModelID Version", 2 );
	file->writeIdLong( "ModelID", m_modelID );
	file->writeIdFloat( "ModelScale", m_modelDefinition.scale );

	

	{
		int i = 0;
		auto it = m_conditions.entries.begin();
		while (it != m_conditions.entries.end()) {
			std::string tmpStr;
			tmpStr = std::format("Team{}Objective{}Condition{}", Alignment(), objectiveNum, i);
			file->writeBlock( tmpStr.c_str() );
			file->writeIdULong( "ConditionSpecies", (unsigned long)(*it)->Species());
			file->writeIdString( "ConditionSpeciesString", g_conditionSpeciesStringArray[(int)(*it)->Species()]);
			(*it)->Save(file);
			i += 1;
			it++;
		}
	}

	{
		int i = 0;
		auto it = m_actionList.entries.begin();
		while (it != m_actionList.entries.end()) {
			std::string tmpStr;
			tmpStr = std::format("Team{}Objective{}Action{}", Alignment(), objectiveNum, i);
			file->writeBlock( tmpStr.c_str() );
			file->writeIdULong( "ActionSpecies", (unsigned long)(*it)->Species());
			file->writeIdString( "ActionSpeciesString", g_actionSpeciesStringArray[(int)(*it)->Species()]);
			(*it)->Save(file);
			i += 1;
			it++;
		}
	}

	{
		int i = 0;
		auto it = m_failureConditionList.entries.begin();
		while (it != m_failureConditionList.entries.end()) {
			std::string tmpStr;
			tmpStr = std::format("Team{}Objective{}FailureCondition{}", Alignment(), objectiveNum, i);
			file->writeBlock( tmpStr.c_str() );
			file->writeIdULong( "FailureConditionSpecies", (unsigned long)(*it)->Species());
			file->writeIdString( "FailureConditionSpeciesString", g_conditionSpeciesStringArray[(int)(*it)->Species()]);
			(*it)->Save(file);
			i += 1;
			it++;
		}
	}

	{
		int i = 0;
		auto it = m_failureActionList.entries.begin();
		while (it != m_failureActionList.entries.end()) {
			std::string tmpStr;
			tmpStr = std::format("Team{}Objective{}FailureAction{}", Alignment(), objectiveNum, i);
			file->writeBlock( tmpStr.c_str() );
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
		auto it = m_conditions.entries.begin();
		while (it != m_conditions.entries.end()) {
			bool res = (*it)->NoteThePositionsOfObjectsReferenced();
			if (false == res) {
				retval = false;
			}
			it++;
		}
	}
	{
		auto it = m_failureConditionList.entries.begin();
		while (it != m_failureConditionList.entries.end()) {
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
		auto it = m_conditions.entries.begin();
		while (it != m_conditions.entries.end()) {
			bool res = (*it)->RestoreObjectPointerReferencesFromNotedPositions();
			if (false == res) {
				retval = false;
			}
			it++;
		}
	}
	{
		auto it = m_failureConditionList.entries.begin();
		while (it != m_failureConditionList.entries.end()) {
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

std::string CObjective::LocalizedTitle() const {
	std::string retval;
	if (TitleUseResourceString()) {
		CString CStr;
		CSLoadString(TitleResourceStringID(), CStr);
		retval = mc2::str::from_c(CStr.GetBuffer(0));
	} else {
		retval = Title();
	}
	return retval;
}

std::string CObjective::LocalizedDescription() const {
	std::string retval;
	if (DescriptionUseResourceString()) {
		CString CStr;
		CSLoadString(DescriptionResourceStringID(), CStr);
		retval = mc2::str::from_c(CStr.GetBuffer(0));
	} else {
		retval = Description();
	}
	return retval;
}

CObjectives &CObjectives::operator=(const CObjectives &master) {
	m_alignment = master.m_alignment;

	Clear();
	auto it = master.entries.begin();
	while (it != master.entries.end()) {
		CObjective *tmpObjective = new CObjective(m_alignment);
		(*tmpObjective) = (*(*it));
		entries.push_back(tmpObjective);
		it++;
	}
	return (*this);
}

bool CObjectives::operator==(const CObjectives &rhs) const {
	bool retval = true;
	if (m_alignment != rhs.m_alignment) {
		retval = false;
	} else if (entries.size() != rhs.entries.size()) {
		retval = false;
	} else {
		auto it = entries.begin();
		auto it2 = rhs.entries.begin();
		while (it != entries.end()) {
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
	auto it = entries.begin();
	while (it != entries.end()) {
		delete (*it);
		it++;
	}
	entries.clear();
}

void CObjectives::Alignment(int alignment) {
	m_alignment = alignment;
	auto it = entries.begin();
	while (it != entries.end()) {
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
			std::string tmpStr;
			tmpStr = std::format("Team{}Objectives", Alignment());
			result = missionFile->seekBlock(tmpStr.c_str());
		}
		if (NO_ERR != result) { assert(false); }
		int numObjectives = 0;
		result = sReadIdWholeNum(missionFile, "NumObjectives", numObjectives);
		if (NO_ERR != result) { return false; }
		int i;
		for (i = 0; i < numObjectives; i += 1) {
			std::string tmpStr;
			if (2 == objectivesVersion) {
				tmpStr = std::format("Objective{}", i);
			} else {
				tmpStr = std::format("Team{}Objective{}", Alignment(), i);
			}
			result = missionFile->seekBlock(tmpStr.c_str());
			if (NO_ERR != result) { assert(false); continue; }
			CObjective *pNewObjective = new CObjective(m_alignment);
			bool bresult = pNewObjective->Read(missionFile, i, objectivesVersion);
			if (true != bresult) { delete pNewObjective; return false; }
			entries.push_back(pNewObjective);
		}
	}
	return true;
}

bool CObjectives::Save( FitIniFile* file )
{
	std::string tmpStr;
	tmpStr = std::format("Team{}Objectives", Alignment());
	file->writeBlock( tmpStr.c_str() );
	file->writeIdULong( "NumObjectives", static_cast<unsigned long>(entries.size()) );
	int i = 0;
	auto it = entries.begin();
	while (it != entries.end()) {
		std::string tmpStr;
		tmpStr = std::format("Team{}Objective{}", Alignment(), i);
		file->writeBlock( tmpStr.c_str() );
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
	for (auto it = entries.begin(); it != entries.end(); it++) {
		CObjective *pObjective = *it;
		for (auto* conditions : {&pObjective->m_conditions, &pObjective->m_failureConditionList}) {
			for (size_t index = conditions->entries.size(); index > 0; --index) {
				auto* condition = conditions->entries[index - 1];
				if (condition->RefersTo(pObj)) {
					delete condition;
					conditions->entries.erase(conditions->entries.begin() + index - 1);
				}
			}
		}
	}
}

bool CObjectives::NoteThePositionsOfObjectsReferenced() {
	bool retval = true;
	auto it = entries.begin();
	while (it != entries.end()) {
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
	auto it = entries.begin();
	while (it != entries.end()) {
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
	auto it = entries.begin();
	while (it != entries.end()) {
		bool res = (*it)->ThisObjectiveHasNoConditions();
		if (true == res) {
			retval = true;
			break;
		}
		it++;
	}
	return retval;
}
