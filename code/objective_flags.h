// objective_flags.h -- the boolean flags that mission objectives set and test.
//
// WHAT: a team's table of named boolean flags, kept in insertion order, with the save/load of its `Team<N>BooleanFlags` block.
// WHY: objectives coordinate through these flags (an action sets one, a condition or an activation test reads it), so the table is
// mission logic and a lookup that misses a flag keeps an objective from ever completing. Flag IDs come from mission data and are written
// in different case by different authors, so identity is the case-insensitive ASCII fold of mc2::str, the same on every host and locale.
// LEAF: depends on FitIniFile, objective_text and the log facade only, so the unit tier links it without the game.
#pragma once

#include <string>
#include <string_view>
#include <vector>

class FitIniFile;

class CBooleanArray {
public:
	void Clear() { m_flags.clear(); }

	// Sets the flag, adding it when it is not in the table yet.
	void setElementValue(std::string_view id, bool value);

	// A flag that was never set reads false.
	bool getElementValue(std::string_view id) const;

	void save(long alignment, FitIniFile* file) const;

	// Appends the flags of the team's block; a missing block adds none.
	void load(long alignment, FitIniFile* file);

private:
	struct Flag {
		std::string id;
		bool value;
	};
	std::vector<Flag> m_flags;
};

// Logs once per run that a null part was handed to an objective list and dropped (a failed factory build reaches Append as null).
void logDroppedObjectivePart();
