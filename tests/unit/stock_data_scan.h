// tests/unit/stock_data_scan.h
//
// Offline readers for the stock mech data the node-coverage doctests scan (tgl/*.ini [Nodes], objects/*.csv variants,
// compbas.csv, tgl/*.package.json + GLB node names). The stock data is not in the repo: the tests locate it through
// MC2_TEST_SRCDATA (mc2srcdata root: objects/, tgl/) and MC2_TEST_INSTALL_DATA (an install's data/ dir), with the
// maintainer-machine paths as defaults, and the data-driven cases skip when it is absent. Header-only, no engine code.
#pragma once

#include "mclib/arm_node_remap.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace stockscan {

namespace fs = std::filesystem;

inline fs::path rootFromEnv(const char* env, const char* fallback) {
	const char* v = std::getenv(env);
	fs::path p = (v && *v) ? v : fallback;
	std::error_code ec;
	return fs::is_directory(p, ec) ? p : fs::path();
}
inline fs::path srcDataRoot() { return rootFromEnv("MC2_TEST_SRCDATA", "A:/Games/mc2-opengl/mc2srcdata-fresh"); }
inline fs::path installDataRoot() { return rootFromEnv("MC2_TEST_INSTALL_DATA", "A:/Games/mc2-opengl/mc2-win64-v0.4/data"); }

inline std::string readAll(const fs::path& p) {
	std::ifstream in(p, std::ios::binary);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

inline std::string lower(std::string s) {
	for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}
inline std::string trim(const std::string& s) {
	const auto b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) return {};
	return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// The weapon-node list of a mech .ini: one entry per [WeaponNodeN], in file order, with the node's WeaponType.
// `declared` is [Nodes] NumWeapon (-1 when the file has no [Nodes] block).
struct IniNodes {
	long declared = -1;
	std::vector<long> types;
};

inline IniNodes parseIniNodes(const std::string& text) {
	IniNodes r;
	std::istringstream in(text);
	std::string line, section;
	while (std::getline(in, line)) {
		line = trim(line);
		if (line.empty() || line.rfind("//", 0) == 0) continue;
		if (line.front() == '[') {
			section = line.substr(1, line.find(']') - 1);
			continue;
		}
		const auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
		if (section == "Nodes" && key.find("NumWeapon") != std::string::npos) r.declared = std::atol(val.c_str());
		if (section.rfind("WeaponNode", 0) == 0 && key.find("WeaponType") != std::string::npos) {
			const long idx = std::atol(section.c_str() + 10);
			if (static_cast<size_t>(idx) >= r.types.size()) r.types.resize(static_cast<size_t>(idx) + 1, -1);
			r.types[static_cast<size_t>(idx)] = std::atol(val.c_str());
		}
	}
	return r;
}

inline std::vector<std::string> csvFields(const std::string& line) {
	std::vector<std::string> out;
	std::string cur;
	bool quoted = false;
	for (char c : line) {
		if (c == '"') quoted = !quoted;
		else if (c == ',' && !quoted) { out.push_back(cur); cur.clear(); }
		else if (c != '\r') cur.push_back(c);
	}
	out.push_back(cur);
	return out;
}

// compbas.csv: component number -> MECH3D_WEAPONTYPE_* of a weapon (-1 for a non-weapon component).
inline std::map<long, long> parseCompbasWeaponTypes(const std::string& text) {
	std::map<long, long> m;
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		const auto f = csvFields(line);
		if (f.size() < 2) continue;
		if (f[1] == "MissileWeapon") m[std::atol(f[0].c_str())] = mc2_arm_remap::kMissile;
		else if (f[1] == "BallisticWeapon") m[std::atol(f[0].c_str())] = mc2_arm_remap::kBallistic;
		else if (f[1] == "EnergyWeapon") m[std::atol(f[0].c_str())] = mc2_arm_remap::kEnergy;
	}
	return m;
}

// One mech variant block: its weapons in inventory order, as node-type requests.
struct Variant {
	std::string name;
	std::vector<long> weaponTypes;
};

// objects/<mech>.csv: a "VariantID" line starts a variant; "ItemNMasterId,<id>,<bayX>,<bayY>,<CompBas#>,..." rows list its items.
inline std::vector<Variant> parseCsvVariants(const std::string& text, const std::map<long, long>& weaponOf, std::string* mechName) {
	std::vector<Variant> out;
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		const auto f = csvFields(line);
		if (f.size() < 2) continue;
		if (f[0] == "MechName" && mechName) *mechName = f[1];
		if (f[0] == "VariantID") out.emplace_back();
		else if (f[0] == "VariantName" && !out.empty()) out.back().name = f[1];
		else if (f[0].rfind("Item", 0) == 0 && f[0].find("MasterId") != std::string::npos && f.size() > 4 && !out.empty()) {
			const auto it = weaponOf.find(std::atol(f[4].c_str()));
			if (it != weaponOf.end()) out.back().weaponTypes.push_back(it->second);
		}
	}
	return out;
}

// Weapon-node bone names of a package json: every "weapon_*": "bone" pair of its "nodes" object.
inline std::vector<std::pair<std::string, std::string>> packageWeaponBones(const std::string& json) {
	std::vector<std::pair<std::string, std::string>> out;
	size_t pos = 0;
	while ((pos = json.find("\"weapon_", pos)) != std::string::npos) {
		const size_t kEnd = json.find('"', pos + 1);
		const size_t vBeg = json.find('"', json.find(':', kEnd) + 1);
		const size_t vEnd = json.find('"', vBeg + 1);
		if (kEnd == std::string::npos || vBeg == std::string::npos || vEnd == std::string::npos) break;
		out.emplace_back(json.substr(pos + 1, kEnd - pos - 1), json.substr(vBeg + 1, vEnd - vBeg - 1));
		pos = vEnd + 1;
	}
	return out;
}

// True when the GLB's JSON chunk names a node `bone` ("name":"bone", whitespace tolerant).
inline bool glbHasNode(const std::string& glb, const std::string& bone) {
	if (glb.size() < 20) return false;
	const uint32_t jsonLen = static_cast<uint8_t>(glb[12]) | (static_cast<uint8_t>(glb[13]) << 8) | (static_cast<uint8_t>(glb[14]) << 16) |
	                         (static_cast<uint32_t>(static_cast<uint8_t>(glb[15])) << 24);
	const std::string json = glb.substr(20, std::min<size_t>(jsonLen, glb.size() - 20));
	const std::string needle = "\"" + bone + "\"";
	for (size_t p = json.find(needle); p != std::string::npos; p = json.find(needle, p + 1)) {
		size_t b = p;
		while (b > 0 && (json[b - 1] == ' ' || json[b - 1] == ':')) --b;
		if (b >= 6 && json.compare(b - 6, 6, "\"name\"") == 0) return true;
	}
	return false;
}

} // namespace stockscan
