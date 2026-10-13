// objective_text.h -- text values of the objective model in mission FIT files and saves.
//
// WHAT: reads and writes the quoted string values of an objective (title, description, flag IDs, model name, file names) and converts the
// line breaks between the FIT form ("\n" inside the quotes) and the in-memory form ("\r\n").
// WHY: the old readers left their output variable untouched when a key was missing, and every caller reused one temporary for the next key,
// so a missing Description showed the previous key's text and a missing ObjectiveModel took a flag ID. A missing key is now an empty
// optional, the caller decides what it means, and a writer always writes its key (an empty value is a legal value) so a saved file never
// depends on whether a string was ever assigned.
// LEAF: depends on FitIniFile and the log facade only, so the unit tier links it without the game.
#pragma once

#include <optional>
#include <string>
#include <string_view>

class FitIniFile;

namespace objective_text {

// Longest value read back; a longer one is cut and logged (it is malformed content, never a crash).
inline constexpr unsigned long kMaxTextLength = 2000;

// The value of `key` in the current block, line breaks converted to "\r\n"; empty when the key is absent.
std::optional<std::string> read(FitIniFile& file, const char* key);

// Writes `key = "text"` with line breaks stored as "\n". Always writes the key. Returns the writer's result.
long write(FitIniFile& file, const char* key, std::string_view text);

// The text fields of one objective block (Title, Description, ActivateFlagID, ResetStatusFlagID, ObjectiveModel).
struct Fields {
	std::string title;
	std::string description;
	std::string activateFlagID;
	std::string resetStatusFlagID;
	std::string modelName;
};

// Reads the five fields of the current block into `fields`. The rule for each missing key is written here and nowhere else:
//  - Title: empty.
//  - Description: the Title. Older mission files carry only a title, so the objective shows it.
//  - ActivateFlagID: the decimal ActivateFlagIndex of files from before flags had names; when that is absent too, `fields` keeps its value.
//  - ResetStatusFlagID: `fields` keeps its value.
//  - ObjectiveModel: empty, never the text of a key read before it.
void readFields(FitIniFile& file, Fields& fields);

// "\n" -> "\r\n" (every one) and "\r\n" -> "\n". All other bytes, including a lone trailing character, are kept.
std::string toMemoryBreaks(std::string_view fileText);
std::string toFileBreaks(std::string_view memoryText);

}  // namespace objective_text
