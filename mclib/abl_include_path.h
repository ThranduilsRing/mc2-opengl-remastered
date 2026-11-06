#pragma once
// ABL #include_ path resolution, shared by ablscan.cpp and its doctest (tests/unit/test_abl_include_path.cpp).
// The include sits next to the including file. Mission paths are "data/missions/..." on every platform and FST keys use
// '/', so the directory is taken with std::filesystem (either separator) and the result keeps '/' (a preferred_separator
// scan dropped the directory on Windows when the source path mixed separators, and OConst.abi failed to open).
#include <filesystem>
#include <string>

inline std::string ablIncludePath(const char* includingFile, const char* includeName) {
	return std::filesystem::path(includingFile).replace_filename(includeName).generic_string();
}
