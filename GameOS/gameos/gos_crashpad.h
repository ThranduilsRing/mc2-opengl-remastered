#pragma once
// Out-of-process crash minidumps (sentry-native, crashpad backend, no transport, no DSN: nothing leaves the machine).
// crashpad_handler(.exe) runs as a child of the game and writes <exe_dir>/crashes/crashpad/pending/<guid>.dmp when the
// game faults. Windows: gos_crashbundle.cpp chains crashpad's unhandled-exception filter and keeps MiniDumpWriteDump
// as the fallback when the handler did not start. Linux: crashpad installs the signal handlers itself.
#include <cstddef>

// Idempotent. Returns true when the handler child is running and the crash clients are installed.
bool crashpad_start();
bool crashpad_active();
// Writes the newest .dmp under <db>/pending (or <db>/completed) into out; false when there is none.
bool crashpad_newest_dump(char* out, size_t cap);
// The crashpad database directory ("" until crashpad_start ran).
const char* crashpad_db_dir();
