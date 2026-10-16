#ifndef EDITOR_OBJECTIVE_H
#define EDITOR_OBJECTIVE_H
/*************************************************************************************************\
Objective.h			: Interface for the Objective component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "elist.h"
#include <vector>
#include <string>
#include <utility>
#include "tchar.h"
#include "AuthoredMissionState.h"

class EditorObject;
class Unit;
class FitIniFile;


/**************************************************************************************************
CLASS DESCRIPTION
CObjectiveCondition:
**************************************************************************************************/
enum condition_species_type {
	DESTROY_ALL_ENEMY_UNITS,
	DESTROY_NUMBER_OF_ENEMY_UNITS,
	DESTROY_ENEMY_UNIT_GROUP,
	DESTROY_SPECIFIC_ENEMY_UNIT,
	DESTROY_SPECIFIC_STRUCTURE,

	CAPTURE_OR_DESTROY_ALL_ENEMY_UNITS,
	CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS,
	CAPTURE_OR_DESTROY_ENEMY_UNIT_GROUP,
	CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT,
	CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE,

	DEAD_OR_FLED_ALL_ENEMY_UNITS,
	DEAD_OR_FLED_NUMBER_OF_ENEMY_UNITS,
	DEAD_OR_FLED_ENEMY_UNIT_GROUP,
	DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT,

	CAPTURE_UNIT,
	CAPTURE_STRUCTURE,

	GUARD_SPECIFIC_UNIT,
	GUARD_SPECIFIC_STRUCTURE,

	MOVE_ANY_UNIT_TO_AREA,
	MOVE_ALL_UNITS_TO_AREA,
	MOVE_ALL_SURVIVING_UNITS_TO_AREA,
	MOVE_ALL_SURVIVING_MECHS_TO_AREA,

	BOOLEAN_FLAG_IS_SET,
	ELAPSED_MISSION_TIME,

	NUM_CONDITION_SPECIES
};

static const char *g_conditionSpeciesStringArray[] = {
	"DestroyAllEnemyUnits",
		"DestroyNumberOfEnemyUnits",
		"DestroyEnemyUnitGroup",
		"DestroySpecificEnemyUnit",
		"DestroySpecificStructure",

		"CaptureOrDestroyAllEnemyUnits",
		"CaptureOrDestroyNumberOfEnemyUnits",
		"CaptureOrDestroyEnemyUnitGroup",
		"CaptureOrDestroySpecificEnemyUnit",
		"CaptureOrDestroySpecificStructure",

		"DeadOrFledAllEnemyUnits",
		"DeadOrFledNumberOfEnemyUnits",
		"DeadOrFledEnemyUnitGroup",
		"DeadOrFledSpecificEnemyUnit",

		"CaptureSpecificUnit",
		"CaptureSpecificStructure",

		"GuardSpecificUnit",
		"GuardSpecificStructure",

		"MoveAnyUnitToArea",
		"MoveAllUnitsToArea",
		"MoveAllSurvivingUnitsToArea",
		"MoveAllSurvivingMechsToArea",

		"BooleanFlagIsSet",
		"ElapsedMissionTime",
};

class CObjectiveCondition {
private:
	int m_alignment;
public:
	CObjectiveCondition(int alignment) { m_alignment = alignment; }
	virtual ~CObjectiveCondition() {}
	virtual bool operator==(const CObjectiveCondition &rhs) const;
	int Alignment() { return m_alignment; }
	void Alignment(int alignment) { m_alignment = alignment; }
	bool DoCommonEditDialog() {}
	virtual condition_species_type Species() const = 0;
	virtual bool Init() { return true; }
	virtual bool Read( FitIniFile* missionFile ) { return true; }
	virtual bool Save( FitIniFile* file ) { return true; }
	virtual bool EditDialog() { return true; }
	virtual void WriteAbl() {}
	virtual std::string Description() = 0;
	virtual std::string InstanceDescription() { return {}; }
	virtual void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*pMaster); }
	virtual bool RefersTo(const EditorObject *pObj) { return false; }
	virtual bool NoteThePositionsOfObjectsReferenced() { return true; }
	virtual bool RestoreObjectPointerReferencesFromNotedPositions() { return true; }
};

class CDestroyAllEnemyUnits: public CObjectiveCondition {
public:
	CDestroyAllEnemyUnits(int alignment) : CObjectiveCondition(alignment) {}
	condition_species_type Species() const { return DESTROY_ALL_ENEMY_UNITS; }
	std::string Description();
};

