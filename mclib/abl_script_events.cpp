// WHAT: the ABL script-content log lines (abl_script_events.h).
// WHY: see the header.
#include "abl_script_events.h"

#include "abl.h"
#include "mc2fmt.h"

#include <map>

extern int execLineNumber;
extern ABLModulePtr CurModule;

namespace {

constexpr int kMaxLinesPerEvent = 16;

// True while this event still has room in its log budget, and counts the line.
bool takeLine(std::string_view event) {
	static std::map<std::string_view, int> s_lines;
	int& lines = s_lines[event];
	if (lines >= kMaxLinesPerEvent)
		return false;
	++lines;
	return true;
}

}  // namespace

namespace abl {

void reportTruncated(std::string_view event, size_t capacity, size_t dropped) {
	if (!takeLine(event))
		return;
	mc2::fmt::print_err("[ABL v1] event={} module={} line={:d} capacity={:d} dropped={:d}\n", event,
	                    mc2::fmt::safe(CurModule ? CurModule->getName() : nullptr), mc2::fmt::as_i32(execLineNumber),
	                    mc2::fmt::as_size(capacity), mc2::fmt::as_size(dropped));
}

void reportBadArgument(std::string_view builtin, std::string_view argument) {
	if (!takeLine("bad_argument"))
		return;
	mc2::fmt::print_err("[ABL v1] event=bad_argument builtin={} argument={} module={} line={:d}\n", builtin, argument,
	                    mc2::fmt::safe(CurModule ? CurModule->getName() : nullptr), mc2::fmt::as_i32(execLineNumber));
}

}  // namespace abl
