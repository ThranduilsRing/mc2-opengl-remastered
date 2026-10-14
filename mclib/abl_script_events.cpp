// WHAT: the ABL script-content log lines (abl_script_events.h).
// WHY: see the header.
#include "abl_script_events.h"

#include "abl.h"
#include "mc2fmt.h"

#include <algorithm>
#include <array>

extern int execLineNumber;
extern ABLModulePtr CurModule;

namespace {

constexpr int kMaxLinesPerEvent = 16;

enum class Event : size_t {
    BadArgument,
    Assign,
    ArrayParam,
    Concat,
    Getmodule,
    Return,
    Getcameraposition,
    Getcameragoalposition,
    Getcamerarotation,
    Getcameragoalrotation,
    Getcameravelocity,
    Getcameragoalvelocity,
    Convertcoords,
    Getnextpilotevent,
    Objectstatuscount,
    Getobjects,
    Getrelativepositiontopoint,
    Getrelativepositiontoobject,
    Getcontacts,
    Getweaponsready,
    Getweaponslocked,
    Getweaponsinrange,
    Getweaponranges,
    Getobjectposition,
    Getalarmtriggers,
    Getfireranges,
    Getattackers,
    Sortweapons,
    Getunitmates,
    Gettacorder,
    Getlasttacorder,
    Getrelativepositiontotarget,
    Getweapons,
    Getweaponsstatus,
    Getmapinfo,
    Getsalvage,
    Other,
    Count
};

constexpr std::array<std::string_view, static_cast<size_t>(Event::Count)> eventNames{
    "bad_argument",
    "assign_truncated",
    "array_param_truncated",
    "concat_truncated",
    "getmodule_truncated",
    "return_truncated",
    "getcameraposition_truncated",
    "getcameragoalposition_truncated",
    "getcamerarotation_truncated",
    "getcameragoalrotation_truncated",
    "getcameravelocity_truncated",
    "getcameragoalvelocity_truncated",
    "convertcoords_truncated",
    "getnextpilotevent_truncated",
    "objectstatuscount_truncated",
    "getobjects_truncated",
    "getrelativepositiontopoint_truncated",
    "getrelativepositiontoobject_truncated",
    "getcontacts_truncated",
    "getweaponsready_truncated",
    "getweaponslocked_truncated",
    "getweaponsinrange_truncated",
    "getweaponranges_truncated",
    "getobjectposition_truncated",
    "getalarmtriggers_truncated",
    "getfireranges_truncated",
    "getattackers_truncated",
    "sortweapons_truncated",
    "getunitmates_truncated",
    "gettacorder_truncated",
    "getlasttacorder_truncated",
    "getrelativepositiontotarget_truncated",
    "getweapons_truncated",
    "getweaponsstatus_truncated",
    "getmapinfo_truncated",
    "getsalvage_truncated",
    ""
};
std::array<int, static_cast<size_t>(Event::Count)> linesPerEvent{};

Event eventKey(std::string_view name) {
    const auto found = std::find(eventNames.begin(), eventNames.end(), name);
    return found == eventNames.end() ? Event::Other : static_cast<Event>(found - eventNames.begin());
}

// A finite per-event budget keeps looping scripts from flooding diagnostics.
bool takeLine(Event event) {
	int& lines = linesPerEvent[static_cast<size_t>(event)];
	if (lines >= kMaxLinesPerEvent)
		return false;
	++lines;
	return true;
}

}  // namespace

namespace abl {

long clampArrayCount(long count, size_t capacity, std::string_view event) {
	if (count <= 0) return 0;
	const auto written = std::min(static_cast<size_t>(count), capacity);
	if (written < static_cast<size_t>(count))
		reportTruncated(event, capacity, static_cast<size_t>(count) - written);
	return static_cast<long>(written);
}

long copyIntArray(std::span<int> dest, std::span<const int> src, std::string_view event) {
	const auto written = clampArrayCount(static_cast<long>(src.size()), dest.size(), event);
	if (written) std::copy_n(src.begin(), written, dest.begin());
	return written;
}

long copyRealArray(std::span<float> dest, std::span<const float> src, std::string_view event) {
	const auto written = clampArrayCount(static_cast<long>(src.size()), dest.size(), event);
	if (written) std::copy_n(src.begin(), written, dest.begin());
	return written;
}


#ifdef MC2_ABL_EVENTS_TEST
void resetEventBudgetsForTest() {
    linesPerEvent.fill(0);
}
#endif

void reportTruncated(std::string_view event, size_t capacity, size_t dropped) {
	if (!takeLine(eventKey(event)))
		return;
	mc2::fmt::print_err("[ABL v1] event={} module={} line={:d} capacity={:d} dropped={:d}\n", event,
	                    mc2::fmt::safe(CurModule ? CurModule->getName() : nullptr), mc2::fmt::as_i32(execLineNumber),
	                    mc2::fmt::as_size(capacity), mc2::fmt::as_size(dropped));
}

void reportBadArgument(std::string_view builtin, std::string_view argument) {
	if (!takeLine(Event::BadArgument))
		return;
	mc2::fmt::print_err("[ABL v1] event=bad_argument builtin={} argument={} module={} line={:d}\n", builtin, argument,
	                    mc2::fmt::safe(CurModule ? CurModule->getName() : nullptr), mc2::fmt::as_i32(execLineNumber));
}

}  // namespace abl
