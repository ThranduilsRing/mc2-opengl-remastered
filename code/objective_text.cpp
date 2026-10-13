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

}  // namespace

std::string toMemoryBreaks(std::string_view fileText) { return replaceAll(fileText, "\n", "\r\n"); }

std::string toFileBreaks(std::string_view memoryText) { return replaceAll(memoryText, "\r\n", "\n"); }

ReadStatus readChecked(FitIniFile& file, const char* key, std::string& value)
{
	value.clear();
	char buffer[kMaxTextLength + 1] = {};
	const long result = file.readIdString(key, buffer, sizeof(buffer));
	if (result == BUFFER_TOO_SMALL) {
		MC2_LOG(Io, Warn, "objective: the value of '{}' is longer than {} characters and is ignored", key, kMaxTextLength);
		return ReadStatus::TooLong;
	}
	if (result != NO_ERR) {
		return ReadStatus::Absent;
	}
	value = toMemoryBreaks(buffer);
	return ReadStatus::Read;
}

std::optional<std::string> read(FitIniFile& file, const char* key)
{
	std::string value;
	if (readChecked(file, key, value) != ReadStatus::Read) {
		return std::nullopt;
	}
	return value;
}

std::optional<std::string> readPlain(FitIniFile& file, const char* key, unsigned long maxLength)
{
	std::string buffer(maxLength + 1, '\0');
	const long result = file.readIdString(key, buffer.data(), static_cast<unsigned long>(buffer.size()));
	if (result == BUFFER_TOO_SMALL) {
		MC2_LOG(Io, Warn, "fit: the value of '{}' is longer than {} characters and is ignored", key, maxLength);
		return std::nullopt;
	}
	if (result != NO_ERR) {
		return std::nullopt;
	}
	return std::string(buffer.c_str());
}

std::optional<std::string> readFlagID(FitIniFile& file, const char* idKey, const char* indexKey)
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

long write(FitIniFile& file, const char* key, std::string_view text)
{
	const std::string fileText = toFileBreaks(text);
	return file.writeIdString(key, fileText.c_str());
}

}  // namespace objective_text
