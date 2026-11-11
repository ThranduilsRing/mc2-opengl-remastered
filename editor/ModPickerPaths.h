//-------------------------------------------------------------------------------------------------
// ModPickerPaths.h -- pure (std-only) helpers behind ModPicker: mod enumeration from
// mods/*/mod.json and the per-mod mission save directory. Header-only so the offline
// doctests (editor/tests/test_mod_picker.cpp) cover the exact code the editor runs.
//-------------------------------------------------------------------------------------------------
#ifndef MOD_PICKER_PATHS_H
#define MOD_PICKER_PATHS_H

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace ModPickerPaths
{
	struct Entry
	{
		std::string id;     // folder name == MC2_ACTIVE_MOD value (validator requires mod.json id == folder)
		std::string name;   // mod.json "name" (falls back to id)
	};

	// First string value of "key" in a mod.json text ("" when absent). Same minimal reader as
	// mclib/file.cpp ReadModJson; mod.json is the only format involved.
	inline std::string JsonString( const std::string& json, const char* key )
	{
		const std::string needle = std::string( "\"" ) + key + "\"";
		size_t pos = json.find( needle );
		if ( pos == std::string::npos ) return std::string();
		pos += needle.size();
		while ( pos < json.size() && ( json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t' ||
		                               json[pos] == '\r' || json[pos] == '\n' ) ) ++pos;
		if ( pos >= json.size() || json[pos] != '"' ) return std::string();
		const size_t end = json.find( '"', pos + 1 );
		if ( end == std::string::npos ) return std::string();
		return json.substr( pos + 1, end - pos - 1 );
	}

	// Every direct subfolder of modsRoot that holds a mod.json, sorted by id.
	inline std::vector<Entry> Scan( const std::filesystem::path& modsRoot )
	{
		std::vector<Entry> out;
		std::error_code ec;
		for ( std::filesystem::directory_iterator it( modsRoot, ec ), end; !ec && it != end; it.increment( ec ) )
		{
			std::error_code e2;
			if ( !it->is_directory( e2 ) ) continue;
			const std::filesystem::path manifest = it->path() / "mod.json";
			if ( !std::filesystem::is_regular_file( manifest, e2 ) ) continue;
			std::ifstream f( manifest, std::ios::binary );
			std::stringstream ss;
			ss << f.rdbuf();
			Entry e;
			e.id = it->path().filename().string();
			e.name = JsonString( ss.str(), "name" );
			if ( e.name.empty() ) e.name = e.id;
			out.push_back( e );
		}
		std::sort( out.begin(), out.end(), []( const Entry& a, const Entry& b ) { return a.id < b.id; } );
		return out;
	}

	// mods/<id>/data/missions (created when create is true). Empty path for stock ("" id), so
	// callers keep their existing default.
	inline std::filesystem::path SaveDir( const std::filesystem::path& modsRoot, const std::string& id, bool create )
	{
		if ( id.empty() ) return std::filesystem::path();
		std::filesystem::path dir = modsRoot / id / "data" / "missions";
		if ( create )
		{
			std::error_code ec;
			std::filesystem::create_directories( dir, ec );
		}
		return dir;
	}
}

#endif // MOD_PICKER_PATHS_H
