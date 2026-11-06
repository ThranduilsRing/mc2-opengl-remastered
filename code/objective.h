#ifndef OBJECTIVE_H
#define OBJECTIVE_H
namespace mc2::render_view { struct HudObjective; }
/*************************************************************************************************\
Objective.h			: Interface for the Objective component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"objective_flags.h"
#include"objective_text.h"
#include"platform_tchar.h"

#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include"mover.h"

#ifndef BLDNG_H
#include"bldng.h"
#endif

#ifndef GAMETACMAP_H
#include"gametacmap.h"
#endif

class FitIniFile;

class aFont;
class Camera;

//*************************************************************************************************

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

enum MARKER_TYPES
{
	NO_MARKER = 0,
	NAV = 1,
	NUMERIC = 2
};

extern const char *g_conditionSpeciesStringArray[];

enum objective_status_type {
	OS_UNDETERMINED = 0,		//Make sure first one is zero just to be safe
	OS_SUCCESSFUL,
	OS_FAILED
};

class CObjectiveCondition {
private:
	int m_alignment;
public:
	CObjectiveCondition(int alignment) { m_alignment = alignment; }
	virtual ~CObjectiveCondition() {}
	int Alignment() { return m_alignment; }
	void Alignment(int alignment) { m_alignment = alignment; }
	virtual condition_species_type Species() = 0;
	virtual bool Init() { return true; }
	virtual bool Read( FitIniFile* missionFile ) { return true; }
	virtual bool Save( FitIniFile* file ) { return true; }
	virtual objective_status_type Status() = 0;
	virtual void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*pMaster); }
	virtual glm::vec3 GetObjectivePosition()		//Used to draw on tacmap
	{
		return glm::vec3(-999999.0f,-999999.0f,-999999.0f);
	}
};

class CDestroyAllEnemyUnits: public CObjectiveCondition {
public:
	CDestroyAllEnemyUnits(int alignment) : CObjectiveCondition(alignment) {}
	condition_species_type Species() { return DESTROY_ALL_ENEMY_UNITS; }
	objective_status_type Status();
};

class CNumberOfUnitsObjectiveCondition: public CObjectiveCondition { /*abstract class*/
protected:
	int m_num;
public:
	CNumberOfUnitsObjectiveCondition(int alignment) : CObjectiveCondition(alignment) { m_num = 0; }
	virtual bool Read( FitIniFile* missionFile );
	virtual bool Save( FitIniFile* file );
	virtual void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*(dynamic_cast<const CNumberOfUnitsObjectiveCondition *>(pMaster))); }
};

class CDestroyNumberOfEnemyUnits: public CNumberOfUnitsObjectiveCondition {
public:
	CDestroyNumberOfEnemyUnits(int alignment) : CNumberOfUnitsObjectiveCondition(alignment) {}
	condition_species_type Species() { return DESTROY_NUMBER_OF_ENEMY_UNITS; }
	objective_status_type Status();
};

class CSpecificUnitObjectiveCondition: public CObjectiveCondition { /*abstract class*/
protected:
	GameObjectWatchID       m_pUnitWID;

public:
	CSpecificUnitObjectiveCondition(int alignment) : CObjectiveCondition(alignment) { m_pUnitWID = 0; }
	virtual void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*(dynamic_cast<const CSpecificUnitObjectiveCondition *>(pMaster))); }
	virtual bool Save( FitIniFile* file );
    GameObjectWatchID UnitWatchID() const { return m_pUnitWID; }
};

class CSpecificEnemyUnitObjectiveCondition: public CSpecificUnitObjectiveCondition { /*abstract class*/
public:
	CSpecificEnemyUnitObjectiveCondition(int alignment) : CSpecificUnitObjectiveCondition(alignment) {}
	bool Read( FitIniFile* missionFile );
};

class CDestroySpecificEnemyUnit: public CSpecificEnemyUnitObjectiveCondition {
public:
	CDestroySpecificEnemyUnit(int alignment) : CSpecificEnemyUnitObjectiveCondition(alignment) {}
	condition_species_type Species() { return DESTROY_SPECIFIC_ENEMY_UNIT; }
	objective_status_type Status();
};

