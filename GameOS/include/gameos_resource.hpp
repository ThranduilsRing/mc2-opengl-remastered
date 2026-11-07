// gameos_resource.hpp -- the resource-string provider query beside gos_GetResourceString (gameos.hpp).
//
// gos_GetResourceString never fails: a missing id returns a printable placeholder, so a caller cannot tell a real string from the marker. The F4
// missing-id contract needs that answer without the marker, so this is the narrow query: does the provider hold a string for the id, and how long is it.
// It writes no log line and returns no marker; existing callers of gos_GetResourceString are unchanged.
#pragma once

#include "gameos.hpp"

#include "../gameos/MC2Strings.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>

// The typed message for a catalog id (see MC2Strings.h): the template is compiled once per catalog revision and checked against `signature`.
// A bad override falls back to the stock text. Callers use mclib/resource_messages.h, not this directly.
std::string gos_FormatResourceMessage(HSTRRES Handle, DWORD Id, std::span<const MC2Strings::ArgKind> signature, std::span<const MC2Strings::MessageArg> args);

// The length in bytes of the string the provider holds for `Id`, or nothing when it holds none.
std::optional<std::size_t> gos_ResourceStringLength(HSTRRES Handle, DWORD Id);
