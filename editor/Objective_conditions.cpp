// Objective: objective condition classes: compare, read/save, edit dialogs and descriptions.
// Declared in the class headers; shared names in Objective_internal.h.
#include "stdafx.h"
#include "Objective_internal.h"
#include "resource.h"
#include "ChooseUnitDlg.h"
#include "ChooseBuildingDlg.h"
#include "SingleValueDlg.h"
#include "BooleanFlagIsSetDialog.h"
#include "TargetAreaDlg.h"
#include "EditorObjectMgr.h"
#include "EditorObjects.h"
#include "MCLibResource.h"
#include "../resource.h"
#include "mc2fmt.h"
#include <cassert>
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace
{
bool areCloseEnough(float f1, float f2)
{
	const float diff = std::fabs(f1 - f2);
	const float bigger = std::max(std::fabs(f1), std::fabs(f2));
	const float proportion = bigger != 0.0f ? diff / bigger : 0.0f;
	return std::pow(10.0, -FLT_DIG) * 10.0 > proportion;
}
}


bool CObjectiveCondition::operator==(const CObjectiveCondition &rhs) const {
	bool retval = false;
	if ((Species() == rhs.Species()) && (m_alignment == rhs.m_alignment)) {
		retval = true;
	}
	return retval;
}

std::string CDestroyAllEnemyUnits::Description() {
	std::string retval = "DestroyAllEnemyUnits"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_DestroyAllEnemyUnits, retval);
	return retval;
}

bool CNumberOfEnemyUnitsObjectiveCondition::operator==(const CObjectiveCondition &rhs) const {
	bool retval = false;
	const CNumberOfEnemyUnitsObjectiveCondition *pCastedRhs = dynamic_cast<const CNumberOfEnemyUnitsObjectiveCondition *>(&rhs);
	if ((pCastedRhs) && (CObjectiveCondition::operator==(rhs))) {
		if (m_num == pCastedRhs->m_num) {
			retval = true;
		}
	}
	return retval;
}

bool CNumberOfEnemyUnitsObjectiveCondition::Read( FitIniFile* missionFile )
{
	long count = 0;
	if (!EditorFit::longValue(*missionFile, "Num", count, true)) return false;
	m_num = count;
	return true;
}

bool CNumberOfEnemyUnitsObjectiveCondition::Save( FitIniFile* file )
{
	file->writeIdULong( "Num", m_num );
	return true;
}

bool CNumberOfEnemyUnitsObjectiveCondition::EditDialog() {
	SingleValueDlg NumDialog( IDS_NUM_ENEMY_UNITS, IDS_BLANK_SPACE);
	NumDialog.SetVal(m_num);
	int ret = NumDialog.DoModal();
	if (IDOK == ret) {
		m_num = NumDialog.GetVal();
	}
	return (IDOK == ret);
}

std::string CNumberOfEnemyUnitsObjectiveCondition::InstanceDescription() {
	return std::format("{}", m_num);
}

std::string CDestroyNumberOfEnemyUnits::Description() {
	std::string retval = "DestroyNumberOfEnemyUnits"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_DestroyNumberOfEnemyUnits, retval);
	return retval;
}

bool CSpecificUnitObjectiveCondition::operator==(const CObjectiveCondition &rhs) const {
	bool retval = false;
	const CSpecificUnitObjectiveCondition *pCastedRhs = dynamic_cast<const CSpecificUnitObjectiveCondition *>(&rhs);
	if ((pCastedRhs) && (CObjectiveCondition::operator==(rhs))) {
		if (m_pUnit == pCastedRhs->m_pUnit) {
			retval = true;
		}
	}
	return retval;
}