class CSpecificStructureObjectiveCondition: public CObjectiveCondition { /*abstract class*/
protected:
	long m_pBuildingWID;
public:
	CSpecificStructureObjectiveCondition(int alignment) : CObjectiveCondition(alignment) { m_pBuildingWID = 0; }
	virtual bool Read( FitIniFile* missionFile );
	virtual bool Save( FitIniFile* file );
	virtual void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*(dynamic_cast<const CSpecificStructureObjectiveCondition *>(pMaster))); }
	
	virtual glm::vec3 GetObjectivePosition()		//Used to draw on tacmap
	{
		Building *m_pBuilding = static_cast<Building *>(ObjectManager->getByWatchID(m_pBuildingWID));
		if (m_pBuilding)
			return m_pBuilding->getPosition();
			
		return glm::vec3(-999999.0f,-999999.0f,-999999.0f);
	}
};

class CDestroySpecificStructure: public CSpecificStructureObjectiveCondition {
public:
	CDestroySpecificStructure(int alignment) : CSpecificStructureObjectiveCondition(alignment) {}
	condition_species_type Species() { return DESTROY_SPECIFIC_STRUCTURE; }
	objective_status_type Status();
};

class CCaptureOrDestroyAllEnemyUnits: public CObjectiveCondition {
public:
	CCaptureOrDestroyAllEnemyUnits(int alignment) : CObjectiveCondition(alignment) {}
	condition_species_type Species() { return CAPTURE_OR_DESTROY_ALL_ENEMY_UNITS; }
	objective_status_type Status();
};

class CCaptureOrDestroyNumberOfEnemyUnits: public CNumberOfUnitsObjectiveCondition {
public:
	CCaptureOrDestroyNumberOfEnemyUnits(int alignment) : CNumberOfUnitsObjectiveCondition(alignment) {}
	condition_species_type Species() { return CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS; }
	objective_status_type Status();
};

class CCaptureOrDestroySpecificEnemyUnit: public CSpecificEnemyUnitObjectiveCondition {
public:
	CCaptureOrDestroySpecificEnemyUnit(int alignment) : CSpecificEnemyUnitObjectiveCondition(alignment) {}
	condition_species_type Species() { return CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT; }
	bool Read( FitIniFile* missionFile );
	objective_status_type Status();
};

class CCaptureOrDestroySpecificStructure: public CSpecificStructureObjectiveCondition {
public:
	CCaptureOrDestroySpecificStructure(int alignment) : CSpecificStructureObjectiveCondition(alignment) {}
	condition_species_type Species() { return CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE; }
	bool Read( FitIniFile* missionFile );
	objective_status_type Status();
};

class CDeadOrFledAllEnemyUnits: public CObjectiveCondition {
public:
	CDeadOrFledAllEnemyUnits(int alignment) : CObjectiveCondition(alignment) {}
	condition_species_type Species() { return DEAD_OR_FLED_ALL_ENEMY_UNITS; }
	objective_status_type Status();
};

class CDeadOrFledNumberOfEnemyUnits: public CNumberOfUnitsObjectiveCondition {
public:
	CDeadOrFledNumberOfEnemyUnits(int alignment) : CNumberOfUnitsObjectiveCondition(alignment) {}
	condition_species_type Species() { return DEAD_OR_FLED_NUMBER_OF_ENEMY_UNITS; }
	objective_status_type Status();
};

class CDeadOrFledSpecificEnemyUnit: public CSpecificEnemyUnitObjectiveCondition {
public:
	CDeadOrFledSpecificEnemyUnit(int alignment) : CSpecificEnemyUnitObjectiveCondition(alignment) {}
	condition_species_type Species() { return DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT; }
	objective_status_type Status();
};

class CCaptureUnit: public CSpecificEnemyUnitObjectiveCondition {
public:
	CCaptureUnit(int alignment) : CSpecificEnemyUnitObjectiveCondition(alignment) {}
	condition_species_type Species() { return CAPTURE_UNIT; }
	bool Read( FitIniFile* missionFile );
	objective_status_type Status();
};