class CNumberOfEnemyUnitsObjectiveCondition: public CObjectiveCondition { /*abstract class*/
protected:
	int m_num;
public:
	CNumberOfEnemyUnitsObjectiveCondition(int alignment) : CObjectiveCondition(alignment) { m_num = 0; }
	virtual bool operator==(const CObjectiveCondition &rhs) const;
	virtual bool Read( FitIniFile* missionFile );
	virtual bool Save( FitIniFile* file );
	virtual bool EditDialog();
	virtual std::string InstanceDescription();
	virtual void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*(dynamic_cast<const CNumberOfEnemyUnitsObjectiveCondition *>(pMaster))); }
	int Num() { return m_num; }   // readable-line accessor (additive)
	void Num(int v) { m_num = v; }   // template-preset setter (additive)
};

class CDestroyNumberOfEnemyUnits: public CNumberOfEnemyUnitsObjectiveCondition {
public:
	CDestroyNumberOfEnemyUnits(int alignment) : CNumberOfEnemyUnitsObjectiveCondition(alignment) {}
	condition_species_type Species() const { return DESTROY_NUMBER_OF_ENEMY_UNITS; }
	std::string Description();
};

class CSpecificUnitObjectiveCondition: public CObjectiveCondition { /*abstract class*/
protected:
	AuthoredTargetPosition m_authoredPosition;
	Unit *m_pUnit;
	float m_LastNotedPositionX;
	float m_LastNotedPositionY;
public:
	CSpecificUnitObjectiveCondition(int alignment) : CObjectiveCondition(alignment) { m_pUnit = 0; m_LastNotedPositionX = m_LastNotedPositionY = 0.0; }
	virtual bool operator==(const CObjectiveCondition &rhs) const;
	virtual bool Read( FitIniFile* missionFile );
	virtual bool Save( FitIniFile* file );
	virtual bool EditDialog();
	virtual std::string InstanceDescription();
	virtual void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*(dynamic_cast<const CSpecificUnitObjectiveCondition *>(pMaster))); }
	virtual bool RefersTo(const EditorObject *pObj) { return (pObj == ((const EditorObject *)m_pUnit)); }
	virtual bool NoteThePositionsOfObjectsReferenced();
	virtual bool RestoreObjectPointerReferencesFromNotedPositions();
	// readable-line accessors (additive). UnitPtr() may be null; deref only when set.
	Unit *UnitPtr() { return m_pUnit; }
	float NotedX() { return m_LastNotedPositionX; }
	float NotedY() { return m_LastNotedPositionY; }
	std::string ReadableTarget();
};

class CSpecificEnemyUnitObjectiveCondition: public CSpecificUnitObjectiveCondition { /*abstract class*/
public:
	CSpecificEnemyUnitObjectiveCondition(int alignment) : CSpecificUnitObjectiveCondition(alignment) {}
	virtual bool EditDialog();
};

class CSpecificAlliedUnitObjectiveCondition: public CSpecificUnitObjectiveCondition { /*abstract class*/
public:
	CSpecificAlliedUnitObjectiveCondition(int alignment) : CSpecificUnitObjectiveCondition(alignment) {}
};

class CDestroySpecificEnemyUnit: public CSpecificEnemyUnitObjectiveCondition {
public:
	CDestroySpecificEnemyUnit(int alignment) : CSpecificEnemyUnitObjectiveCondition(alignment) {}
	condition_species_type Species() const { return DESTROY_SPECIFIC_ENEMY_UNIT; }
	std::string Description();
};

class CSpecificStructureObjectiveCondition: public CObjectiveCondition { /*abstract class*/
protected:
	AuthoredTargetPosition m_authoredPosition;
	EditorObject *m_pBuilding;
	float m_LastNotedPositionX;
	float m_LastNotedPositionY;
public:
	CSpecificStructureObjectiveCondition(int alignment) : CObjectiveCondition(alignment) { m_pBuilding = 0; m_LastNotedPositionX = m_LastNotedPositionY = 0.0; }
	virtual bool operator==(const CObjectiveCondition &rhs) const;
	virtual bool Read( FitIniFile* missionFile );
	virtual bool Save( FitIniFile* file );
	virtual bool EditDialog();
	virtual std::string InstanceDescription();
	virtual void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*(dynamic_cast<const CSpecificStructureObjectiveCondition *>(pMaster))); }
	virtual bool RefersTo(const EditorObject *pObj) { return (pObj == m_pBuilding); }
	virtual bool NoteThePositionsOfObjectsReferenced();
	virtual bool RestoreObjectPointerReferencesFromNotedPositions();
	// readable-line accessors (additive). BuildingPtr() may be null; deref only when set.
	EditorObject *BuildingPtr() { return m_pBuilding; }
	float NotedX() { return m_LastNotedPositionX; }
	float NotedY() { return m_LastNotedPositionY; }
	std::string ReadableTarget();
};