bool CSpecificUnitObjectiveCondition::Read( FitIniFile* missionFile )
{
	float positionX = 0, positionY = 0;
	if (!EditorFit::floatValue(*missionFile, "PositionX", positionX)
		|| !EditorFit::floatValue(*missionFile, "PositionY", positionY))
		return EditorFit::reject("objective unit coordinates");
	if (auto* reader = dynamic_cast<EditorFit::MissionTextReader*>(missionFile); reader && reader->deferObjectReferences)
	{
		m_pUnit = nullptr;
		m_LastNotedPositionX = positionX;
		m_LastNotedPositionY = positionY;
		m_authoredPosition.read(positionX, positionY, positionX, positionY);
		return true;
	}
	EditorObjectMgr::UNIT_LIST units = EditorObjectMgr::instance()->getUnits();
	auto it = units.begin();
	while (it != units.end()) {
		if (areCloseEnough((*it)->getPosition().x, positionX)  && areCloseEnough((*it)->getPosition().y, positionY)) {
			break;
		}
		it++;
	}
	if (it == units.end()) return EditorFit::reject("objective unit reference");
	m_pUnit = (*it);
	m_authoredPosition.read(positionX, positionY, m_pUnit->getPosition().x, m_pUnit->getPosition().y);
	if (m_pUnit) {
		m_LastNotedPositionX = m_pUnit->getPosition().x;
		m_LastNotedPositionY = m_pUnit->getPosition().y;
	}

	return true;
}

bool CSpecificUnitObjectiveCondition::Save( FitIniFile* file )
{
	if (!m_pUnit) return EditorFit::reject("objective unit reference");

	file->writeIdFloat( "PositionX", m_authoredPosition.writeX(m_pUnit->getPosition().x) );
	file->writeIdFloat( "PositionY", m_authoredPosition.writeY(m_pUnit->getPosition().y) );

	long cellx, celly;
	m_pUnit->getCells(cellx, celly);
	file->writeIdULong( "CellX", cellx );
	file->writeIdULong( "CellY", celly );

	int alignment = m_pUnit->getAlignment();
	file->writeIdULong( "Commander", alignment );

	int newLance;
	int index;
	m_pUnit->getLanceInfo(newLance, index);
	file->writeIdULong( "Group", newLance );
	file->writeIdULong( "Mate", index );
	return true;
}

bool CSpecificUnitObjectiveCondition::EditDialog() {
	const Unit* previous = m_pUnit;
	ChooseUnitDlg ChooseUnitDialog(m_pUnit);
	int ret = ChooseUnitDialog.DoModal();
	if (IDOK == ret && previous != m_pUnit) m_authoredPosition = {};
	if (m_pUnit) {
		m_LastNotedPositionX = m_pUnit->getPosition().x;
		m_LastNotedPositionY = m_pUnit->getPosition().y;
	}
	return (IDOK == ret);
}

std::string CSpecificUnitObjectiveCondition::InstanceDescription() {
	std::string tmpEStr;
	Stuff::Vector3D pos = m_pUnit->getPosition();
	const char *szDisplayName = m_pUnit->getDisplayName(); // nb: localization
	assert(szDisplayName);
	tmpEStr = std::format("(pos: {:.3f}, {:.3f}) {}", pos.x, pos.y, mc2::fmt::safe(szDisplayName));
	return tmpEStr;
}

std::string CSpecificUnitObjectiveCondition::ReadableTarget() {
	std::string s;
	if (m_pUnit) {
		Stuff::Vector3D pos = m_pUnit->getPosition();
		const char *name = m_pUnit->getDisplayName();
		s = std::format("{}  ({:.0f}, {:.0f})", (name && name[0]) ? name : "unit", pos.x, pos.y);
	} else {
		s = std::format("unresolved unit  ({:.0f}, {:.0f})", m_LastNotedPositionX, m_LastNotedPositionY);
	}
	return s;
}

bool CSpecificUnitObjectiveCondition::NoteThePositionsOfObjectsReferenced() {
	bool retval = false;
	if (m_pUnit) {
		m_LastNotedPositionX = m_pUnit->getPosition().x;
		m_LastNotedPositionY = m_pUnit->getPosition().y;
		retval = true;
	}
	return retval;
}