class CCaptureStructure: public CSpecificStructureObjectiveCondition {
public:
	CCaptureStructure(int alignment) : CSpecificStructureObjectiveCondition(alignment) {}
	condition_species_type Species() { return CAPTURE_STRUCTURE; }
	bool Read( FitIniFile* missionFile );
	objective_status_type Status();
};

class CGuardSpecificUnit: public CSpecificUnitObjectiveCondition {
public:
	CGuardSpecificUnit(int alignment) : CSpecificUnitObjectiveCondition(alignment) {}
	condition_species_type Species() { return GUARD_SPECIFIC_UNIT; }
	bool Read( FitIniFile* missionFile );
	objective_status_type Status();
};

class CGuardSpecificStructure: public CSpecificStructureObjectiveCondition {
public:
	CGuardSpecificStructure(int alignment) : CSpecificStructureObjectiveCondition(alignment) {}
	condition_species_type Species() { return GUARD_SPECIFIC_STRUCTURE; }
	objective_status_type Status();
};

class CAreaObjectiveCondition: public CObjectiveCondition { /*abstract class*/
protected:
	float m_targetCenterX;
	float m_targetCenterY;
	float m_targetRadius;
public:
	CAreaObjectiveCondition(int alignment) : CObjectiveCondition(alignment) { m_targetCenterX = 0.0; m_targetCenterY = 0.0; m_targetRadius = 0.0; }
	virtual bool SetParams(float targetCenterX, float targetCenterY, float targetRadius) {
		m_targetCenterX = targetCenterX; m_targetCenterY = targetCenterY; m_targetRadius = targetRadius; return true;
	}
	virtual bool Read( FitIniFile* missionFile );
	virtual bool Save( FitIniFile* file );
	virtual void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*(dynamic_cast<const CAreaObjectiveCondition *>(pMaster))); }
	
	virtual glm::vec3 GetObjectivePosition()		//Used to draw on tacmap
	{
 		return glm::vec3(m_targetCenterX,m_targetCenterY,0.0f);
	}
};

class CMoveAnyUnitToArea: public CAreaObjectiveCondition {
public:
	CMoveAnyUnitToArea(int alignment) : CAreaObjectiveCondition(alignment) {}
	condition_species_type Species() { return MOVE_ANY_UNIT_TO_AREA; }
	objective_status_type Status();
};

class CMoveAllUnitsToArea: public CAreaObjectiveCondition {
public:
	CMoveAllUnitsToArea(int alignment) : CAreaObjectiveCondition(alignment) {}
	condition_species_type Species() { return MOVE_ALL_UNITS_TO_AREA; }
	objective_status_type Status();
};

class CMoveAllSurvivingUnitsToArea: public CAreaObjectiveCondition {
public:
	CMoveAllSurvivingUnitsToArea(int alignment) : CAreaObjectiveCondition(alignment) {}
	condition_species_type Species() { return MOVE_ALL_SURVIVING_UNITS_TO_AREA; }
	objective_status_type Status();
};

class CMoveAllSurvivingMechsToArea: public CAreaObjectiveCondition {
public:
	CMoveAllSurvivingMechsToArea(int alignment) : CAreaObjectiveCondition(alignment) {}
	condition_species_type Species() { return MOVE_ALL_SURVIVING_MECHS_TO_AREA; }
	objective_status_type Status();
};

class CBooleanFlagIsSet: public CObjectiveCondition {
protected:
	std::string m_flagID;
	bool m_value;
public:
	CBooleanFlagIsSet(int alignment) : CObjectiveCondition(alignment) { m_flagID = "flag0"; m_value = true; }
	condition_species_type Species() { return BOOLEAN_FLAG_IS_SET; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	objective_status_type Status();
	void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*(dynamic_cast<const CBooleanFlagIsSet *>(pMaster))); }
};