class CSpecificEnemyStructureObjectiveCondition: public CSpecificStructureObjectiveCondition { /*abstract class*/
public:
	CSpecificEnemyStructureObjectiveCondition(int alignment) : CSpecificStructureObjectiveCondition(alignment) {}
};

class CSpecificAlliedStructureObjectiveCondition: public CSpecificStructureObjectiveCondition { /*abstract class*/
public:
	CSpecificAlliedStructureObjectiveCondition(int alignment) : CSpecificStructureObjectiveCondition(alignment) {}
};

class CDestroySpecificStructure: public CSpecificEnemyStructureObjectiveCondition {
public:
	CDestroySpecificStructure(int alignment) : CSpecificEnemyStructureObjectiveCondition(alignment) {}
	condition_species_type Species() const { return DESTROY_SPECIFIC_STRUCTURE; }
	std::string Description();
};

class CCaptureOrDestroyAllEnemyUnits: public CObjectiveCondition {
	/* The CaptureOrDestroyAllEnemyUnits condition should not set the units to be
	capturable. */
public:
	CCaptureOrDestroyAllEnemyUnits(int alignment) : CObjectiveCondition(alignment) {}
	condition_species_type Species() const { return CAPTURE_OR_DESTROY_ALL_ENEMY_UNITS; }
	std::string Description();
};

class CCaptureOrDestroyNumberOfEnemyUnits: public CNumberOfEnemyUnitsObjectiveCondition {
	/* The CaptureOrDestroyAllEnemyUnits condition should not set the units to be
	capturable. */
public:
	CCaptureOrDestroyNumberOfEnemyUnits(int alignment) : CNumberOfEnemyUnitsObjectiveCondition(alignment) {}
	condition_species_type Species() const { return CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS; }
	std::string Description();
};

class CCaptureOrDestroySpecificEnemyUnit: public CSpecificEnemyUnitObjectiveCondition {
public:
	CCaptureOrDestroySpecificEnemyUnit(int alignment) : CSpecificEnemyUnitObjectiveCondition(alignment) {}
	condition_species_type Species() const { return CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT; }
	std::string Description();
};

class CCaptureOrDestroySpecificStructure: public CSpecificEnemyStructureObjectiveCondition {
public:
	CCaptureOrDestroySpecificStructure(int alignment) : CSpecificEnemyStructureObjectiveCondition(alignment) {}
	condition_species_type Species() const { return CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE; }
	std::string Description();
};

class CDeadOrFledAllEnemyUnits: public CObjectiveCondition {
public:
	CDeadOrFledAllEnemyUnits(int alignment) : CObjectiveCondition(alignment) {}
	condition_species_type Species() const { return DEAD_OR_FLED_ALL_ENEMY_UNITS; }
	std::string Description();
};

class CDeadOrFledNumberOfEnemyUnits: public CNumberOfEnemyUnitsObjectiveCondition {
public:
	CDeadOrFledNumberOfEnemyUnits(int alignment) : CNumberOfEnemyUnitsObjectiveCondition(alignment) {}
	condition_species_type Species() const { return DEAD_OR_FLED_NUMBER_OF_ENEMY_UNITS; }
	std::string Description();
};

class CDeadOrFledSpecificEnemyUnit: public CSpecificEnemyUnitObjectiveCondition {
public:
	CDeadOrFledSpecificEnemyUnit(int alignment) : CSpecificEnemyUnitObjectiveCondition(alignment) {}
	condition_species_type Species() const { return DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT; }
	std::string Description();
};

class CCaptureUnit: public CSpecificEnemyUnitObjectiveCondition {
public:
	CCaptureUnit(int alignment) : CSpecificEnemyUnitObjectiveCondition(alignment) {}
	condition_species_type Species() const { return CAPTURE_UNIT; }
	std::string Description();
};

class CCaptureStructure: public CSpecificEnemyStructureObjectiveCondition {
public:
	CCaptureStructure(int alignment) : CSpecificEnemyStructureObjectiveCondition(alignment) {}
	condition_species_type Species() const { return CAPTURE_STRUCTURE; }
	std::string Description();
};

class CGuardSpecificUnit: public CSpecificAlliedUnitObjectiveCondition {
public:
	CGuardSpecificUnit(int alignment) : CSpecificAlliedUnitObjectiveCondition(alignment) {}
	condition_species_type Species() const { return GUARD_SPECIFIC_UNIT; }
	std::string Description();
};

