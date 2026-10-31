//============================================================================
// saved_objects_check.h -- structural check of the object packets of an in-mission save.
//
// WHAT: a save stores one packet per object slot; GameObjectManager::Load tells the object class
// from the packet's byte size and sizes its per-class arrays from the header counts. This header is
// the pure rule for "does the packet run agree with its header": every slot's size is empty or one
// known class size, the per-class totals equal the header counts, and the run is complete.
// WHY: a save written by an older build has different struct sizes, so a class is not recognised and
// its array stays unfilled; the loader then dereferenced the empty slots and crashed. The check runs
// BEFORE the running mission is torn down so a bad save is refused and the game carries on.
// Header-only, no engine types, so the doctest can drive it with synthetic sizes.
//============================================================================
#ifndef SAVED_OBJECTS_CHECK_H
#define SAVED_OBJECTS_CHECK_H

#include <cstdint>
#include <optional>
#include <string_view>
#include <cstring>
#include "team_save_data.h"
#include "commander_save_data.h"

namespace mc2_saved_objects {

// Validate home membership before teardown; sparse legacy IDs remain valid.
template<class File>
bool checkHomes(File& file, long objectPacket, long commanderPacket, bool teamValues, bool commanderValues) {
    auto read = [&]<class T>(long packet, T& value, bool tagged, uint32_t tag, bool prefix = false) {
        if (packet < 0 || packet >= file.getNumPackets() || file.seekPacket(packet) != 0) return false;
        const auto size = file.getPacketSize();
        const auto expected = tagged ? mc2::save::encodeTaggedRecord(T{}, tag).size() : sizeof(T);
        if (size < expected || (!prefix && size != expected) || size > expected + 4 + 1024 * 4) return false;
        mc2::save::Bytes bytes(size);
        if (file.readPacket(packet, bytes.data()) != size) return false;
        if (tagged) return mc2::save::decodeTaggedRecord(std::span(bytes).first(expected), value, tag);
        std::memcpy(&value, bytes.data(), sizeof(T));
        return true;
    };
    TeamStaticData teams{};
    long teamPacket = -1;
    for (long count = 1; count <= MAX_TEAMS; ++count) {
        const long candidate = objectPacket - count - 1;
        if (read(candidate, teams, teamValues, mc2::save::teamStaticTag) && teams.numTeams == count) {
            teamPacket = candidate;
            break;
        }
    }
    if (teamPacket < 0 || teams.homeTeamId < 0 || teams.homeTeamId >= MAX_TEAMS) return false;
    bool homeTeam = false;
    std::array<bool, MAX_TEAMS> teamIds{};
    for (long i = 0; i < teams.numTeams; ++i) {
        TeamData team{};
        if (!read(teamPacket + 1 + i, team, teamValues, mc2::save::teamDataTag, true) ||
            team.id < 0 || team.id >= MAX_TEAMS || teamIds[team.id]) return false;
        teamIds[team.id] = true;
        homeTeam |= team.id == teams.homeTeamId;
    }
    StaticCommanderData commanders{};
    if (!homeTeam || !read(commanderPacket, commanders, commanderValues, mc2::save::commanderStaticTag) ||
        commanders.numCommanders < 1 || commanders.numCommanders > MAX_COMMANDERS ||
        commanders.homeCommanderId < 0 || commanders.homeCommanderId >= MAX_COMMANDERS) return false;
    bool homeCommander = false;
    std::array<bool, MAX_COMMANDERS> commanderIds{};
    for (long i = 0; i < commanders.numCommanders; ++i) {
        CommanderData commander{};
        if (!read(commanderPacket + 1 + i, commander, commanderValues, mc2::save::commanderDataTag) ||
            commander.id < 0 || commander.id >= MAX_COMMANDERS || commanderIds[commander.id] ||
            commander.teamId < 0 || commander.teamId >= MAX_TEAMS || !teamIds[commander.teamId]) return false;
        commanderIds[commander.id] = true;
        homeCommander |= commander.id == commanders.homeCommanderId;
    }
    return homeCommander;
}

// Order matters: the loader tests the sizes in this order and the first match wins.
constexpr int kTerrain = 0;
constexpr int kBuilding = 1;
constexpr int kTurret = 2;
constexpr int kGate = 3;
constexpr int kArtillery = 4;
constexpr int kCarnage = 5;
constexpr int kMech = 6;
constexpr int kVehicle = 7;
constexpr int kBolt = 8;
constexpr int kClassCount = 9;

// sizeof() of each packed object record of the running build, plus the two base records that must
// never be saved on their own.
struct RecordSizes {
	uint32_t byClass[kClassCount];
	uint32_t pureObject;
	uint32_t pureMover;
};

// The counts of the saved header. Elementals and lights have no loader branch in the engine, so a
// save that holds any is refused instead of being half read.
struct HeaderCounts {
	int32_t byClass[kClassCount];
	int32_t elementals;
	int32_t lights;
};

// A header counts as a candidate only when every count is non-negative and the slot total is sane.
// A 60-byte packet elsewhere in the file must not be mistaken for the header.
constexpr int64_t kMaxSlots = 1 << 20;

inline int64_t slotCount(const HeaderCounts& h)
{
	int64_t total = static_cast<int64_t>(h.elementals) + h.lights;
	for (int c = 0; c < kClassCount; ++c)
		total += h.byClass[c];
	return total;
}

inline bool plausibleHeader(const HeaderCounts& h)
{
	if (h.elementals < 0 || h.lights < 0)
		return false;
	for (int c = 0; c < kClassCount; ++c)
		if (h.byClass[c] < 0)
			return false;
	const int64_t total = slotCount(h);
	return total > 0 && total < kMaxSlots;
}

// A packet of this size is a base record, which must never be saved on its own.
inline bool isBaseRecord(const RecordSizes& r, uint32_t size)
{
	return size == r.pureObject || size == r.pureMover;
}

// The class of a packet of `size` bytes, if one owns that size.
inline std::optional<int> classOfSize(const RecordSizes& r, uint32_t size)
{
	for (int c = 0; c < kClassCount; ++c)
		if (size == r.byClass[c])
			return c;
	return std::nullopt;
}

// Check the run that follows the header at packet `headerIndex`: slots 0..maxObjects, then the watch
// table of maxObjects+1 ids. `sizeAt(i)` is the byte size of packet i. -> nothing when the run is
// sound, else a short reason tag.
template <class SizeAt>
inline std::optional<std::string_view> checkRun(const HeaderCounts& h, const RecordSizes& r, uint32_t headerIndex,
                                               uint32_t numPackets, SizeAt sizeAt, bool watchValues = false)
{
	if (h.elementals != 0 || h.lights != 0)
		return "unsupported_object_class";
	const int64_t slots = slotCount(h) + 1;
	const int64_t first = static_cast<int64_t>(headerIndex) + 1;
	if (first + slots + 1 > static_cast<int64_t>(numPackets))
		return "truncated";

	int64_t seen[kClassCount] = {};
	for (int64_t i = 0; i < slots; ++i) {
		const uint32_t size = sizeAt(static_cast<uint32_t>(first + i));
		if (size == 0)
			continue;
		if (isBaseRecord(r, size))
			return "base_record_saved";
		const std::optional<int> c = classOfSize(r, size);
		if (!c)
			return "unknown_record_size";
		++seen[*c];
	}
	for (int c = 0; c < kClassCount; ++c)
		if (seen[c] != h.byClass[c])
			return "object_count_mismatch";

	if (sizeAt(static_cast<uint32_t>(first + slots)) != static_cast<uint32_t>(sizeof(int32_t) * slots + (watchValues ? 12 : 0)))
		return "bad_watch_table";
	return std::nullopt;
}

} // namespace mc2_saved_objects

#endif