class CElapsedMissionTime: public CObjectiveCondition {
protected:
	float m_time;
public:
	CElapsedMissionTime(int alignment) : CObjectiveCondition(alignment) { m_time = 0.0; }
	condition_species_type Species() { return ELAPSED_MISSION_TIME; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	objective_status_type Status();
	void CastAndCopy(const CObjectiveCondition *pMaster) { (*this) = (*(dynamic_cast<const CElapsedMissionTime *>(pMaster))); }
};

enum action_species_type {
	PLAY_BIK,
	PLAY_WAV,
	DISPLAY_TEXT_MESSAGE,
	DISPLAY_RESOURCE_TEXT_MESSAGE,
	SET_BOOLEAN_FLAG,
	MAKE_NEW_TECHNOLOGY_AVAILABLE,
	_REMOVE_STRUCTURE,

	NUM_ACTION_SPECIES
};

class CObjectiveAction {
private:
	int m_alignment;
public:
	int observationOrdinal = -1;
	std::string observationPayload = "null";
	CObjectiveAction(int alignment) { m_alignment = alignment; }
	virtual ~CObjectiveAction() {}
	int Alignment() { return m_alignment; }
	void Alignment(int alignment) { m_alignment = alignment; }
	bool DoCommonEditDialog() { return false; /* sebi: what else can I return here? */}
	virtual action_species_type Species() = 0;
	virtual bool Init() = 0;
	virtual bool Read( FitIniFile* missionFile ) = 0;
	virtual bool Save( FitIniFile* file ) = 0;
	virtual int Execute() = 0;
	virtual void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*pMaster); }
};

class CPlayBIK: public CObjectiveAction {
private:
	std::string m_pathname;
public:
	CPlayBIK(int alignment) : CObjectiveAction(alignment) {}
	action_species_type Species() { return PLAY_BIK; }
	bool Init() { return true; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	int Execute();
	void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*(dynamic_cast<const CPlayBIK *>(pMaster))); }
};

class CPlayWAV: public CObjectiveAction {
private:
	std::string m_pathname;
public:
	CPlayWAV(int alignment) : CObjectiveAction(alignment) {}
	action_species_type Species() { return PLAY_WAV; }
	bool Init() { return true; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	int Execute();
	void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*(dynamic_cast<const CPlayWAV *>(pMaster))); }
};

class CDisplayTextMessage: public CObjectiveAction {
private:
	std::string m_message;
public:
	CDisplayTextMessage(int alignment) : CObjectiveAction(alignment) {}
	action_species_type Species() { return DISPLAY_TEXT_MESSAGE; }
	bool Init() { return true; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	int Execute();
	void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*(dynamic_cast<const CDisplayTextMessage *>(pMaster))); }
};

class CDisplayResourceTextMessage: public CObjectiveAction {
private:
	int m_resourceStringID = 0;
public:
	CDisplayResourceTextMessage(int alignment) : CObjectiveAction(alignment) {}
	action_species_type Species() { return DISPLAY_RESOURCE_TEXT_MESSAGE; }
	bool Init() { return true; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	int Execute();
	void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*(dynamic_cast<const CDisplayResourceTextMessage *>(pMaster))); }
};

class CSetBooleanFlag: public CObjectiveAction {
private:
	std::string m_flagID;
	bool m_value;
public:
	CSetBooleanFlag(int alignment) : CObjectiveAction(alignment) { m_flagID = "flag0"; m_value = true; }
	action_species_type Species() { return SET_BOOLEAN_FLAG; }
	bool Init() { return true; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	int Execute();
	void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*(dynamic_cast<const CSetBooleanFlag *>(pMaster))); }
};

class CMakeNewTechnologyAvailable: public CObjectiveAction {
private:
	std::string m_purchaseFilePathname;
public:
	CMakeNewTechnologyAvailable(int alignment) : CObjectiveAction(alignment) {}
	action_species_type Species() { return MAKE_NEW_TECHNOLOGY_AVAILABLE; }
	bool Init() { return true; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	int Execute();
	void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*(dynamic_cast<const CMakeNewTechnologyAvailable *>(pMaster))); }
};