class CGuardSpecificStructure: public CSpecificAlliedStructureObjectiveCondition {
public:
	CGuardSpecificStructure(int alignment) : CSpecificAlliedStructureObjectiveCondition(alignment) {}
	condition_species_type Species() const { return GUARD_SPECIFIC_STRUCTURE; }
	std::string Description();
};

class CAreaObjectiveCondition: public CObjectiveCondition { /*abstract class*/
private:
	float m_targetCenterX;
	float m_targetCenterY;
	float m_targetRadius;
public:
	CAreaObjectiveCondition(int alignment) : CObjectiveCondition(alignment) { m_targetCenterX = 0.0; m_targetCenterY = 0.0; m_targetRadius = 0.0; }
	virtual bool operator==(const CObjectiveCondition &rhs) const;
	virtual bool Read( FitIniFile* missionFile );
	virtual bool Save( FitIniFile* file );
	virtual bool EditDialog();
	virtual std::string InstanceDescription();
	virtual void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*(dynamic_cast<const CAreaObjectiveCondition *>(pMaster))); }
	// readable-line + picker accessors (additive).
	float TargetCenterX() { return m_targetCenterX; }
	float TargetCenterY() { return m_targetCenterY; }
	float TargetRadius()  { return m_targetRadius; }
	// Slice 2 -- area "Pick center" map-picker setters (additive). TargetAreaDlg
	// writes the picked world XY into the referenced center on pick re-entry;
	// radius is left untouched. Also needed by the templates slice.
	void TargetCenterX(float v) { m_targetCenterX = v; }
	void TargetCenterY(float v) { m_targetCenterY = v; }
	void TargetRadius(float v)  { m_targetRadius  = v; }
};

class CMoveAnyUnitToArea: public CAreaObjectiveCondition {
public:
	CMoveAnyUnitToArea(int alignment) : CAreaObjectiveCondition(alignment) {}
	condition_species_type Species() const { return MOVE_ANY_UNIT_TO_AREA; }
	std::string Description();
};

class CMoveAllUnitsToArea: public CAreaObjectiveCondition {
public:
	CMoveAllUnitsToArea(int alignment) : CAreaObjectiveCondition(alignment) {}
	condition_species_type Species() const { return MOVE_ALL_UNITS_TO_AREA; }
	std::string Description();
};

class CMoveAllSurvivingUnitsToArea: public CAreaObjectiveCondition {
public:
	CMoveAllSurvivingUnitsToArea(int alignment) : CAreaObjectiveCondition(alignment) {}
	condition_species_type Species() const { return MOVE_ALL_SURVIVING_UNITS_TO_AREA; }
	std::string Description();
};

class CMoveAllSurvivingMechsToArea: public CAreaObjectiveCondition {
public:
	CMoveAllSurvivingMechsToArea(int alignment) : CAreaObjectiveCondition(alignment) {}
	condition_species_type Species() const { return MOVE_ALL_SURVIVING_MECHS_TO_AREA; }
	std::string Description();
};

class CBooleanFlagIsSet: public CObjectiveCondition {
protected:
	std::string m_flagID;
	bool m_value;
public:
	CBooleanFlagIsSet(int alignment) : CObjectiveCondition(alignment) { m_flagID = _TEXT("flag0"); m_value = true; }
	bool operator==(const CObjectiveCondition &rhs) const;
	condition_species_type Species() const { return BOOLEAN_FLAG_IS_SET; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	bool EditDialog();
	std::string Description();
	std::string InstanceDescription();
	void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*(dynamic_cast<const CBooleanFlagIsSet *>(pMaster))); }
	const std::string& FlagID() const { return m_flagID; }
	bool FlagValue() { return m_value; }
};

class CElapsedMissionTime: public CObjectiveCondition {
protected:
	float m_time;
public:
	CElapsedMissionTime(int alignment) : CObjectiveCondition(alignment) { m_time = 0.0; }
	bool operator==(const CObjectiveCondition &rhs) const;
	condition_species_type Species() const { return ELAPSED_MISSION_TIME; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	bool EditDialog();
	std::string Description();
	std::string InstanceDescription();
	void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*(dynamic_cast<const CElapsedMissionTime *>(pMaster))); }
	float Time() { return m_time; }   // readable-line accessor (additive)
};

enum action_species_type {
	PLAY_BIK,
	PLAY_WAV,
	DISPLAY_TEXT_MESSAGE,
	DISPLAY_RESOURCE_TEXT_MESSAGE,
	SET_BOOLEAN_FLAG,
	MAKE_NEW_TECHNOLOGY_AVAILABLE,