bool CSpecificUnitObjectiveCondition::RestoreObjectPointerReferencesFromNotedPositions() {
	EditorObjectMgr::UNIT_LIST units = EditorObjectMgr::instance()->getUnits();
	auto it = units.begin();
	while (it != units.end()) {
		if (areCloseEnough((*it)->getPosition().x, m_LastNotedPositionX)  && areCloseEnough((*it)->getPosition().y, m_LastNotedPositionY)) {
			break;
		}
		it++;
	}
	if (it == units.end()) return EditorFit::reject("objective unit reference");
	if (!m_pUnit)
		m_authoredPosition.read(m_LastNotedPositionX, m_LastNotedPositionY, (*it)->getPosition().x, (*it)->getPosition().y);
	m_pUnit = (*it);
	return true;
}

bool CSpecificEnemyUnitObjectiveCondition::EditDialog() {
	const Unit* previous = m_pUnit;
	ChooseUnitDlg ChooseUnitDialog(m_pUnit, Alignment());
	int ret = ChooseUnitDialog.DoModal();
	if (IDOK == ret && previous != m_pUnit) m_authoredPosition = {};
	if (m_pUnit) {
		m_LastNotedPositionX = m_pUnit->getPosition().x;
		m_LastNotedPositionY = m_pUnit->getPosition().y;
	}
	return (IDOK == ret);
}

std::string CDestroySpecificEnemyUnit::Description() {
	std::string retval = "DestroySpecificEnemyUnit"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_DestroySpecificEnemyUnit, retval);
	return retval;
}

bool CSpecificStructureObjectiveCondition::operator==(const CObjectiveCondition &rhs) const {
	bool retval = false;
	const CSpecificStructureObjectiveCondition *pCastedRhs = dynamic_cast<const CSpecificStructureObjectiveCondition *>(&rhs);
	if ((pCastedRhs) && (CObjectiveCondition::operator==(rhs))) {
		if (m_pBuilding == pCastedRhs->m_pBuilding) {
			retval = true;
		}
	}
	return retval;
}

bool CSpecificStructureObjectiveCondition::Read( FitIniFile* missionFile )
{
	float positionX = 0, positionY = 0;
	if (!EditorFit::floatValue(*missionFile, "PositionX", positionX)
		|| !EditorFit::floatValue(*missionFile, "PositionY", positionY))
		return EditorFit::reject("objective structure coordinates");
	if (auto* reader = dynamic_cast<EditorFit::MissionTextReader*>(missionFile); reader && reader->deferObjectReferences)
	{
		m_pBuilding = nullptr;
		m_LastNotedPositionX = positionX;
		m_LastNotedPositionY = positionY;
		m_authoredPosition.read(positionX, positionY, positionX, positionY);
		return true;
	}
	EditorObjectMgr::BUILDING_LIST buildings = EditorObjectMgr::instance()->getBuildings();
	auto it = buildings.begin();
	while (it != buildings.end()) {
		if (areCloseEnough((*it)->getPosition().x, positionX)  && areCloseEnough((*it)->getPosition().y, positionY)) {
			break;
		}
		it++;
	}
	if (it == buildings.end()) return EditorFit::reject("objective structure reference");
	m_pBuilding = (*it);
	m_authoredPosition.read(positionX, positionY, m_pBuilding->getPosition().x, m_pBuilding->getPosition().y);
	if (m_pBuilding) {
		m_LastNotedPositionX = m_pBuilding->getPosition().x;
		m_LastNotedPositionY = m_pBuilding->getPosition().y;
	}

	return true;
}

bool CSpecificStructureObjectiveCondition::Save( FitIniFile* file )
{
	if (!m_pBuilding) return EditorFit::reject("objective structure reference");

	file->writeIdFloat( "PositionX", m_authoredPosition.writeX(m_pBuilding->getPosition().x) );
	file->writeIdFloat( "PositionY", m_authoredPosition.writeY(m_pBuilding->getPosition().y) );

	long cellx, celly;
	m_pBuilding->getCells(cellx, celly);
	file->writeIdULong( "CellX", cellx );
	file->writeIdULong( "CellY", celly );

	int alignment = m_pBuilding->getAlignment();
	file->writeIdULong( "Commander", alignment );
	return true;
}

