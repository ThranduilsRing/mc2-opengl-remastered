// Objective: objective condition classes: compare, read/save, edit dialogs and descriptions.
// Declared in the class headers; shared names in Objective_internal.h.
#include "Objective_internal.h"


bool CObjectiveCondition::operator==(const CObjectiveCondition &rhs) const {
	bool retval = false;
	if ((Species() == rhs.Species()) && (m_alignment == rhs.m_alignment)) {
		retval = true;
	}
	return retval;
}

EString CDestroyAllEnemyUnits::Description() {
	EString retval = "DestroyAllEnemyUnits"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_DestroyAllEnemyUnits, retval);
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
	long result = 0;
	result = sReadIdWholeNum(missionFile, "Num", m_num);
	if (NO_ERR != result) { return false; }

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

EString CNumberOfEnemyUnitsObjectiveCondition::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("%d", m_num);
	return tmpEStr;
}

EString CDestroyNumberOfEnemyUnits::Description() {
	EString retval = "DestroyNumberOfEnemyUnits"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_DestroyNumberOfEnemyUnits, retval);
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
	long result = 0;

	float positionX, positionY;
	result = sReadIdFloat(missionFile, "PositionX", positionX);
	if (NO_ERR != result) { return false; }
	result = sReadIdFloat(missionFile, "PositionY", positionY);
	if (NO_ERR != result) { return false; }
	EditorObjectMgr::UNIT_LIST units = EditorObjectMgr::instance()->getUnits();
	EditorObjectMgr::UNIT_LIST::EConstIterator it = units.Begin();
	while (!it.IsDone()) {
		if (areCloseEnough((*it)->getPosition().x, positionX)  && areCloseEnough((*it)->getPosition().y, positionY)) {
			break;
		}
		it++;
	}
	if (it.IsDone()) { assert(false); return false; }
	m_pUnit = (*it);
	if (m_pUnit) {
		m_LastNotedPositionX = m_pUnit->getPosition().x;
		m_LastNotedPositionY = m_pUnit->getPosition().y;
	}

	return true;
}

bool CSpecificUnitObjectiveCondition::Save( FitIniFile* file )
{
	assert(m_pUnit);	// todo: exception handling

	file->writeIdFloat( "PositionX", m_pUnit->getPosition().x  );
	file->writeIdFloat( "PositionY", m_pUnit->getPosition().y );

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
	ChooseUnitDlg ChooseUnitDialog(m_pUnit);
	int ret = ChooseUnitDialog.DoModal();
	if (m_pUnit) {
		m_LastNotedPositionX = m_pUnit->getPosition().x;
		m_LastNotedPositionY = m_pUnit->getPosition().y;
	}
	return (IDOK == ret);
}

EString CSpecificUnitObjectiveCondition::InstanceDescription() {
	EString tmpEStr;
	Stuff::Vector3D pos = m_pUnit->getPosition();
	const char *szDisplayName = m_pUnit->getDisplayName(); // nb: localization
	assert(szDisplayName);
	tmpEStr.Format("(pos: %.3f, %.3f) %s", pos.x, pos.y, szDisplayName);
	return tmpEStr;
}

EString CSpecificUnitObjectiveCondition::ReadableTarget() {
	EString s;
	if (m_pUnit) {
		Stuff::Vector3D pos = m_pUnit->getPosition();
		const char *name = m_pUnit->getDisplayName();
		s.Format("%s  (%.0f, %.0f)", (name && name[0]) ? name : "unit", pos.x, pos.y);
	} else {
		s.Format("unresolved unit  (%.0f, %.0f)", m_LastNotedPositionX, m_LastNotedPositionY);
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
	EditorObjectMgr::UNIT_LIST::EConstIterator it = units.Begin();
	while (!it.IsDone()) {
		if (areCloseEnough((*it)->getPosition().x, m_LastNotedPositionX)  && areCloseEnough((*it)->getPosition().y, m_LastNotedPositionY)) {
			break;
		}
		it++;
	}
	if (it.IsDone()) { assert(false); return false; }
	m_pUnit = (*it);
	return true;
}

bool CSpecificEnemyUnitObjectiveCondition::EditDialog() {
	ChooseUnitDlg ChooseUnitDialog(m_pUnit, Alignment());
	int ret = ChooseUnitDialog.DoModal();
	if (m_pUnit) {
		m_LastNotedPositionX = m_pUnit->getPosition().x;
		m_LastNotedPositionY = m_pUnit->getPosition().y;
	}
	return (IDOK == ret);
}

EString CDestroySpecificEnemyUnit::Description() {
	EString retval = "DestroySpecificEnemyUnit"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_DestroySpecificEnemyUnit, retval);
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
	long result = 0;

	float positionX, positionY;
	result = sReadIdFloat(missionFile, "PositionX", positionX);
	if (NO_ERR != result) { return false; }
	result = sReadIdFloat(missionFile, "PositionY", positionY);
	if (NO_ERR != result) { return false; }
	EditorObjectMgr::BUILDING_LIST buildings = EditorObjectMgr::instance()->getBuildings();
	EditorObjectMgr::BUILDING_LIST::EConstIterator it = buildings.Begin();
	while (!it.IsDone()) {
		if (areCloseEnough((*it)->getPosition().x, positionX)  && areCloseEnough((*it)->getPosition().y, positionY)) {
			break;
		}
		it++;
	}
	if (it.IsDone()) { assert(false); return false; }
	m_pBuilding = (*it);
	if (m_pBuilding) {
		m_LastNotedPositionX = m_pBuilding->getPosition().x;
		m_LastNotedPositionY = m_pBuilding->getPosition().y;
	}

	return true;
}

