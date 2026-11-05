// mc2_czstring.h - counted alias for NUL-terminated read-only C-string parameters.
//
// Contract: a parameter typed mc2::czstring MUST point at a NUL-terminated string (or be
// nullptr where the callee documents that). Use it when the value is forwarded to a C API
// (fopen, strcmp, strcpy, printf family, Win32, ABL tables) or stored as a C string.
// Use std::string_view for read-only text that is only compared, hashed or copied by length;
// never std::string_view + .data() into a C API (the view is not NUL-terminated).
// cxx17_ratchet LG06 does not count this alias: it is a contract, not residue.
#pragma once

namespace mc2 {
using czstring = const char*;
}