	NUM_ACTION_SPECIES
};

static const char *g_actionSpeciesStringArray[] = {
	"PlayBIK",
	"PlayWAV",
	"DisplayTextMessage",
	"DisplayResourceTextMessage",
	"SetBooleanFlag",
	"MakeNewTechnologyAvailable",
};

class CObjectiveAction {
private:
	int m_alignment;
public:
	CObjectiveAction(int alignment) { m_alignment = alignment; }
	virtual ~CObjectiveAction() {}
	virtual bool operator==(const CObjectiveAction &rhs) const;
	int Alignment() { return m_alignment; }
	void Alignment(int alignment) { m_alignment = alignment; }
	bool DoCommonEditDialog() {}
	virtual action_species_type Species() const = 0;
	virtual bool Init() = 0;
	virtual bool Read( FitIniFile* missionFile ) = 0;
	virtual bool Save( FitIniFile* file ) = 0;
	virtual bool EditDialog() = 0;
	virtual void WriteAbl() = 0;
	virtual std::string Description() = 0;
	virtual std::string InstanceDescription() { return {}; }
	virtual void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*pMaster); }
};

class CPlayBIK: public CObjectiveAction {
private:
	std::string m_pathname;
public:
	CPlayBIK(int alignment) : CObjectiveAction(alignment) {}
	bool operator==(const CObjectiveAction &rhs) const;
	action_species_type Species() const { return PLAY_BIK; }
	bool Init() { return true; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	bool EditDialog();
	void WriteAbl() {};
	std::string Description();
	std::string InstanceDescription();
	void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*(dynamic_cast<const CPlayBIK *>(pMaster))); }
	const std::string& Pathname() const { return m_pathname; }
};

class CPlayWAV: public CObjectiveAction {
private:
	std::string m_pathname;
public:
	CPlayWAV(int alignment) : CObjectiveAction(alignment) {}
	bool operator==(const CObjectiveAction &rhs) const;
	action_species_type Species() const { return PLAY_WAV; }
	bool Init() { return true; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	bool EditDialog();
	void WriteAbl() {};
	std::string Description();
	std::string InstanceDescription();
	void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*(dynamic_cast<const CPlayWAV *>(pMaster))); }
	const std::string& Pathname() const { return m_pathname; }
};

class CDisplayTextMessage: public CObjectiveAction {
private:
	std::string m_message;
public:
	CDisplayTextMessage(int alignment) : CObjectiveAction(alignment) {}
	bool operator==(const CObjectiveAction &rhs) const;
	action_species_type Species() const { return DISPLAY_TEXT_MESSAGE; }
	bool Init() { return true; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	bool EditDialog();
	void WriteAbl() {};
	std::string Description();
	std::string InstanceDescription();
	void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*(dynamic_cast<const CDisplayTextMessage *>(pMaster))); }
	const std::string& Message() const { return m_message; }
};

class CDisplayResourceTextMessage: public CObjectiveAction {
private:
	int m_resourceStringID;
public:
	CDisplayResourceTextMessage(int alignment) : CObjectiveAction(alignment) {}
	bool operator==(const CObjectiveAction &rhs) const;
	action_species_type Species() const { return DISPLAY_RESOURCE_TEXT_MESSAGE; }
	bool Init() { return true; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	bool EditDialog();
	void WriteAbl() {};
	std::string Description();
	std::string InstanceDescription();
	void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*(dynamic_cast<const CDisplayResourceTextMessage *>(pMaster))); }
	int ResourceStringID() { return m_resourceStringID; }   // readable-line accessor (additive)
};

class CSetBooleanFlag: public CObjectiveAction {
private:
	std::string m_flagID;
	bool m_value;
public:
	CSetBooleanFlag(int alignment) : CObjectiveAction(alignment) { m_flagID = _TEXT("flag0"); m_value = true; }
	bool operator==(const CObjectiveAction &rhs) const;
	action_species_type Species() const { return SET_BOOLEAN_FLAG; }
	bool Init() { return true; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	bool EditDialog();
	void WriteAbl() {};
	std::string Description();
	std::string InstanceDescription();
	void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*(dynamic_cast<const CSetBooleanFlag *>(pMaster))); }
	const std::string& FlagID() const { return m_flagID; }
	bool FlagValue() { return m_value; }
};