bool CSpecificStructureObjectiveCondition::Save( FitIniFile* file )
{
	assert(m_pBuilding);	// todo: exception handling

	file->writeIdFloat( "PositionX", m_pBuilding->getPosition().x  );
	file->writeIdFloat( "PositionY", m_pBuilding->getPosition().y );

	long cellx, celly;
	m_pBuilding->getCells(cellx, celly);
	file->writeIdULong( "CellX", cellx );
	file->writeIdULong( "CellY", celly );

	int alignment = m_pBuilding->getAlignment();
	file->writeIdULong( "Commander", alignment );
	return true;
}

bool CSpecificStructureObjectiveCondition::EditDialog() {
	ChooseBuildingDlg ChooseBuildingDlg(m_pBuilding);
	int ret = ChooseBuildingDlg.DoModal();
	if (m_pBuilding) {
		m_LastNotedPositionX = m_pBuilding->getPosition().x;
		m_LastNotedPositionY = m_pBuilding->getPosition().y;
	}
	return (IDOK == ret);
}

EString CSpecificStructureObjectiveCondition::InstanceDescription() {
	EString tmpEStr;
	Stuff::Vector3D pos = m_pBuilding->getPosition();
	const char *szDisplayName = m_pBuilding->getDisplayName(); // nb: localization
	assert(szDisplayName);
	tmpEStr.Format("(pos: %.3f, %.3f) %s", pos.x, pos.y, szDisplayName);
	return tmpEStr;
}

EString CSpecificStructureObjectiveCondition::ReadableTarget() {
	EString s;
	if (m_pBuilding) {
		Stuff::Vector3D pos = m_pBuilding->getPosition();
		const char *name = m_pBuilding->getDisplayName();
		s.Format("%s  (%.0f, %.0f)", (name && name[0]) ? name : "structure", pos.x, pos.y);
	} else {
		s.Format("unresolved structure  (%.0f, %.0f)", m_LastNotedPositionX, m_LastNotedPositionY);
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
	EditorObjectMgr::BUILDING_LIST::EConstIterator it = buildings.Begin();
	while (!it.IsDone()) {
		if (areCloseEnough((*it)->getPosition().x, m_LastNotedPositionX)  && areCloseEnough((*it)->getPosition().y, m_LastNotedPositionY)) {
			break;
		}
		it++;
	}
	if (it.IsDone()) { assert(false); return false; }
	m_pBuilding = (*it);
	return true;
}

EString CDestroySpecificStructure::Description() {
	EString retval = "DestroySpecificStructure"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_DestroySpecificStructure, retval);
	return retval;
}

EString CCaptureOrDestroyAllEnemyUnits::Description() {
	EString retval = "CaptureOrDestroyAllEnemyUnits"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_CaptureOrDestroyAllEnemyUnits, retval);
	return retval;
}

EString CCaptureOrDestroyNumberOfEnemyUnits::Description() {
	EString retval = "CaptureOrDestroyNumberOfEnemyUnits"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_CaptureOrDestroyNumberOfEnemyUnits, retval);
	return retval;
}

EString CCaptureOrDestroySpecificEnemyUnit::Description() {
	EString retval = "CaptureOrDestroySpecificEnemyUnit"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_CaptureOrDestroySpecificEnemyUnit, retval);
	return retval;
}

EString CCaptureOrDestroySpecificStructure::Description() {
	EString retval = "CaptureOrDestroySpecificStructure"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_CaptureOrDestroySpecificStructure, retval);
	return retval;
}

EString CDeadOrFledAllEnemyUnits::Description() {
	EString retval = "DeadOrFledAllEnemyUnits"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_DeadOrFledAllEnemyUnits, retval);
	return retval;
}

