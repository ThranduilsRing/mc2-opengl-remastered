// prefs_init.h - deterministic preference initialization (replaces the hardware sniffer).
//
// options.cfg missing: write one fixed default set, the old sniffer's top tier (hiprefs.cfg), to
// orgprefs.cfg and options.cfg, overwriting like the sniffer's CopyFile(..., false) did. If the install
// lacks hiprefs.cfg, fall back to minprefs.cfg (the old pre-sniffer seed).
// options.cfg present: never touched. orgprefs.cfg missing (recovery file): seeded from minprefs.cfg
// without overwrite, exactly as the old CopyFile(..., true) did. File names are lowercase (Linux case).
#pragma once
#include <filesystem>
#include <system_error>

namespace mc2::prefs {

inline bool copyPref(const std::filesystem::path& src, const std::filesystem::path& dst, bool failIfExists)
{
	std::error_code ec;
	const auto opt = failIfExists ? std::filesystem::copy_options::none : std::filesystem::copy_options::overwrite_existing;
	return std::filesystem::copy_file(src, dst, opt, ec) && !ec;
}

// Returns true when options.cfg was created by this call.
inline bool initPreferences(const std::filesystem::path& dir)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	bool created = false;
	if (!fs::exists(dir / "options.cfg", ec)) {
		const fs::path src = fs::exists(dir / "hiprefs.cfg", ec) ? dir / "hiprefs.cfg" : dir / "minprefs.cfg";
		copyPref(src, dir / "orgprefs.cfg", false);
		created = copyPref(src, dir / "options.cfg", false);
	}
	if (!fs::exists(dir / "orgprefs.cfg", ec))
		copyPref(dir / "minprefs.cfg", dir / "orgprefs.cfg", true);
	return created;
}

}  // namespace mc2::prefs