class CMakeNewTechnologyAvailable: public CObjectiveAction {
private:
	std::string m_purchaseFilePathname;
public:
	CMakeNewTechnologyAvailable(int alignment) : CObjectiveAction(alignment) {}
	bool operator==(const CObjectiveAction &rhs) const;
	action_species_type Species() const { return MAKE_NEW_TECHNOLOGY_AVAILABLE; }
	bool Init() { return true; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	bool EditDialog();
	void WriteAbl() {};
	std::string Description();
	std::string InstanceDescription();
	void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*(dynamic_cast<const CMakeNewTechnologyAvailable *>(pMaster))); }
	const std::string& PurchaseFile() const { return m_purchaseFilePathname; }
};

class  CObjectiveConditionList {
public:
	std::vector<CObjectiveCondition*> entries;
	CObjectiveConditionList() = default;
	CObjectiveConditionList(const CObjectiveConditionList& master) { *this = master; }
	~CObjectiveConditionList() { Clear(); }
	CObjectiveConditionList &operator=(const CObjectiveConditionList &master);
	bool operator==(const CObjectiveConditionList &rhs) const;
	void Clear();
};

class  CObjectiveActionList {
public:
	std::vector<CObjectiveAction*> entries;
	CObjectiveActionList() = default;
	CObjectiveActionList(const CObjectiveActionList& master) { *this = master; }
	~CObjectiveActionList() { Clear(); }
	CObjectiveActionList &operator=(const CObjectiveActionList &master);
	bool operator==(const CObjectiveActionList &rhs) const;
	void Clear();
};

class CObjective {
private:
	int m_alignment;
	std::string m_title;
	bool m_titleUseResourceString;
	int m_titleResourceStringID;
	std::string m_description;
	bool m_descriptionUseResourceString;
	int m_descriptionResourceStringID;
	int m_priority;
	int m_resourcePoints;
	bool m_previousPrimaryObjectiveMustBeComplete;
	bool m_allPreviousPrimaryObjectivesMustBeComplete;
	bool m_displayMarker;
	float m_markerX;
	float m_markerY;
	bool m_isHiddenTrigger;
	bool m_activateOnFlag;
	std::string m_activateFlagID;
	bool m_resetStatusOnFlag;
	std::string m_resetStatusFlagID;
	long	m_modelID;
	ObjectiveModelDefinition m_modelDefinition;
	long m_baseColor;
	long m_highlightColor;
	long m_highlightColor2;

public: /* we could make this protected if only the editdialog is to acces these functions */
	static CObjectiveCondition *new_CObjectiveCondition(condition_species_type conditionSpecies, int alignment);
	static std::string DescriptionOfConditionSpecies(condition_species_type conditionSpecies);
	static CObjectiveAction *new_CObjectiveAction(action_species_type actionSpecies, int alignment);
	static std::string DescriptionOfActionSpecies(action_species_type actionSpecies);
public:
	typedef CObjectiveConditionList condition_list_type;
	typedef CObjectiveActionList action_list_type;
	condition_list_type m_conditions;
	action_list_type m_actionList;
	condition_list_type m_failureConditionList;
	action_list_type m_failureActionList;

