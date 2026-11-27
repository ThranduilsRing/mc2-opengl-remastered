// Real-compiler proof for the Smarter profile (ai-abl-compat-check-1): every stock warrior that a campaign
// mission (mc2_01-24, tut_01-05) uses compiles after FixABL against orders, miscfunc, the Magic corebrain and
// the stock headers. Stock sources come from the install's mission.fst and the deployed
// mods/magic-brain-1.45 profile (working directory, else the golden v0.4c install); the case skips when absent.
#include "doctest.h"
#include "abl.h"
#include "ablmc2_internal.h"
#include "lz.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {

using Sources = std::map<std::string, std::string>;   // lowercase forward-slash data/missions/... -> text

std::string lower(std::string name) {
	std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::replace(name.begin(), name.end(), '\\', '/');
	return name;
}

#pragma pack(push, 1)
struct Entry { std::uint32_t offset, size, realSize, hash; char name[250]; };
#pragma pack(pop)
static_assert(sizeof(Entry) == 266);

bool readFst(const std::filesystem::path& path, Sources& sources) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (bytes.size() < 8) return false;
	auto* const data = reinterpret_cast<unsigned char*>(const_cast<char*>(bytes.data()));
	std::uint32_t count = 0;
	std::memcpy(&count, data + 4, 4);
	if (count > (bytes.size() - 8) / sizeof(Entry)) return false;
	for (std::uint32_t i = 0; i < count; ++i) {
		const auto* entry = reinterpret_cast<const Entry*>(data + 8 + static_cast<size_t>(i) * sizeof(Entry));
		if (!std::memchr(entry->name, 0, sizeof(entry->name)) || entry->offset > bytes.size() ||
			entry->size > bytes.size() - entry->offset) return false;
		const std::string name = lower(entry->name);
		if (name.rfind("data/missions/", 0) != 0) continue;
		unsigned char* const begin = data + entry->offset;
		std::string text;
		if (entry->size == entry->realSize) {
			text.assign(reinterpret_cast<const char*>(begin), entry->size);
		} else {
			text.resize(entry->realSize);
			const size_t size = LZDecomp(reinterpret_cast<MemoryPtr>(text.data()), begin, entry->size, entry->realSize);
			if (size != entry->realSize) return false;
		}
		sources[name] = std::move(text);
	}
	return !sources.empty();
}

std::string readFile(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// In-memory ABL sources over the game's own native registration (initABL): the compiler then resolves
// coreAttack, newScan, MECH_CLASS... exactly as a mission load does.
struct Fixture {
	Sources& sources;
	std::string fatal;
	struct Reader { const std::string* text; size_t offset = 0; };
	static Fixture* current;

	static const std::string* find(const char* name) {
		const std::string k = lower(name);
		const auto direct = current->sources.find(k);
		if (direct != current->sources.end()) return &direct->second;
		for (const char* folder : {"data/missions/warriors/", "data/missions/"}) {
			const auto loose = current->sources.find(folder + k.substr(k.rfind('/') + 1));
			if (loose != current->sources.end()) return &loose->second;
		}
		return nullptr;
	}
	static long open(void** file, const char* name) {
		const std::string* text = find(name);
		if (!text) return -1;
		*file = new Reader{text};
		return 0;
	}
	static long close(void** file) {
		delete static_cast<Reader*>(*file);
		*file = nullptr;
		return 0;
	}
	static bool eof(void* file) {
		const auto& reader = *static_cast<Reader*>(file);
		return reader.offset == reader.text->size();
	}
	static long read(void* file, unsigned char* buffer, long length) {
		auto& reader = *static_cast<Reader*>(file);
		const long count = static_cast<long>(std::min<size_t>(static_cast<size_t>(length), reader.text->size() - reader.offset));
		std::memcpy(buffer, reader.text->data() + reader.offset, static_cast<size_t>(count));
		reader.offset += static_cast<size_t>(count);
		return count;
	}
	static long remaining(void* file) {
		const auto& reader = *static_cast<Reader*>(file);
		return static_cast<long>(reader.text->size() - reader.offset);
	}
	static void fatalCallback(long, mc2::czstring message) { current->fatal += message; }

	decltype(ABLFile::openCB) savedOpen;
	decltype(ABLFile::closeCB) savedClose;
	decltype(ABLFile::eofCB) savedEof;
	decltype(ABLFile::readCB) savedRead;
	decltype(ABLFile::remainingCB) savedRemaining;
	decltype(ABLFatalCallback) savedFatal;

	explicit Fixture(Sources& s) : sources(s) {
		current = this;
		initABL();
		savedOpen = ABLFile::openCB; savedClose = ABLFile::closeCB; savedEof = ABLFile::eofCB;
		savedRead = ABLFile::readCB; savedRemaining = ABLFile::remainingCB; savedFatal = ABLFatalCallback;
		ABLFile::openCB = open; ABLFile::closeCB = close; ABLFile::eofCB = eof;
		ABLFile::readCB = read; ABLFile::remainingCB = remaining; ABLFatalCallback = fatalCallback;
		ABLi_setDebugPrintCallback(nullptr);
	}
	~Fixture() {
		ABLi_close();
		ABLFile::openCB = savedOpen; ABLFile::closeCB = savedClose; ABLFile::eofCB = savedEof;
		ABLFile::readCB = savedRead; ABLFile::remainingCB = savedRemaining; ABLFatalCallback = savedFatal;
		current = nullptr;
	}
};
Fixture* Fixture::current = nullptr;

std::string compile(Fixture& fixture, const std::string& name, bool library) {
	fixture.fatal.clear();
	long errors = 0;
	const bool ok = library ? ABLi_loadLibrary(name.c_str(), &errors) != nullptr : ABLi_preProcess(name.c_str(), &errors) >= 0;
	if (ok && !errors) return {};
	return name + ": " + (fixture.fatal.empty() ? std::to_string(errors) + " errors" : fixture.fatal);
}

}