class C_RemoveStructure: public CObjectiveAction {
private:
	long m_pBuildingWID;
public:
	C_RemoveStructure(int alignment) : CObjectiveAction(alignment) {}
	bool SetParams(float positionX, float positionY);
	action_species_type Species() { return _REMOVE_STRUCTURE; }
	bool Init() { return true; }
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	int Execute();
	void CastAndCopy(const CObjectiveAction *pMaster) { (*this) = (*(dynamic_cast<const C_RemoveStructure *>(pMaster))); }
};

class CObjectives;

// An ordered list that owns its elements, for the parts of an objective. Order is evaluation order and save order; elements are heap objects, so an element's address
// never changes while the list grows. Nothing removes a single element: Clear() frees the whole list head to tail. Iterating yields the plain pointers the call sites
// use; Append() takes ownership and drops null with a log line. The list is not copyable: CObjective and CObjectives copy by building each element through the species factory.
template <class T>
class CObjectiveOwnedList {
	std::vector<std::unique_ptr<T>> m_items;
public:
	class const_iterator {
		typename std::vector<std::unique_ptr<T>>::const_iterator m_it;
	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type = T *;
		using difference_type = std::ptrdiff_t;
		using pointer = T * const *;
		using reference = T *;
		const_iterator() = default;
		explicit const_iterator(typename std::vector<std::unique_ptr<T>>::const_iterator it) : m_it(it) {}
		T *operator*() const { return m_it->get(); }
		const_iterator &operator++() { ++m_it; return *this; }
		const_iterator operator++(int) { const_iterator before(*this); ++m_it; return before; }
		friend bool operator==(const const_iterator &a, const const_iterator &b) { return a.m_it == b.m_it; }
	};
	using iterator = const_iterator;

	CObjectiveOwnedList() = default;
	CObjectiveOwnedList(const CObjectiveOwnedList &) = delete;
	CObjectiveOwnedList &operator=(const CObjectiveOwnedList &) = delete;

	void Append(T *element) { Append(std::unique_ptr<T>(element)); }
	void Append(std::unique_ptr<T> element) {
		if (element) { m_items.push_back(std::move(element)); }
		else { logDroppedObjectivePart(); }
	}
	unsigned long Count() const { return static_cast<unsigned long>(m_items.size()); }
	void Clear() {
		for (auto &element : m_items) { element.reset(); }
		m_items.clear();
	}
	const_iterator begin() const { return const_iterator(m_items.cbegin()); }
	const_iterator end() const { return const_iterator(m_items.cend()); }
};

using CObjectiveConditionList = CObjectiveOwnedList<CObjectiveCondition>;
using CObjectiveActionList = CObjectiveOwnedList<CObjectiveAction>;

class CObjective {
private:
	int m_fitOrdinal = -1;
	unsigned long long m_observationState = 0;
	void ObserveState(std::string_view kind, int oldState, int newState);
	void Dispatch(CObjectiveAction& action, std::string_view list);
	int m_alignment;
	objective_text::Fields m_text;
	bool m_titleUseResourceString;
	int m_titleResourceStringID;
	bool m_descriptionUseResourceString;
	int m_descriptionResourceStringID;
	int m_priority;
	int m_resourcePoints;
	bool m_previousPrimaryObjectiveMustBeComplete;
	bool m_allPreviousPrimaryObjectivesMustBeComplete;
	int m_displayMarker;
	float m_markerX;
	float m_markerY;
	char m_markerText[4];
	bool m_isHiddenTrigger;
	bool m_isActive;
	bool m_activateOnFlag;
	bool m_resetStatusOnFlag;
	double m_activationTime;
	bool m_resolved;
	bool m_changedStatus;
	objective_status_type m_resolvedStatus;
	int m_modelType = 0;
	int m_modelBaseColor = 0;
	int m_modelHighlightColor = 0;
	int m_modelHighlightColor2 = 0;
	float m_modelScale;
	static float s_blinkLength;
	static float s_lastBlinkTime;
	static unsigned long s_blinkColor;
	static std::unique_ptr<aFont>	s_markerFont;
public: /* we could make this protected if only the editdialog is to access these functions */
	static std::unique_ptr<CObjectiveCondition> new_CObjectiveCondition(condition_species_type conditionSpecies, int alignment);
	static std::unique_ptr<CObjectiveAction> new_CObjectiveAction(action_species_type actionSpecies, int alignment);
public:
	using condition_list_type = CObjectiveConditionList;
	using action_list_type = CObjectiveActionList;
	/* The success conditions: every one must hold for the objective to succeed. */
	condition_list_type m_conditionList;
	action_list_type m_actionList;
	condition_list_type m_failureConditionList;
	action_list_type m_failureActionList;