bool CSpecificStructureObjectiveCondition::EditDialog() {
	const EditorObject* previous = m_pBuilding;
	ChooseBuildingDlg ChooseBuildingDlg(m_pBuilding);
	int ret = ChooseBuildingDlg.DoModal();
	if (IDOK == ret && previous != m_pBuilding) m_authoredPosition = {};
	if (m_pBuilding) {
		m_LastNotedPositionX = m_pBuilding->getPosition().x;
		m_LastNotedPositionY = m_pBuilding->getPosition().y;
	}
	return (IDOK == ret);
}

std::string CSpecificStructureObjectiveCondition::InstanceDescription() {
	std::string tmpEStr;
	Stuff::Vector3D pos = m_pBuilding->getPosition();
	const char *szDisplayName = m_pBuilding->getDisplayName(); // nb: localization
	assert(szDisplayName);
	tmpEStr = std::format("(pos: {:.3f}, {:.3f}) {}", pos.x, pos.y, mc2::fmt::safe(szDisplayName));
	return tmpEStr;
}

std::string CSpecificStructureObjectiveCondition::ReadableTarget() {
	std::string s;
	if (m_pBuilding) {
		Stuff::Vector3D pos = m_pBuilding->getPosition();
		const char *name = m_pBuilding->getDisplayName();
		s = std::format("{}  ({:.0f}, {:.0f})", (name && name[0]) ? name : "structure", pos.x, pos.y);
	} else {
		s = std::format("unresolved structure  ({:.0f}, {:.0f})", m_LastNotedPositionX, m_LastNotedPositionY);
	}
	return s;
}

bool CSpecificStructureObjectiveCondition::NoteThePositionsOfObjectsReferenced() {
	bool retval = false;
	if (m_pBuilding) {
		m_LastNotedPositionX = m_pBuilding->getPosition().x;
		m_LastNotedPositionY = m_pBuilding->getPosition().y;
		retval = true;
	}
	return retval;
}

bool CSpecificStructureObjectiveCondition::RestoreObjectPointerReferencesFromNotedPositions() {
	EditorObjectMgr::BUILDING_LIST buildings = EditorObjectMgr::instance()->getBuildings();
	auto it = buildings.begin();
	while (it != buildings.end()) {
		if (areCloseEnough((*it)->getPosition().x, m_LastNotedPositionX)  && areCloseEnough((*it)->getPosition().y, m_LastNotedPositionY)) {
			break;
		}
		it++;
	}
	if (it == buildings.end()) return EditorFit::reject("objective structure reference");
	if (!m_pBuilding)
		m_authoredPosition.read(m_LastNotedPositionX, m_LastNotedPositionY, (*it)->getPosition().x, (*it)->getPosition().y);
	m_pBuilding = (*it);
	return true;
}

std::string CDestroySpecificStructure::Description() {
	std::string retval = "DestroySpecificStructure"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_DestroySpecificStructure, retval);
	return retval;
}

std::string CCaptureOrDestroyAllEnemyUnits::Description() {
	std::string retval = "CaptureOrDestroyAllEnemyUnits"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_CaptureOrDestroyAllEnemyUnits, retval);
	return retval;
}

std::string CCaptureOrDestroyNumberOfEnemyUnits::Description() {
	std::string retval = "CaptureOrDestroyNumberOfEnemyUnits"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_CaptureOrDestroyNumberOfEnemyUnits, retval);
	return retval;
}

std::string CCaptureOrDestroySpecificEnemyUnit::Description() {
	std::string retval = "CaptureOrDestroySpecificEnemyUnit"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_CaptureOrDestroySpecificEnemyUnit, retval);
	return retval;
}

std::string CCaptureOrDestroySpecificStructure::Description() {
	std::string retval = "CaptureOrDestroySpecificStructure"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_CaptureOrDestroySpecificStructure, retval);
	return retval;
}

