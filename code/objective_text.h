// objective_text.h -- text values of the objective model in mission FIT files and saves.
//
// WHAT: reads and writes the quoted string values of an objective (title, description, flag IDs, model name, file names) and converts the
// line breaks between the FIT form ("\n" inside the quotes) and the in-memory form ("\r\n").
// WHY: a missing key must not leak the text of the key read before it, so the reader reports it as an empty optional and the caller decides
// what absence means. A writer always writes its key (an empty value is a legal value) so a saved file never depends on whether a string
// was ever assigned.
// LEAF: depends on FitIniFile and the log facade only, so the unit tier links it without the game.
#pragma once

#include <optional>
#include <string>
#include <string_view>

class FitIniFile;

namespace objective_text {

// Longest value read back. A longer one is malformed content: it is logged and read as absent, never cut, because a cut flag ID or
// file name would be a different name.
inline constexpr unsigned long kMaxTextLength = 2000;

// The value of `key` in the current block, line breaks converted to "\r\n"; empty when the key is absent or over-long.
std::optional<std::string> read(FitIniFile& file, const char* key);

// A flag reference: the text of `idKey`, else the decimal number stored under `indexKey` (files from before flags had names).
// Empty when neither key is present.
std::optional<std::string> readFlagID(FitIniFile& file, const char* idKey, const char* indexKey);

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
//  - ActivateFlagID: readFlagID with ActivateFlagIndex; when that is absent too, `fields` keeps its value.
//  - ResetStatusFlagID: `fields` keeps its value.
//  - ObjectiveModel: empty, never the text of a key read before it.
void readFields(FitIniFile& file, Fields& fields);

// "\n" -> "\r\n" (every one) and "\r\n" -> "\n". All other bytes, including a lone trailing character, are kept.
std::string toMemoryBreaks(std::string_view fileText);
std::string toFileBreaks(std::string_view memoryText);

}  // namespace objective_text