	CObjective(int alignment);
	CObjective(const CObjective &master) { (*this) = master; }
	~CObjective() { Clear(); }
	CObjective &operator=(const CObjective &master);
	void Init() {}
	void Clear();
	int Alignment() { return m_alignment; }
	void Alignment(int alignment);
	bool Read( FitIniFile* missionFile, int objectiveNum, int version, int markerNum, char secondaryMarkerNum );
	bool Save( FitIniFile* file, int objectiveNum );
	/* The following function evaluates the status of the objective irrespective of the other
	objectives (i.e. it disregards qualifiers like "PreviousPrimaryObjectiveMustbeComplete"). */
	objective_status_type Status();
	objective_status_type ReportedStatus() const { return m_resolved ? m_resolvedStatus : OS_UNDETERMINED; }
	/* The following function evaluates the status of the objective in the context of the given
	objectives. */
	objective_status_type Status(CObjectives &objectives);
	objective_status_type observedStatus() const { return m_resolved ? m_resolvedStatus : OS_UNDETERMINED; }
	void Status(objective_status_type newStatus);
	/* The following function evaluates the status of the objective in the context of the given
	objectives. It returns a bool telling me if the objective has failed or been completed since I last checked!*/
	bool StatusChangedSuccess (void);
	bool StatusChangedFailed (void);
	const std::string& Title() const { return m_text.title; }
	void Title(std::string title) { m_text.title = std::move(title); }
	bool TitleUseResourceString() const { return m_titleUseResourceString; }
	void TitleUseResourceString(bool titleUseResourceString) { m_titleUseResourceString = titleUseResourceString; }
	int TitleResourceStringID() const { return m_titleResourceStringID; }
	void TitleResourceStringID(int titleResourceStringID) { m_titleResourceStringID = titleResourceStringID; }
	std::string LocalizedTitle() const;
	const std::string& Description() const { return m_text.description; }
	void Description(std::string description) { m_text.description = std::move(description); }
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
	bool IsResolved() { return m_resolved; }                  // read-only (slice E ImGui tac map)
	const char* MarkerText() const { return m_markerText; }  // read-only (slice E ImGui tac map)
	int DisplayMarker() { return m_displayMarker; }
	void DisplayMarker(int displayMarker) { m_displayMarker = displayMarker; }
	float MarkerX() { return m_markerX; }
	void MarkerX(float markerX) { m_markerX = markerX; }
	float MarkerY() { return m_markerY; }
	void MarkerY(float markerY) { m_markerY = markerY; }
	void IsHiddenTrigger(bool isHiddenTrigger) { m_isHiddenTrigger = isHiddenTrigger; }
	bool IsHiddenTrigger() { return m_isHiddenTrigger; }
	void IsActive(bool isActive);
	void ObserveCBills(int before, int after);
	bool IsActive() { return m_isActive; }
	void ActivateOnFlag(bool activateOnFlag) { m_activateOnFlag = activateOnFlag; }
	bool ActivateOnFlag() { return m_activateOnFlag; }
	void ActivateFlagID(std::string activateFlagId) { m_text.activateFlagID = std::move(activateFlagId); }
	const std::string& ActivateFlagID() const { return m_text.activateFlagID; }
	void ResetStatusOnFlag(bool resetStatusOnFlag) { m_resetStatusOnFlag = resetStatusOnFlag; }
	bool ResetStatusOnFlag() { return m_resetStatusOnFlag; }
	void ResetStatusFlagID(std::string resetStatusFlagID) { m_text.resetStatusFlagID = std::move(resetStatusFlagID); }
	const std::string& ResetStatusFlagID() const { return m_text.resetStatusFlagID; }
	const std::string& ModelName() const { return m_text.modelName; }
	void ModelName(std::string modelName) { m_text.modelName = std::move(modelName); }
	int ModelType() { return m_modelType; }
	void ModelType(int modelType) { m_modelType = modelType; }
	int ModelBaseColor() { return m_modelBaseColor; }
	void ModelBaseColor(int modelBaseColor) { m_modelBaseColor = modelBaseColor; }
	int ModelHighlightColor() { return m_modelHighlightColor; }
	void ModelHighlightColor(int modelHighlightColor) { m_modelHighlightColor = modelHighlightColor; }
	int ModelHighlightColor2() { return m_modelHighlightColor2; }
	void ModelHighlightColor2(int modelHighlightColor2) { m_modelHighlightColor2 = modelHighlightColor2; }
	float ModelScale() { return m_modelScale; }
	void ModelScale(float modelScale) { m_modelScale = modelScale; }
	void ActivationTime(double activationTime) { m_activationTime = activationTime; }
	double ActivationTime() { return m_activationTime; }
	void Render( unsigned long xPos, unsigned long yPos, HGOSFONT3D );
	static bool RenderMarkers(const mc2::render_view::HudObjective& row, GameTacMap* tacMap, bool blink);