	void Construct(int alignment);
	CObjective() { Construct(0); }
	CObjective(int alignment) { Construct(alignment); }
	CObjective(const CObjective &master) { (*this) = master; }
	~CObjective() { Clear(); }
	CObjective &operator=(const CObjective &master);
	bool operator==(const CObjective &rhs) const;
	void Init() {}
	void Clear();
	int Alignment() { return m_alignment; }
	void Alignment(int alignment);
	bool Read( FitIniFile* missionFile, int objectiveNum, int version = 0);
	bool Save( FitIniFile* file, int objectiveNum);
	bool EditDialog();
	bool NoteThePositionsOfObjectsReferenced();
	bool RestoreObjectPointerReferencesFromNotedPositions();
	bool ThisObjectiveHasNoConditions() { return (m_conditions.entries.empty() && m_failureConditionList.entries.empty()); }
	const std::string& Title() const { return m_title; }
	void Title(std::string title) { m_title = std::move(title); }
	bool TitleUseResourceString() const { return m_titleUseResourceString; }
	void TitleUseResourceString(bool titleUseResourceString) { m_titleUseResourceString = titleUseResourceString; }
	int TitleResourceStringID() const { return m_titleResourceStringID; }
	void TitleResourceStringID(int titleResourceStringID) { m_titleResourceStringID = titleResourceStringID; }
	std::string LocalizedTitle() const;
	const std::string& Description() const { return m_description; }
	void Description(std::string description) { m_description = std::move(description); }
	bool DescriptionUseResourceString() const { return m_descriptionUseResourceString; }
	void DescriptionUseResourceString(bool descriptionUseResourceString) { m_descriptionUseResourceString = descriptionUseResourceString; }
	int DescriptionResourceStringID() const { return m_descriptionResourceStringID; }
	void DescriptionResourceStringID(int descriptionResourceStringID) { m_descriptionResourceStringID = descriptionResourceStringID; }
	std::string LocalizedDescription() const;
	int Priority() { return m_priority; }
	void Priority(int priority) { m_priority = priority; }
	int ResourcePoints() { return m_resourcePoints; }
	void ResourcePoints(int resourcePoints) { m_resourcePoints = resourcePoints; }
	bool PreviousPrimaryObjectiveMustBeComplete() { return m_previousPrimaryObjectiveMustBeComplete; }
	void PreviousPrimaryObjectiveMustBeComplete(bool previousPrimaryObjectiveMustBeComplete) { m_previousPrimaryObjectiveMustBeComplete = previousPrimaryObjectiveMustBeComplete; }
	bool AllPreviousPrimaryObjectivesMustBeComplete() { return m_allPreviousPrimaryObjectivesMustBeComplete; }
	void AllPreviousPrimaryObjectivesMustBeComplete(bool allPreviousPrimaryObjectivesMustBeComplete) { m_allPreviousPrimaryObjectivesMustBeComplete = allPreviousPrimaryObjectivesMustBeComplete; }
	bool DisplayMarker() { return m_displayMarker; }
	void DisplayMarker(bool displayMarker) { m_displayMarker = displayMarker; }
	float MarkerX() { return m_markerX; }
	void MarkerX(float markerX) { m_markerX = markerX; }
	float MarkerY() { return m_markerY; }
	void MarkerY(float markerY) { m_markerY = markerY; }
	void IsHiddenTrigger(bool isHiddenTrigger) { m_isHiddenTrigger = isHiddenTrigger; }
	bool IsHiddenTrigger() { return m_isHiddenTrigger; }
	void ActivateOnFlag(bool activateOnFlag) { m_activateOnFlag = activateOnFlag; }
	bool ActivateOnFlag() { return m_activateOnFlag; }
	void ActivateFlagID(std::string activateFlagId) { m_activateFlagID = std::move(activateFlagId); }
	const std::string& ActivateFlagID() const { return m_activateFlagID; }
	void ResetStatusOnFlag(bool resetStatusOnFlag) { m_resetStatusOnFlag = resetStatusOnFlag; }
	bool ResetStatusOnFlag() { return m_resetStatusOnFlag; }
	void ResetStatusFlagID(std::string resetStatusFlagID) { m_resetStatusFlagID = std::move(resetStatusFlagID); }
	const std::string& ResetStatusFlagID() const { return m_resetStatusFlagID; }
	int BaseColor( ) const { return m_baseColor; }
	void BaseColor( int newColor ) { m_baseColor = newColor; }
	int HighlightColor( ) const { return m_highlightColor; }
	void HighlightColor( int newColor ) { m_highlightColor = newColor; }
	int HighlightColor2( ) const { return m_highlightColor2; }
	void HighlightColor2( int newColor ) { m_highlightColor2 = newColor; }
	long ModelID() const { return m_modelID; }
	void ModelID(int newID);
	bool refreshModelFromCatalog();
	const ObjectiveModelDefinition& ModelDefinition() const { return m_modelDefinition; }
};

class CObjectives {
private:
	int m_alignment;
public:
	std::vector<CObjective*> entries;
	CObjectives() { m_alignment = 0; }
	CObjectives(int alignment) { m_alignment = alignment; }
	CObjectives(const CObjectives &master) { (*this) = master; }
	~CObjectives() { Clear(); }
	CObjectives &operator=(const CObjectives &master);
	bool operator==(const CObjectives &rhs) const;
	void Init();
	void Clear();
	int Alignment() { return m_alignment; }
	void Alignment(int alignment);
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	bool EditDialog();
	bool WriteMissionScript(const char *Name, const char *OutputPath);
	void handleObjectInvalidation(const EditorObject *pObj);
	bool NoteThePositionsOfObjectsReferenced();
	bool RestoreObjectPointerReferencesFromNotedPositions();
	bool ThereAreObjectivesWithNoConditions();
};

class CObjectivesEditState {
public:
	int alignment;
	CObjectives ModifiedObjectives;
	enum objective_function_id_type {
		ADD,
		EDIT
	};
	objective_function_id_type objectiveFunction;
	int nSelectionIndex;

