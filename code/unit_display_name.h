#pragma once
// The one owner of "does this text name anything" and of the order a display name is chosen in:
// resource ID -> free-text name -> variant name -> chassis name. Mod data points chassis, weapon and
// vehicle name cells at stock string-table slots the stock table never filled ("undefined"), so a
// lookup that trusts the slot alone puts that word on screen.
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace mc2::names {

// The text without the blanks around it.
inline std::string_view trimmed(std::string_view text) {
	const auto blank = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
	while (!text.empty() && blank(text.front())) text.remove_prefix(1);
	while (!text.empty() && blank(text.back())) text.remove_suffix(1);
	return text;
}

// The NameID cell of the master list holds an mc2res.dll slot or free text, the chassis' display name.
// CSVFile::readLong always reports success (text that is not a number reads as 0), so the cell is read as
// text first. It is the slot only when the whole cell, trimmed, is one integer: a name may start with a
// digit or a sign ("3D Custom", "+Uziel") and is the name all the same.
template <class Csv>
inline void readNameCell(Csv& csv, long row, long col, long& id, char* text, size_t textSize) {
	std::string cell(256, '\0');
	if (csv.readString(row, col, cell.data(), static_cast<unsigned long>(cell.size())) != 0) return; // NO_ERR
	const std::string_view whole = trimmed({cell.data(), cell.find('\0')});
	const size_t sign = !whole.empty() && (whole.front() == '-' || whole.front() == '+');
	if (whole.size() > sign && whole.find_first_not_of("0123456789", sign) == std::string_view::npos) {
		csv.readLong(row, col, id);
		return;
	}
	const size_t length = std::min(cell.find('\0'), textSize - 1);
	std::copy_n(cell.begin(), static_cast<std::ptrdiff_t>(length), text);
	text[length] = '\0';
}

inline bool iequals(std::string_view a, std::string_view b) {
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
		[](char l, char r) { return std::tolower(static_cast<unsigned char>(l)) == std::tolower(static_cast<unsigned char>(r)); });
}

// Blank, the spreadsheet placeholder or the catalog's own missing text carries no name.
inline bool usable(std::string_view text) {
	text = trimmed(text);
	if (text.empty()) return false;
	for (const std::string_view marker : {"undefined", "<missing-string", "NULL for ID"})
		if (text.size() >= marker.size() && iequals(text.substr(0, marker.size()), marker)) return false;
	return true;
}

inline std::string resolve(std::string_view resource, std::string_view freeText, std::string_view variant, std::string_view chassis) {
	for (const std::string_view candidate : {resource, freeText, variant, chassis})
		if (usable(candidate)) return std::string(candidate);
	return {};
}

// The string table's text for `id`, blank when the slot carries no name.
std::string resource(long id);

} // namespace mc2::names