	// Tactical Overview: draw this objective's marker text projected over its
	// world location on the main 3D screen (like the tac map, but in the world).
	static void RenderOverviewMarker(const mc2::render_view::HudObjective& row, Camera* eye, float alpha);

	friend class Mission;
	friend struct ObjectiveObservationFixture;
};

class CObjectives {
	CObjectiveOwnedList<CObjective> m_list;
public:
	struct Observation {
		struct Sink { unsigned long long cause; std::string data; };
		struct Technology { const char* path; unsigned long long dispatch; };
		std::string missionHash;
		unsigned long long sequence = 0, nextId = 1, dispatch = 0;
		unsigned long long traversal = 0, nextTraversal = 1;
		const char* traversalSource = "write";
		// Sink evidence requires its completed dispatch as an earlier cause.
		std::vector<Sink> pendingSinks;
		std::vector<Technology> technologies;
		int finalResult = -1;
		bool completed = false, rejected = false, settled = false;
		void Begin(FitIniFile& file);
		unsigned long long Emit(std::string_view kind, unsigned long long cause, std::string_view data,
		                        unsigned long long id = 0);
		void SinkResult(std::string_view sink, std::string_view stage, std::string_view result, std::string_view payload,
		                unsigned long long cause = 0);
		void Finish();
	} observation;
	using const_iterator = CObjectiveOwnedList<CObjective>::const_iterator;
	const_iterator begin() const { return m_list.begin(); }
	const_iterator end() const { return m_list.end(); }
	unsigned long Count() const { return m_list.Count(); }
	void Append(CObjective *objective) { m_list.Append(objective); }
	void Append(std::unique_ptr<CObjective> objective) { m_list.Append(std::move(objective)); }
	CObjectives(int alignment = 0) { m_alignment = alignment; }
	CObjectives(const CObjectives &master) { (*this) = master; }
	~CObjectives() { Clear(); }
	CObjectives &operator=(const CObjectives &master);
	void Init();
	void Clear();
	int Alignment() { return m_alignment; }
	void Alignment(int alignment);
	bool Read( FitIniFile* missionFile );
	bool Save( FitIniFile* file );
	objective_status_type Status();
	CBooleanArray boolFlags;
private:
	int m_alignment;
};

bool objectiveObservationEnabled();
std::string objectiveObservationQuote(std::string_view text);
CObjectives::Observation* objectiveObservation();
// payload may be null (an empty payload); other text arguments are present.
void objectiveObservationSink(std::string_view sink, std::string_view stage, std::string_view result, mc2::czstring payload);
void objectiveObservationWav(mc2::czstring requested, mc2::czstring resource, int channel, mc2::czstring result, bool accepted);

/* Reads Nav Marker info and adds appropriate "hidden trigger" objectives. */
bool ReadNavMarkers( FitIniFile* missionFile, CObjectives &objectives );

//*************************************************************************************************
#endif  // end of file ( Objective.h )
