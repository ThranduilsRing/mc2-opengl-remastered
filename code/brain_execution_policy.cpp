// BrainExecutionPolicy decision (see brain_execution_policy.h).
#include "brain_execution_policy.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <filesystem>

#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "file.h"
#include "mc2fmt.h"
#include "brain_missionfit_oporbd.h"
#include "game_rules.h"

namespace mc2::brainpolicy {

Policy g_policy = Policy::Legacy;
std::uint32_t g_generation = 1;

const char* name(Policy p) { return p == Policy::TechBrain ? "techbrain" : "legacy"; }

namespace {

bool fileExists(std::string_view path) {
	File f;
	return f.open(std::string(path).c_str()) == NO_ERR;
}

bool overlayBrainFile(std::string_view path) {
	if (ResolveModFileSource(path).path) return true;
	if (fileExists(path)) {
		static std::unordered_set<std::string> ignored;
		if (const auto [it, inserted] = ignored.emplace(path); inserted)
			mc2::fmt::print_err("[BRAIN_POLICY] ignored base-game brain sidecar={} (use a mod overlay or explicit brain policy)\n",
				mc2::fmt::safe(it->c_str()));
	}
	return false;
}

// True when the raw mission.fit text carries at least one inline Brain{} block.
bool missionFitHasBrainBlock(std::string_view missionFitPath) {
	File f;
	if (f.open(std::string(missionFitPath).c_str()) != NO_ERR) return false;
	const unsigned long sz = f.getLength();
	std::string text(sz, '\0');
	if (sz) f.read(reinterpret_cast<MemoryPtr>(text.data()), static_cast<long>(sz));
	static MissionFitBrain probe;
	return parseMissionFitBrains(text.c_str(), &probe, 1) > 0;
}

}  // namespace

bool profileInstalled(std::string_view root) {
	const std::filesystem::path base = std::filesystem::path(root) / "magic-brain-1.45";
	return fileExists((base / "mod.json").string()) && fileExists((base / "data/missions/corebrain.abx").string());
}

void decide(std::string_view missionFitPath, std::string_view missionDir, std::string_view missionName) {
	const char* why = "none";
	Policy p = Policy::Legacy;
	if (mc2::gate::is<"MC2_BRAIN_POLICY", "legacy">()) {
		why = "forced-legacy";
	} else if (mc2::gate::is<"MC2_BRAIN_POLICY", "techbrain">()) {
		p = Policy::TechBrain; why = "forced-techbrain";
	} else if (mc2::rules::current().ai == 1 && magicCampaign(mc2::gate::str<"MC2_ACTIVE_MOD">())) {
		why = profileInstalled("mods") ? "player-smarter-magic-1.45" : "player-smarter-magic-1.45 magic-profile-missing,stock-brain";
		const std::string base = std::string(missionDir).append(missionName);
		if (missionFitHasBrainBlock(missionFitPath))                    { p = Policy::TechBrain; why = "mission.fit Brain{}"; }
		else if (overlayBrainFile(base + "_specials.fit"))              { p = Policy::TechBrain; why = "mod _specials.fit"; }
		else if (overlayBrainFile(base + "_ai.fit"))                    { p = Policy::TechBrain; why = "mod _ai.fit"; }
		else if (!mc2::gate::str<"MC2_BRAIN_SPECIAL_FIT">().empty())    { p = Policy::TechBrain; why = "special-fit-gate"; }
	} else {
		why = "player-stock";
	}
	g_policy = p;
	++g_generation;
	if (mc2::rules::current().ai == 1 && !magicCampaign(mc2::gate::str<"MC2_ACTIVE_MOD">()))
		mc2::fmt::print_err("[BRAIN_POLICY] smarter-unavailable mod={}\n", mc2::fmt::safe(mc2::gate::str<"MC2_ACTIVE_MOD">().c_str()));
	mc2::fmt::print_err("[BRAIN_POLICY] mission={} policy={} reason={}\n",
		mc2::fmt::safe(std::string(missionName).c_str()), mc2::fmt::safe(name(p)), mc2::fmt::safe(why));
}

}  // namespace mc2::brainpolicy
