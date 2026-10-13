// objective_flags.cpp -- see objective_flags.h.

#include "objective_flags.h"

#include "inifile.h"
#include "mc2str.h"
#include "objective_text.h"
#include "../GameOS/gameos/utils/mc2log.h"

#include <algorithm>
#include <format>

namespace {

// A predicate for the flag whose ID is `id`, ignoring ASCII case.
auto flagNamed(std::string_view id)
{
	return [id](const auto& flag) { return mc2::str::iequals(flag.id, id); };
}

}  // namespace

void CBooleanArray::setElementValue(std::string_view id, bool value)
{
	if (auto flag = std::ranges::find_if(m_flags, flagNamed(id)); flag != m_flags.end()) {
		flag->value = value;
	} else {
		m_flags.push_back({std::string(id), value});
	}
}

bool CBooleanArray::getElementValue(std::string_view id) const
{
	const auto flag = std::ranges::find_if(m_flags, flagNamed(id));
	return flag != m_flags.end() && flag->value;
}

void CBooleanArray::save(long alignment, FitIniFile* file) const
{
	file->writeBlock(std::format("Team{}BooleanFlags", alignment).c_str());
	for (size_t i = 0; i < m_flags.size(); ++i) {
		objective_text::write(*file, std::format("BooleanString{}", i).c_str(), m_flags[i].id);
		file->writeIdBoolean(std::format("BooleanValue{}", i).c_str(), m_flags[i].value);
	}
}

void CBooleanArray::load(long alignment, FitIniFile* file)
{
	if (file->seekBlock(std::format("Team{}BooleanFlags", alignment).c_str()) != NO_ERR) {
		return;
	}
	for (size_t i = 0;; ++i) {
		std::optional<std::string> id = objective_text::read(*file, std::format("BooleanString{}", i).c_str());
		if (!id) {
			return;
		}
		bool value = false;
		if (file->readIdBoolean(std::format("BooleanValue{}", i).c_str(), value) != NO_ERR) {
			MC2_LOG(Io, Warn, "objective: flag '{}' has no BooleanValue{}, read as false", *id, i);
		}
		m_flags.push_back({std::move(*id), value});
	}
}