	CObjective ModifiedObjective;
	enum class list_id_type {
		SUCCESS_CONDITION,
		FAILURE_CONDITION
	};
	using enum list_id_type;
	list_id_type listID;
	int nConditionSpeciesSelectionIndex;
	int nActionSpeciesSelectionIndex;
	int nFailureConditionSpeciesSelectionIndex;
	int nFailureActionSpeciesSelectionIndex;

	void *pModifiedUnitPtr;
	void *pModifiedBuildingPtr;

	/* Slice 1 -- Marker XY map-picker (PICKER-SELECT-POINT-RECON-1, Option B).
	pendingPickPoint: set true when the objective dialog hands off for a terrain
	point pick; consumed by EditorInterface::handleLeftButtonDown which writes the
	clicked world XY into pendingPickX/Y and sets pendingPickResultReady so the
	re-opened ObjectiveDlg loads the picked coordinates exactly once. */
	bool  pendingPickPoint;
	bool  pendingPickResultReady;
	float pendingPickX;
	float pendingPickY;

	/* Slice 2 fix (BUG A) -- defer the pick re-open to EditorInterface::update().
	handleLeftButtonDown captures the click on mouse-DOWN while MFC still holds
	SetCapture(); re-entering a nested DoModal from there (the old direct Team()
	call) lost the WM_LBUTTONUP and forced the user to ESC several times. Mirror
	the Locate pattern: handleLeftButtonDown only ARMS this flag (after capturing
	the XY + ReleaseCapture), and update() -- the per-frame point with no modal/
	capture on the stack -- reopens the dialog chain via Team(alignment). Covers
	BOTH the marker pick and the area "Pick center" pick (shared capture block). */
	bool  pendingPickReopen;

	/* Slice 2 -- area "Pick center" map-picker (PICKER-SELECT-POINT-RECON-1).
	The marker pick (Slice 1) and the area-center pick share pendingPickPoint/
	pendingPickResultReady/pendingPickX/Y (only one pick is in flight at a time),
	so a discriminator is required: the re-opened ObjectiveDlg::OnInitDialog marker
	consumer must NOT swallow an area pick result (it would corrupt the objective
	marker and never re-reach TargetAreaDlg), and TargetAreaDlg::OnInitDialog must
	consume ONLY an area pick. Set when arming; cleared back to PICK_NONE when the
	matching consumer reads the result or on cancelled-pick disarm. */
	enum class pick_target_type {
		PICK_NONE,
		PICK_MARKER,
		PICK_AREA
	};
	using enum pick_target_type;
	pick_target_type pendingPickTarget;

	/* Slice 3 -- Locate (PICKER-SELECT-POINT-RECON-1 section 6). Distinct from the
	pick fields above: Locate is a read-only "center camera on + select the target
	of the selected condition" operation, NOT a map pick. The ObjectiveDlg "Locate"
	handler extracts the target world XY (and, when safe, the live EditorObject*)
	from the selected specific-unit / specific-structure condition, sets pendingLocate
	plus the coords/obj, enters ObjectSelectOnlyMode, and EndDialog(IDOK) to unwind
	the modal stack. EditorInterface::update() (the per-frame main-thread tick that
	runs once the modal stack is gone -- NO map click required) consumes pendingLocate:
	unselectAll + (guarded) select(*pendingLocateObj) + eye->setPosition(XY), then
	clears the flag, leaves ObjectSelectOnlyMode, and reopens the objectives dialog
	chain via Team(alignment). pendingLocateObj may be null (stale/deleted target or
	missing appearance) -> the camera still centers on pendingLocateX/Y and the
	select() is skipped. */
	bool          pendingLocate;
	float         pendingLocateX;
	float         pendingLocateY;
	EditorObject *pendingLocateObj;

	CObjectivesEditState() {
		Clear();
	}

	void Clear() {
		alignment = 0;
		ModifiedObjectives.Clear();
		objectiveFunction = ADD;
		nSelectionIndex = -1;
		ModifiedObjective.Clear();
		listID = SUCCESS_CONDITION;
		nConditionSpeciesSelectionIndex = -1;
		nActionSpeciesSelectionIndex = -1;
		nFailureConditionSpeciesSelectionIndex = -1;
		nFailureActionSpeciesSelectionIndex = -1;
		pModifiedUnitPtr = 0;
		pModifiedBuildingPtr = 0;
		pendingPickPoint = false;
		pendingPickResultReady = false;
		pendingPickX = 0.0f;
		pendingPickY = 0.0f;
		pendingPickReopen = false;
		pendingPickTarget = PICK_NONE;
		pendingLocate = false;
		pendingLocateX = 0.0f;
		pendingLocateY = 0.0f;
		pendingLocateObj = 0;
	}
};
#endif  // end of file ( Objective.h )