EString CDeadOrFledNumberOfEnemyUnits::Description() {
	EString retval = "DeadOrFledNumberOfEnemyUnits"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_DeadOrFledNumberOfEnemyUnits, retval);
	return retval;
}

EString CDeadOrFledSpecificEnemyUnit::Description() {
	EString retval = "DeadOrFledSpecificEnemyUnit"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_DeadOrFledSpecificEnemyUnit, retval);
	return retval;
}

EString CCaptureUnit::Description() {
	EString retval = "CaptureSpecificUnit"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_CaptureSpecificUnit, retval);
	return retval;
}

EString CCaptureStructure::Description() {
	EString retval = "CaptureSpecificStructure"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_CaptureSpecificStructure, retval);
	return retval;
}

EString CGuardSpecificUnit::Description() {
	EString retval = "GuardSpecificUnit"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_GuardSpecificUnit, retval);
	return retval;
}

EString CGuardSpecificStructure::Description() {
	EString retval = "GuardSpecificStructure"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_GuardSpecificStructure, retval);
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

EString CAreaObjectiveCondition::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("x: %.3f, y: %.3f, radius: %.3f", m_targetCenterX, m_targetCenterY, m_targetRadius);
	return tmpEStr;
}

EString CMoveAnyUnitToArea::Description() {
	EString retval = "MoveAnyUnitToArea"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_MoveAnyUnitToArea, retval);
	return retval;
}

EString CMoveAllUnitsToArea::Description() {
	EString retval = "MoveAllUnitsToArea"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_MoveAllUnitsToArea, retval);
	return retval;
}

EString CMoveAllSurvivingUnitsToArea::Description() {
	EString retval = "MoveAllSurvivingUnitsToArea"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_MoveAllSurvivingUnitsToArea, retval);
	return retval;
}

EString CMoveAllSurvivingMechsToArea::Description() {
	EString retval = "MoveAllSurvivingMechsToArea"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_MoveAllSurvivingMechsToArea, retval);
	return retval;
}

bool CBooleanFlagIsSet::operator==(const CObjectiveCondition &rhs) const {
	bool retval = false;
	const CBooleanFlagIsSet *pCastedRhs = dynamic_cast<const CBooleanFlagIsSet *>(&rhs);
	if ((pCastedRhs) && (CObjectiveCondition::operator==(rhs))) {
		if ((m_flagID == pCastedRhs->m_flagID) &&
			(m_value == pCastedRhs->m_value)) {
			retval = true;
		}
	}
	return retval;
}

bool CBooleanFlagIsSet::Read( FitIniFile* missionFile )
{
	long result = 0;
	EString tmpECStr;
	result = sReadIdString(missionFile, "FlagID", tmpECStr);
	if (NO_ERR != result) {
		/*for backward compatibility*/
		int flagIndex = 0;
		result = sReadIdWholeNum(missionFile, "FlagIndex", flagIndex);
		if (NO_ERR != result) { return false; }
		m_flagID.Format("%d", flagIndex);
	} else {
		m_flagID = tmpECStr.Data();
	}
	result = sReadIdBoolean(missionFile, "Value", m_value);
	return true;
}

bool CBooleanFlagIsSet::Save( FitIniFile* file )
{
	file->writeIdString("FlagID", m_flagID.Data());
	file->writeIdBoolean("Value", m_value);
	return true;
}

bool CBooleanFlagIsSet::EditDialog() {
	CBooleanFlagIsSetDialog BooleanFlagIsSetDlg;
	BooleanFlagIsSetDlg.m_FlagID = m_flagID.Data();
	BooleanFlagIsSetDlg.m_Value = 1;
	if (false == m_value) { BooleanFlagIsSetDlg.m_Value = 0; }
	if (IDOK == BooleanFlagIsSetDlg.DoModal()) {
		m_flagID = BooleanFlagIsSetDlg.m_FlagID.GetBuffer(0);
		m_value = true;
		if (0 == BooleanFlagIsSetDlg.m_Value) { m_value = false; }
		return true;
	} else {
		return false;
	}
}

EString CBooleanFlagIsSet::Description() {
	EString retval = "BooleanFlagIsSet"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_BooleanFlagIsSet, retval);
	return retval;
}

EString CBooleanFlagIsSet::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("flag name: %s, value: %d", m_flagID.Data(), m_value);
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

EString CElapsedMissionTime::Description() {
	EString retval = "ElapsedMissionTime"; /* needs to be put somewhere localizable */
	ESLoadString(IDS_ElapsedMissionTime, retval);
	return retval;
}

EString CElapsedMissionTime::InstanceDescription() {
	EString tmpEStr;
	tmpEStr.Format("%f seconds", m_time);
	return tmpEStr;
}
