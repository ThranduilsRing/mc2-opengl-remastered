// objective_text.cpp -- see objective_text.h.

#include "objective_text.h"

#include "inifile.h"
#include "../GameOS/gameos/utils/mc2log.h"

namespace objective_text {

namespace {

std::string replaceAll(std::string_view text, std::string_view from, std::string_view to)
{
	std::string result;
	result.reserve(text.size());
	size_t at = 0;
	for (size_t hit = text.find(from); hit != std::string_view::npos; hit = text.find(from, at)) {
		result.append(text, at, hit - at);
		result.append(to);
		at = hit + from.size();
	}
	result.append(text, at, std::string_view::npos);
	return result;
}

// The one FIT text read: the value of `key` up to its first NUL, at most `maxLength` characters, into `value` (empty unless the status is Read).
// WHY one function: classifying the reader's result and logging a malformed value must read the same for every caller.
ReadStatus readText(FitIniFile& file, mc2::czstring key, unsigned long maxLength, std::string& value)
{
	value.clear();
	std::string buffer(maxLength + 1, '\0');
	const long result = file.readIdString(key, buffer.data(), static_cast<unsigned long>(buffer.size()));
	if (result == BUFFER_TOO_SMALL) {
		MC2_LOG(Io, Warn, "fit: the value of '{}' is longer than {} characters and is ignored", key, maxLength);
		return ReadStatus::TooLong;
	}
	if (result != NO_ERR) {
		return ReadStatus::Absent;
	}
	value = buffer.c_str();
	return ReadStatus::Read;
}

}  // namespace

std::string toMemoryBreaks(std::string_view fileText) { return replaceAll(fileText, "\n", "\r\n"); }

std::string toFileBreaks(std::string_view memoryText) { return replaceAll(memoryText, "\r\n", "\n"); }

ReadStatus readChecked(FitIniFile& file, mc2::czstring key, std::string& value)
{
	const ReadStatus status = readText(file, key, kMaxTextLength, value);
	if (status == ReadStatus::Read) {
		value = toMemoryBreaks(value);
	}
	return status;
}

std::optional<std::string> read(FitIniFile& file, mc2::czstring key)
{
	std::string value;
	if (readChecked(file, key, value) != ReadStatus::Read) {
		return std::nullopt;
	}
	return value;
}

std::optional<std::string> readPlain(FitIniFile& file, mc2::czstring key, unsigned long maxLength)
{
	std::string value;
	if (readText(file, key, maxLength, value) != ReadStatus::Read) {
		return std::nullopt;
	}
	return value;
}

std::optional<std::string> readFlagID(FitIniFile& file, mc2::czstring idKey, mc2::czstring indexKey)
{
	if (std::optional<std::string> id = read(file, idKey)) {
		return id;
	}
	unsigned long index = 0;
	if (file.readIdULong(indexKey, index) == NO_ERR) {
		return std::to_string(static_cast<int>(index));
	}
	return std::nullopt;
}

void readFields(FitIniFile& file, Fields& fields)
{
	fields.title = read(file, "Title").value_or("");
	if (std::optional<std::string> description = read(file, "Description")) {
		fields.description = std::move(*description);
	} else {
		fields.description = fields.title;
	}
	if (std::optional<std::string> flagID = readFlagID(file, "ActivateFlagID", "ActivateFlagIndex")) {
		fields.activateFlagID = std::move(*flagID);
	}
	if (std::optional<std::string> flagID = read(file, "ResetStatusFlagID")) {
		fields.resetStatusFlagID = std::move(*flagID);
	}
	fields.modelName = read(file, "ObjectiveModel").value_or("");
}

long write(FitIniFile& file, mc2::czstring key, std::string_view text)
{
	const std::string fileText = toFileBreaks(text);
	return file.writeIdString(key, fileText.c_str());
}

}  // namespace objective_text
