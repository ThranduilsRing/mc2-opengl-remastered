// The structural check of the object packets of an in-mission save.
// A save written by an older build holds records of other sizes; the check must refuse it with a reason
// instead of letting the loader walk off unfilled arrays.
#include "doctest.h"
#include "saved_objects_check.h"
#include "file.h"
#include "packet.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

TEST_CASE("saved objects: home identities reject malformed tagged and legacy fixtures") {
    struct Packets {
        std::vector<mc2::save::Bytes> data;
        long cursor = 0;
        long getNumPackets() const { return static_cast<long>(data.size()); }
        int seekPacket(long p) { cursor = p; return 0; }
        size_t getPacketSize() const { return data[cursor].size(); }
        size_t readPacket(long p, unsigned char* out) {
            std::memcpy(out, data[p].data(), data[p].size());
            return data[p].size();
        }
    } file;
    for (bool tagged : {false, true}) {
        auto bytes = [tagged](auto value, uint32_t tag) {
            if (tagged) return mc2::save::encodeTaggedRecord(value, tag);
            const auto* p = reinterpret_cast<const uint8_t*>(&value);
            return mc2::save::Bytes(p, p + sizeof(value));
        };
        TeamStaticData teams{};
        teams.numTeams = 1;
        teams.homeTeamId = 6; // Present sparse ID, greater than the loaded count.
        TeamData team{};
        team.id = 6;
        StaticCommanderData commanders{1, 7};
        CommanderData commander{};
        commander.id = 7;
        commander.teamId = 6;
        auto fixture = [&] {
            file.data = {bytes(teams, mc2::save::teamStaticTag), bytes(team, mc2::save::teamDataTag), {},
                         bytes(commanders, mc2::save::commanderStaticTag), bytes(commander, mc2::save::commanderDataTag)};
        };
        auto valid = [&] { return mc2_saved_objects::checkHomes(file, 2, 3, tagged, tagged); };
        fixture();
        CHECK(valid());
        teams.homeTeamId = 5;
        fixture();
        CHECK_FALSE(valid());
        teams.homeTeamId = 6;
        commanders.homeCommanderId = 5;
        fixture();
        CHECK_FALSE(valid());
        commanders.homeCommanderId = 7;
        commander.teamId = 5;
        fixture();
        CHECK_FALSE(valid());
        commander.teamId = 6;
        fixture();
        file.data.back().pop_back();
        CHECK_FALSE(valid());
    }
}
#include <string_view>
#include <vector>

using namespace mc2_saved_objects;

namespace {

RecordSizes sizes() {
	RecordSizes r = {};
	const uint32_t s[kClassCount] = { 101, 102, 103, 104, 105, 106, 107, 108, 109 };
	for (int c = 0; c < kClassCount; ++c)
		r.byClass[c] = s[c];
	r.pureObject = 50;
	r.pureMover = 51;
	return r;
}

// A file of `prefix` unrelated packets, then the header, slot 0 (always empty), one slot per object and the watch table.
struct SaveImage {
	std::vector<uint32_t> packets;
	uint32_t headerIndex = 0;
	HeaderCounts counts = {};
};

SaveImage makeSave(const std::vector<int>& slotClasses, uint32_t prefix = 7) {
	SaveImage img;
	const RecordSizes r = sizes();
	img.packets.assign(prefix, 999);
	img.headerIndex = static_cast<uint32_t>(img.packets.size());
	img.packets.push_back(60);
	for (int c : slotClasses) {
		img.packets.push_back(r.byClass[c]);
		++img.counts.byClass[c];
	}
	// Slot 0 is the NULL handle and is always empty.
	img.packets.insert(img.packets.begin() + img.headerIndex + 1, 0u);
	const int64_t slots = slotCount(img.counts) + 1;
	img.packets.push_back(static_cast<uint32_t>(sizeof(int32_t) * slots));
	img.packets.push_back(4);   // the trailing mission number / END packets
	return img;
}

std::optional<std::string_view> check(const SaveImage& img) {
	return checkRun(img.counts, sizes(), img.headerIndex, static_cast<uint32_t>(img.packets.size()),
	                [&img](uint32_t i) { return img.packets[i]; });
}

} // namespace

TEST_CASE("saved objects: a sound run passes") {
	const SaveImage img = makeSave({ kTerrain, kTerrain, kBuilding, kTurret, kMech, kMech, kVehicle, kBolt });
	CHECK(plausibleHeader(img.counts));
	CHECK_FALSE(check(img).has_value());
}

TEST_CASE("saved objects: an older build's record size is refused, not read") {
	SaveImage img = makeSave({ kBuilding, kTurret, kTurret, kMech });
	// The turret record grew between builds: both turret packets now have a size no class owns.
	for (uint32_t& p : img.packets)
		if (p == 103)
			p = 99;
	const auto why = check(img);
	REQUIRE(why.has_value());
	CHECK((*why == "unknown_record_size"));
}