std::string CDeadOrFledAllEnemyUnits::Description() {
	std::string retval = "DeadOrFledAllEnemyUnits"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_DeadOrFledAllEnemyUnits, retval);
	return retval;
}

std::string CDeadOrFledNumberOfEnemyUnits::Description() {
	std::string retval = "DeadOrFledNumberOfEnemyUnits"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_DeadOrFledNumberOfEnemyUnits, retval);
	return retval;
}

std::string CDeadOrFledSpecificEnemyUnit::Description() {
	std::string retval = "DeadOrFledSpecificEnemyUnit"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_DeadOrFledSpecificEnemyUnit, retval);
	return retval;
}

std::string CCaptureUnit::Description() {
	std::string retval = "CaptureSpecificUnit"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_CaptureSpecificUnit, retval);
	return retval;
}

std::string CCaptureStructure::Description() {
	std::string retval = "CaptureSpecificStructure"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_CaptureSpecificStructure, retval);
	return retval;
}

std::string CGuardSpecificUnit::Description() {
	std::string retval = "GuardSpecificUnit"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_GuardSpecificUnit, retval);
	return retval;
}

std::string CGuardSpecificStructure::Description() {
	std::string retval = "GuardSpecificStructure"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_GuardSpecificStructure, retval);
	return retval;
}

bool CAreaObjectiveCondition::operator==(const CObjectiveCondition &rhs) const {
	bool retval = false;
	const CAreaObjectiveCondition *pCastedRhs = dynamic_cast<const CAreaObjectiveCondition *>(&rhs);
	if ((pCastedRhs) && (CObjectiveCondition::operator==(rhs))) {
		if ((m_targetCenterX == pCastedRhs->m_targetCenterX) &&
			(m_targetCenterY == pCastedRhs->m_targetCenterY) &&
			(m_targetRadius == pCastedRhs->m_targetRadius)) {
			retval = true;
		}
	}
	return retval;
}

bool CAreaObjectiveCondition::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = sReadIdFloat(missionFile, "TargetCenterX", m_targetCenterX);
	if (NO_ERR != result) { return false; }
	result = sReadIdFloat(missionFile, "TargetCenterY", m_targetCenterY);
	if (NO_ERR != result) { return false; }
	result = sReadIdFloat(missionFile, "TargetRadius", m_targetRadius);
	if (NO_ERR != result) { return false; }
	return true;
}

bool CAreaObjectiveCondition::Save( FitIniFile* file )
{
	file->writeIdFloat( "TargetCenterX", m_targetCenterX );
	file->writeIdFloat( "TargetCenterY", m_targetCenterY );
	file->writeIdFloat( "TargetRadius", m_targetRadius );
	return true;
}

bool CAreaObjectiveCondition::EditDialog() {
	TargetAreaDlg TargetAreaDialog(m_targetCenterX, m_targetCenterY, m_targetRadius);
	int ret = TargetAreaDialog.DoModal();
	return (IDOK == ret);
}

std::string CAreaObjectiveCondition::InstanceDescription() {
	std::string tmpEStr;
	tmpEStr = std::format("x: {:.3f}, y: {:.3f}, radius: {:.3f}", m_targetCenterX, m_targetCenterY, m_targetRadius);
	return tmpEStr;
}

std::string CMoveAnyUnitToArea::Description() {
	std::string retval = "MoveAnyUnitToArea"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_MoveAnyUnitToArea, retval);
	return retval;
}

std::string CMoveAllUnitsToArea::Description() {
	std::string retval = "MoveAllUnitsToArea"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_MoveAllUnitsToArea, retval);
	return retval;
}

std::string CMoveAllSurvivingUnitsToArea::Description() {
	std::string retval = "MoveAllSurvivingUnitsToArea"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_MoveAllSurvivingUnitsToArea, retval);
	return retval;
}

std::string CMoveAllSurvivingMechsToArea::Description() {
	std::string retval = "MoveAllSurvivingMechsToArea"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_MoveAllSurvivingMechsToArea, retval);
	return retval;
}

