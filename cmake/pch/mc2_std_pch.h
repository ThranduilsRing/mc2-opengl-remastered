// Precompiled header for the gameos, mclib and mc2 targets (option MC2_PCH).
//
// WHAT: the C and C++ standard-library headers that most translation units of those targets pull in directly or through a project header.
// WHY: parsing them once per target instead of once per translation unit is the whole point; nothing from this repository belongs here,
// because a header that changes would rebuild the target's precompiled header and, with it, every translation unit.
// Every header below is idempotent (include guards), so a TU that includes it again pays nothing.
#pragma once

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
