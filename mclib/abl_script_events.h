// WHAT: the log lines ABL writes when it rejects or clamps a script's content: a string cut to fit its char[N] variable, a builtin that
// was handed no output variable.
// WHY: malformed script content never crashes the engine; it is clamped or ignored with one greppable line, and
// the line stops after a few repeats so a looping script cannot flood the log. One place keeps the line format identical across the
// executor, the standard routines and the native bindings.
#pragma once

#include <cstddef>
#include <string_view>
#include <span>

// The declared script extent bounds every native array writer.
std::span<int> ABLi_popIntBuf(void);
std::span<float> ABLi_popRealBuf(void);

namespace abl {

// `[ABL v1] event=<event> module= line= capacity= dropped=`. `event` is the whole event name as a literal ("concat_truncated"); each name
// logs at most 16 lines between ABL session resets; unregistered names share one fallback budget.
void reportTruncated(std::string_view event, size_t capacity, size_t dropped);

// `[ABL v1] event=bad_argument builtin= argument= module= line=`, at most 16 lines between ABL session resets.
void reportBadArgument(std::string_view builtin, std::string_view argument);

long clampArrayCount(long count, size_t capacity, std::string_view event);
long copyIntArray(std::span<int> dest, std::span<const int> src, std::string_view event);
long copyRealArray(std::span<float> dest, std::span<const float> src, std::string_view event);

// A new ABL session needs fresh diagnostic budgets so earlier missions cannot silence malformed content.
// The session owner must reset them before loading or executing scripts.
void resetEventBudgets();

}  // namespace abl