bool CBooleanFlagIsSet::operator==(const CObjectiveCondition &rhs) const {
	bool retval = false;
	const CBooleanFlagIsSet *pCastedRhs = dynamic_cast<const CBooleanFlagIsSet *>(&rhs);
	if ((pCastedRhs) && (CObjectiveCondition::operator==(rhs))) {
		if ((mc2::str::iequals(m_flagID, pCastedRhs->m_flagID)) &&
			(m_value == pCastedRhs->m_value)) {
			retval = true;
		}
	}
	return retval;
}

bool CBooleanFlagIsSet::Read( FitIniFile* missionFile )
{
	long result = 0;
	if (auto flagID = EditorFit::flagID(*missionFile, "FlagID", "FlagIndex")) m_flagID = std::move(*flagID);
	result = sReadIdBoolean(missionFile, "Value", m_value);
	return true;
}

bool CBooleanFlagIsSet::Save( FitIniFile* file )
{
	file->writeIdString("FlagID", m_flagID.c_str());
	file->writeIdBoolean("Value", m_value);
	return true;
}

bool CBooleanFlagIsSet::EditDialog() {
	CBooleanFlagIsSetDialog BooleanFlagIsSetDlg;
	BooleanFlagIsSetDlg.m_FlagID = m_flagID.c_str();
	BooleanFlagIsSetDlg.m_Value = 1;
	if (false == m_value) { BooleanFlagIsSetDlg.m_Value = 0; }
	if (IDOK == BooleanFlagIsSetDlg.DoModal()) {
		m_flagID = mc2::str::from_c(BooleanFlagIsSetDlg.m_FlagID.GetBuffer(0));
		m_value = true;
		if (0 == BooleanFlagIsSetDlg.m_Value) { m_value = false; }
		return true;
	} else {
		return false;
	}
}

std::string CBooleanFlagIsSet::Description() {
	std::string retval = "BooleanFlagIsSet"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_BooleanFlagIsSet, retval);
	return retval;
}

std::string CBooleanFlagIsSet::InstanceDescription() {
	std::string tmpEStr;
	tmpEStr = std::format("flag name: {}, value: {}", m_flagID, mc2::fmt::as_i32(m_value));
	return tmpEStr;
}

bool CElapsedMissionTime::operator==(const CObjectiveCondition &rhs) const {
	bool retval = false;
	const CElapsedMissionTime *pCastedRhs = dynamic_cast<const CElapsedMissionTime *>(&rhs);
	if ((pCastedRhs) && (CObjectiveCondition::operator==(rhs))) {
		if (m_time == pCastedRhs->m_time) {
			retval = true;
		}
	}
	return retval;
}

bool CElapsedMissionTime::Read( FitIniFile* missionFile )
{
	long result = 0;
	result = sReadIdFloat(missionFile, "Time", m_time);
	if (NO_ERR != result) { return false; }
	return true;
}

bool CElapsedMissionTime::Save( FitIniFile* file )
{
	file->writeIdFloat("Time", m_time);
	return true;
}

bool CElapsedMissionTime::EditDialog() {
	SingleValueDlg ElapsedMissionTimeDialog( IDS_ELAPSED_TIME, IDS_IN_SECONDS);
	ElapsedMissionTimeDialog.SetVal((int)m_time);
	if( ElapsedMissionTimeDialog.DoModal()==IDOK ) {
		m_time = (float)ElapsedMissionTimeDialog.GetVal();
		return true;
	} else {
		return false;
	}
}

std::string CElapsedMissionTime::Description() {
	std::string retval = "ElapsedMissionTime"; /* needs to be put somewhere localizable */
	LoadObjectiveString(IDS_ElapsedMissionTime, retval);
	return retval;
}

std::string CElapsedMissionTime::InstanceDescription() {
	std::string tmpEStr;
	tmpEStr = std::format("{:.6f} seconds", m_time);
	return tmpEStr;
}
