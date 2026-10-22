// The two process pointers that a mission save must carry as null, pinned in the source of the functions that write and read them.
//
// WHAT: Gate::CopyTo writes GateData::closestObject as null and Gate::Load drops a nonzero value of an older save with one log line;
// MechWarrior::copyToData writes every QueuedTacOrder::marker of the queue as null.
// WHY: both fields held addresses of the writing process, so two saves of one game state differed and a load of an older save could follow a wild
// pointer. Gate and MechWarrior need the whole object layer to construct, which the unit tier does not link, so this reads the function bodies
// from the repository and checks the statements in order; the byte-level proof is `saveload_roundtrip check` on the mc2_24 baseline (gates) and
// mc2_01 / mc2_03 / mc2_10 / mc2_16 (queues), which fails when a pointer returns to a record.
#include "doctest.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef MC2_REPO_ROOT
#define MC2_REPO_ROOT "."
#endif

namespace {

std::string readSource(const char *relative)
{
	std::ifstream in(std::filesystem::path(MC2_REPO_ROOT) / relative, std::ios::binary);
	std::stringstream all;
	all << in.rdbuf();
	return all.str();
}

// Source text of the function whose signature starts at `signature`: from there to the first line that is exactly "}" (the engine closes
// every function at column 0). Whitespace runs are collapsed so the check does not depend on indentation or line endings.
std::string functionBody(const std::string &source, const char *signature)
{
	const size_t begin = source.find(signature);
	if (begin == std::string::npos)
		return std::string();
	size_t end = source.find("\n}", begin);
	if (end == std::string::npos)
		end = source.size();
	std::string text;
	bool space = false;
	for (size_t i = begin; i < end; ++i) {
		const unsigned char c = (unsigned char)source[i];
		if (std::isspace(c)) {
			space = true;
			continue;
		}
		if (space && !text.empty())
			text += ' ';
		space = false;
		text += (char)c;
	}
	return text;
}

} // namespace

TEST_CASE("area-cap runtime refusal guards builder writes and final allocation")
{
	const auto source = readSource("mclib/move_globalmap_build.cpp");
	for (const char* signature : {"bool GlobalMap::fillArea (", "bool GlobalMap::fillSpecialArea ("}) {
		const auto body = functionBody(source, signature);
		const auto guard = body.find("if (!mc2::globalmap::areaCountInRange(area + 1)) Fatal(");
		REQUIRE(guard != std::string::npos);
		CHECK(guard < body.find("areaView[row, col] = area;"));
	}
	const auto body = functionBody(source, "void GlobalMap::calcAreas (");
	const auto guard = body.find("if (!mc2::globalmap::areaCountInRange(numAreas)) Fatal(");
	REQUIRE(guard != std::string::npos);
	CHECK(guard < body.find("ownZeroed(areasStore"));
}

TEST_CASE("tactical unpack drops absent terrain targets and load filters before marker restore")
{
	const auto body = functionBody(readSource("code/tacordr.cpp"), "long TacticalOrder::unpack (");
	const std::string lookup = "GameObjectPtr obj = targetPartId ? ObjectManager->findByPartId(*targetPartId) : nullptr;";
	const std::string drop = "targetWID = obj ? obj->getWatchID() : 0;";
	const auto first = body.find(lookup);
	REQUIRE(first != std::string::npos);
	const auto second = body.find(lookup, first + lookup.size());
	REQUIRE(second != std::string::npos);
	CHECK(body.find(drop, first) < second);
	CHECK(body.find(drop, second) != std::string::npos);
	CHECK(body.find("NULL Terrain Target") == std::string::npos);
	const auto load = functionBody(readSource("code/warrior_state.cpp"), "void MechWarrior::copyFromData (");
	const auto filter = load.find("dropCompactTerrainOrdersOnLoad(");
	REQUIRE(filter != std::string::npos);
	CHECK(load.find("MemCpy(tacOrderQueue,data.tacOrderQueue);") < filter);
	CHECK(filter < load.find("mc2::restoreQueueMarkers("));
}

TEST_CASE("source pin: Gate::CopyTo writes the blocker pointer as null")
{
	const std::string body = functionBody(readSource("code/gate.cpp"), "void Gate::CopyTo (GateData *data)");
	REQUIRE_FALSE(body.empty());
	CHECK(body.find("data->closestObject = nullptr;") != std::string::npos);
	CHECK(body.find("data->closestObject = closestObject") == std::string::npos);
}

TEST_CASE("source pin: Gate::Load logs once for a stale blocker pointer, then drops it")
{
	const std::string body = functionBody(readSource("code/gate.cpp"), "void Gate::Load (GateData *data)");
	REQUIRE_FALSE(body.empty());
	const size_t check = body.find("if (data->closestObject) MC2_LOG_ONCE(Sim, Warn,");
	const size_t drop = body.find("closestObject = nullptr;");
	REQUIRE(check != std::string::npos);
	REQUIRE(drop != std::string::npos);
	CHECK(check < drop);   // the value is tested before it is overwritten
	CHECK(body.find("closestObject = data->closestObject") == std::string::npos);
}

TEST_CASE("source pin: MechWarrior::copyToData writes every queued order marker as null, after the queue is copied")
{
	const std::string body = functionBody(readSource("code/warrior_state.cpp"), "void MechWarrior::copyToData (MechWarriorData &data)");
	REQUIRE_FALSE(body.empty());
	const size_t copy = body.find("MemCpy(data.tacOrderQueue,tacOrderQueue);");
	const size_t clear = body.find("for (auto& queued : data.tacOrderQueue) queued.marker = nullptr;");
	REQUIRE(copy != std::string::npos);
	REQUIRE(clear != std::string::npos);
	CHECK(copy < clear);   // clearing before the copy would write the pointers back
}
