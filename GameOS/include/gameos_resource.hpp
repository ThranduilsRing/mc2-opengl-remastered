// gameos_resource.hpp -- the resource-string provider query beside gos_GetResourceString (gameos.hpp).
//
// gos_GetResourceString never fails: a missing id returns a printable placeholder, so a caller cannot tell a real string from the marker. The F4
// missing-id contract needs that answer without the marker, so this is the narrow query: does the provider hold a string for the id, and how long is it.
// It writes no log line and returns no marker; existing callers of gos_GetResourceString are unchanged.
#pragma once

#include "gameos.hpp"

#include <cstddef>
#include <optional>

// The length in bytes of the string the provider holds for `Id`, or nothing when it holds none.
std::optional<std::size_t> gos_ResourceStringLength(HSTRRES Handle, DWORD Id);