TEST_CASE("MagicBrain/every campaign warrior compiles FixABL'd against the Magic library with the real compiler") {
	namespace fs = std::filesystem;
	const fs::path install = fs::is_regular_file("mission.fst") ? fs::current_path() : fs::path("A:/Games/mc2-opengl/mc2-win64-v0.4c");
	const fs::path profile = install / "mods/magic-brain-1.45/data/missions";
	if (!fs::is_regular_file(install / "mission.fst") || !fs::is_regular_file(profile / "corebrain.abx")) {
		MESSAGE("no install carrying the Smarter profile at " << install.string() << "; case skipped");
		return;
	}
	Sources sources;
	REQUIRE(readFst(install / "mission.fst", sources));
	for (const auto& file : fs::recursive_directory_iterator(profile))
		if (file.is_regular_file()) sources["data/missions/" + lower(file.path().string().substr(profile.string().size() + 1))] = readFile(file.path());

	std::map<std::string, std::set<std::string>> missions;
	const std::regex campaign("data/missions/(mc2|tut)_..\\.fit$");
	const std::regex brainKey("Brain\\s*=\\s*\"([^\"]+)\"");
	for (const auto& [name, text] : sources) {
		if (!std::regex_search(name, campaign)) continue;
		auto& brains = missions[name.substr(0, name.size() - 4) + ".abl"];
		for (auto it = std::sregex_iterator(text.begin(), text.end(), brainKey); it != std::sregex_iterator(); ++it) {
			std::string brain = lower((*it)[1].str());
			if (!brain.ends_with(".abl")) brain += ".abl";
			brains.insert("data/missions/warriors/" + brain);
		}
	}
	std::vector<std::string> failures;
	int compiled = 0;
	for (const auto& [mission, brains] : missions) {
		Fixture fixture(sources);
		for (const char* lib : {"data/missions/orders.abx", "data/missions/miscfunc.abx", "data/missions/corebrain.abx"}) {
			const std::string failure = compile(fixture, lib, true);
			if (!failure.empty()) failures.push_back(failure);
		}
		if (failures.empty()) {
			const std::string missionFailure = compile(fixture, mission, false);
			REQUIRE_MESSAGE(missionFailure.empty(), missionFailure);
			for (const auto& name : brains) {
				const std::string failure = compile(fixture, name, false);
				if (failure.empty()) ++compiled; else failures.push_back(failure);
			}
			if (mission == missions.begin()->first)
			for (const char* known : {"dredcircle01", "dredpatrol01", "hal", "infantry001"})   // non-campaign: reported, not failed
				MESSAGE("non-campaign " << std::string(known) << ": " << std::string(compile(fixture, std::string("data/missions/warriors/") + known + ".abl", false).empty()
					? "compiles" : "does NOT compile"));
		}
	}
	MESSAGE("campaign warrior instances compiled=" << compiled << " across " << missions.size() << " missions against the Smarter profile at " << install.string());
	for (const auto& failure : failures) MESSAGE(failure);
	CHECK(failures.empty());
}
