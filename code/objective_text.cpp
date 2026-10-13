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

std::optional<std::string> read(FitIniFile& file, const char* key)
{
	char buffer[kMaxTextLength + 1] = {};
	const long result = file.readIdString(key, buffer, kMaxTextLength);
	if (result == BUFFER_TOO_SMALL) {
		// The reader fills the whole buffer without a terminator in this case; the zeroed last byte supplies it.
		MC2_LOG(Io, Warn, "objective: the value of '{}' is longer than {} characters and was cut", key, kMaxTextLength);
	} else if (result != NO_ERR) {
		return std::nullopt;
	}
	return toMemoryBreaks(buffer);
}

void readFields(FitIniFile& file, Fields& fields)
{
	fields.title = read(file, "Title").value_or("");
	if (std::optional<std::string> description = read(file, "Description")) {
		fields.description = std::move(*description);
	} else {
		fields.description = fields.title;
	}
	if (std::optional<std::string> flagID = read(file, "ActivateFlagID")) {
		fields.activateFlagID = std::move(*flagID);
	} else {
		unsigned long flagIndex = 0;
		if (file.readIdULong("ActivateFlagIndex", flagIndex) == NO_ERR) {
			fields.activateFlagID = std::to_string(static_cast<int>(flagIndex));
		}
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