TEST_CASE("saved objects: a class missing from the run is a count mismatch") {
	SaveImage img = makeSave({ kBuilding, kTurret, kMech });
	--img.counts.byClass[kTurret];     // the header says a mech where the run holds a turret
	++img.counts.byClass[kMech];
	const auto why = check(img);
	REQUIRE(why.has_value());
	CHECK((*why == "object_count_mismatch"));
}

TEST_CASE("saved objects: a base record in a slot is refused") {
	SaveImage img = makeSave({ kBuilding, kMech });
	img.packets[img.headerIndex + 2] = sizes().pureMover;
	const auto why = check(img);
	REQUIRE(why.has_value());
	CHECK((*why == "base_record_saved"));
}

TEST_CASE("saved objects: a file cut short is refused") {
	SaveImage img = makeSave({ kBuilding, kTurret, kMech });
	img.packets.resize(img.packets.size() - 3);
	const auto why = check(img);
	REQUIRE(why.has_value());
	CHECK((*why == "truncated"));
}

TEST_CASE("saved objects: the watch table must have one id per slot") {
	SaveImage img = makeSave({ kBuilding, kMech });
	img.packets[img.packets.size() - 2] = 13;
	const auto why = check(img);
	REQUIRE(why.has_value());
	CHECK((*why == "bad_watch_table"));
}

TEST_CASE("saved objects: classes the loader has no branch for are refused") {
	SaveImage img = makeSave({ kBuilding, kMech });
	img.counts.lights = 1;
	CHECK((check(img) == "unsupported_object_class"));
	img.counts.lights = 0;
	img.counts.elementals = 2;
	CHECK((check(img) == "unsupported_object_class"));
}

TEST_CASE("saved objects: implausible headers are not candidates") {
	HeaderCounts h = {};
	CHECK_FALSE(plausibleHeader(h));            // no objects at all
	h.byClass[kMech] = -1;
	h.byClass[kBuilding] = 5;
	CHECK_FALSE(plausibleHeader(h));            // a negative count
	h.byClass[kMech] = 1 << 22;
	CHECK_FALSE(plausibleHeader(h));            // more slots than any mission has
	h.byClass[kMech] = 3;
	CHECK(plausibleHeader(h));
}

TEST_CASE("saved objects: the first matching class wins, base records are named") {
	RecordSizes r = sizes();
	CHECK(classOfSize(r, 101) == kTerrain);
	CHECK(classOfSize(r, 109) == kBolt);
	CHECK(isBaseRecord(r, 50));
	CHECK(isBaseRecord(r, 51));
	CHECK_FALSE(isBaseRecord(r, 101));
	CHECK_FALSE(classOfSize(r, 50).has_value());
	CHECK_FALSE(classOfSize(r, 7).has_value());
	r.byClass[kGate] = r.byClass[kTurret];      // two classes of one size: the loader's order decides
	CHECK(classOfSize(r, 103) == kTurret);
}

// The real packet container, written the way GameObjectManager::Save writes it (an unwritten slot, a zlib
// watch table): the sizes the check reads back must be the sizes it was written with.
TEST_CASE("saved objects: sizes read back from a real PacketFile drive the check") {
	const std::string path = "saved_objects_check_scratch.ims";
	const RecordSizes r = sizes();
	const int classes[] = { kBuilding, kTurret, kMech, kMech };
	const int n = static_cast<int>(sizeof(classes) / sizeof(classes[0]));
	HeaderCounts h = {};
	for (int c : classes)
		++h.byClass[c];
	const int slots = n + 1;

	std::vector<unsigned char> blob(1024, 0xAB);
	{
		PacketFile out;
		REQUIRE(out.create(path.c_str()) == NO_ERR);
		out.reserve(3 + slots + 2);
		int packet = 0;
		out.writePacket(packet++, blob.data(), 4, STORAGE_TYPE_RAW);            // an unrelated packet
		out.writePacket(packet++, blob.data(), 60, STORAGE_TYPE_RAW);           // the header
		packet++;                                                               // slot 0: never written
		for (int c : classes)
			out.writePacket(packet++, blob.data(), r.byClass[c], STORAGE_TYPE_RAW);
		out.writePacket(packet++, blob.data(), static_cast<unsigned>(sizeof(int32_t) * slots), STORAGE_TYPE_ZLIB);
		out.writePacket(packet++, blob.data(), 4, STORAGE_TYPE_RAW);            // END
		out.close();
	}
	{
		PacketFile in;
		REQUIRE(in.open(path.c_str()) == NO_ERR);
		auto sizeAt = [&in](uint32_t i) { in.seekPacket(static_cast<int>(i)); return static_cast<uint32_t>(in.getPacketSize()); };
		const uint32_t numPackets = static_cast<uint32_t>(in.getNumPackets());
		CHECK(sizeAt(2) == 0u);
		CHECK_FALSE(checkRun(h, r, 1, numPackets, sizeAt).has_value());

		HeaderCounts wrong = h;
		--wrong.byClass[kTurret];
		++wrong.byClass[kMech];
		const auto why = checkRun(wrong, r, 1, numPackets, sizeAt);
		REQUIRE(why.has_value());
		CHECK((*why == "object_count_mismatch"));
		in.close();
	}
	remove(path.c_str());
}
