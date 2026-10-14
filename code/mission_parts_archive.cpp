// Part archives stage validated records before publishing mission state.
#include "mission.h"
#include "objmgr.h"
#include "objtype.h"
#include "team.h"
#include "comndr.h"

mc2::save::Part Part::saveRecord() const {
	mc2::save::Part record;
	record.objectWID = objectWID;
	record.objNumber = objNumber;
	record.baseColor = baseColor;
	record.highlightColor1 = highlightColor1;
	record.highlightColor2 = highlightColor2;
	record.active = active;
	record.exists = exists;
	record.destroyed = destroyed;
	record.position = {position.x, position.y, position.z};
	record.velocity = velocity;
	record.rotation = rotation;
	record.gestureId = gestureId;
	record.alignment = alignment;
	record.teamId = teamId;
	record.commanderID = commanderID;
	record.squadId = squadId;
	record.myIcon = myIcon;
	record.controlType = controlType;
	record.controlDataType = controlDataType;
	const auto end = std::find(std::begin(profileName), std::end(profileName), '\0');
	record.profileName = {2, std::string(profileName, end)};
	record.pilot = pilot;
	record.captureable = captureable;
	record.variantNum = variantNum;
	return record;
}

bool Part::restoreRecord(const mc2::save::Part& record, mc2::save::Diagnostic diagnostic) {
	mc2::save::Archive validation(std::move(diagnostic));
	// Native Part profiles carry Windows-1252 bytes; conversion would lose record identity.
	if (record.profileName.encoding != 2) validation.reject("part_profile_encoding");
	if (record.profileName.bytes.size() > 8) validation.reject("text_length");
	if (!validation.good()) return false;
	try {
		auto checked = record;
		validation(checked);
		if (record.squadId < (std::numeric_limits<char>::min)() ||
			record.squadId > (std::numeric_limits<char>::max)()) validation.reject("part_squad_narrowing");
		if (!validation.finish()) return false;
		Part candidate = *this;
		candidate.objectWID = record.objectWID;
		candidate.objNumber = record.objNumber;
		candidate.baseColor = record.baseColor;
		candidate.highlightColor1 = record.highlightColor1;
		candidate.highlightColor2 = record.highlightColor2;
		candidate.active = record.active;
		candidate.exists = record.exists;
		candidate.destroyed = record.destroyed;
		candidate.position.x = record.position.x;
		candidate.position.y = record.position.y;
		candidate.position.z = record.position.z;
		candidate.velocity = record.velocity;
		candidate.rotation = record.rotation;
		candidate.gestureId = record.gestureId;
		candidate.alignment = record.alignment;
		candidate.teamId = record.teamId;
		candidate.commanderID = record.commanderID;
		candidate.squadId = static_cast<char>(record.squadId);
		candidate.myIcon = record.myIcon;
		candidate.controlType = record.controlType;
		candidate.controlDataType = record.controlDataType;
		std::fill(std::begin(candidate.profileName), std::end(candidate.profileName), '\0');
		std::copy(record.profileName.bytes.begin(), record.profileName.bytes.end(), candidate.profileName);
		candidate.pilot = record.pilot;
		candidate.captureable = record.captureable;
		candidate.variantNum = record.variantNum;
		*this = candidate;
		return true;
	} catch (const std::bad_alloc&) { validation.reject("allocation"); }
	  catch (const std::length_error&) { validation.reject("length"); }
	return false;
}
bool Mission::loadParts(const mc2::save::Section& section, std::uint32_t schemaVersion,
	mc2::save::Diagnostic diagnostic) {
	mc2::save::Archive reader(section.payload, diagnostic);
	if (schemaVersion != 1) reader.reject("schema_version");
	if (section.tag != 10 || !section.required) reader.reject("part_section_tag");
	if (numParts > mc2::save::maxSequenceLength || (numParts && !parts)) reader.reject("part_count");
	if (!reader.good()) return false;
	mc2::save::development::PartSection snapshot;
	reader.sequence(snapshot.parts, static_cast<std::uint32_t>(numParts));
	if (!reader.finish()) return false;
	if (snapshot.parts.size() != numParts) { reader.reject("part_count"); return false; }
	try {
		std::vector<Part> staged;
		staged.reserve(numParts);
		std::array<bool, MAX_WATCHES> referenced{};
		for (std::size_t i = 0; i < snapshot.parts.size(); ++i) {
			const auto& record = snapshot.parts[i];
			// Pilot zero and watch zero represent an unassigned Part; -1 is a neutral owner.
			if (record.pilot >= MAX_WARRIORS || (record.pilot &&
				(!MechWarrior::warriorList[record.pilot] || !MechWarrior::warriorList[record.pilot]->isUsed()))) {
				reader.reject("part_pilot_reference"); return false;
			}
			if (record.teamId < -1 || record.teamId >= MAX_TEAMS) {
				reader.reject("part_team_reference"); return false;
			}
			if (record.commanderID < -1 || record.commanderID >= MAX_COMMANDERS) {
				reader.reject("part_commander_reference"); return false;
			}
			const auto* types = ObjectManager ? ObjectManager->objTypeManager : nullptr;
			// Skipped alternatives have no live object, pilot or type reference.
			const bool omitted = !record.objNumber && !record.objectWID && !record.pilot &&
				!record.exists && !record.active;
			if (!omitted && (!types || !types->table || types->numObjectTypes <= 0 ||
				record.objNumber >= static_cast<std::uint32_t>(types->numObjectTypes) ||
				!types->table[record.objNumber])) {
				reader.reject("part_type_reference"); return false;
			}
			if (record.objectWID < 0 || record.objectWID >= MAX_WATCHES) {
				reader.reject("part_watch_reference"); return false;
			}
			if (record.objectWID) {
				if (referenced[record.objectWID]) { reader.reject("part_watch_duplicate"); return false; }
				referenced[record.objectWID] = true;
				auto* object = ObjectManager ? ObjectManager->getByWatchID(record.objectWID) : nullptr;
				if (!object || !object->isMover() || !record.pilot ||
					object->getPilot() != MechWarrior::warriorList[record.pilot] ||
					object->getTypeHandle() != static_cast<long>(record.objNumber)) {
					reader.reject("part_watch_reference"); return false;
				}
			}
			staged.push_back(parts[i + 1]);
			if (!staged.back().restoreRecord(record, diagnostic)) return false;
		}
		// Omitted CSV import hints stay with their live Parts; slot zero is always reserved.
		for (std::size_t i = 0; i < staged.size(); ++i) parts[i + 1] = staged[i];
	} catch (const std::bad_alloc&) { reader.reject("allocation"); return false; }
	  catch (const std::length_error&) { reader.reject("length"); return false; }
	return true;
}
