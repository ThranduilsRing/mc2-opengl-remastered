// code/logistics_csv.h
//
// Pure CSV field tokenizer for logistics component data (compbas.csv). Extracted
// (LOGISTICS-CSV-TOKENIZER-FIX-1) from LogisticsComponent::extractString/Int/
// Float so the parse has ONE definition that production and a game-free contract
// harness both call (tools/logistics_csv_harness/), and so the past-NUL overrun
// could be fixed once.
//
// Firewall: header-only, no GL, no game-side headers, no globals. Pure char*
// cursor ops over an in-memory line buffer.
//
// FIX vs the original:
//   * The original advanced `cursor += i + 1` even when the field ended on the
//     '\0' terminator, stepping ONE BYTE PAST the NUL. init() runs ~24 sequential
//     extracts; a malformed/truncated row (fewer fields, no trailing delimiter)
//     then walked the cursor into unallocated heap (OOB read). Here the cursor
//     parks ON the NUL when the field ends at end-of-line, so repeat calls return
//     empty and never read past the buffer.
//   * The original's `gosASSERT(i < bufferLength)` bounds check is a release
//     no-op; the copy is now clamped to outCap-1 (defensive — current callers
//     pass >=1024 buffers so well-formed parsing is byte-identical).
// Behavior on well-formed input is unchanged: fields end in ','/'\n' (advance
// past the delimiter exactly as before); the final-field park-vs-past-NUL
// difference is unobservable because init() stops after the last column.

#ifndef MC2_LOGISTICS_CSV_H
#define MC2_LOGISTICS_CSV_H

#include <cstdlib>   // atoi, atof
#include <cstring>   // memcpy

namespace logistics_csv {

// Max field scan length (legacy cap; an over-cap field returns 0 like the
// original's `return false`).
constexpr int kMaxFieldScan = 512;

// Copy one ','/'\n'/'\0'-delimited field from *cursor into out (NUL-terminated,
// clamped to outCap-1). Advances cursor PAST a ','/'\n' delimiter, but NEVER past
// the terminating '\0'. Returns the copied field length (0 = empty or over-cap).
inline int extractField(char*& cursor, char* out, int outCap) {
    if (out && outCap > 0) out[0] = '\0';
    int i = 0;
    for (; i < kMaxFieldScan; ++i) {
        const char c = cursor[i];
        if (c == '\n' || c == ',' || c == '\0') break;
    }
    if (i == kMaxFieldScan) return 0;          // field too long -> legacy false(0)
    int copy = i;
    if (outCap > 0 && copy > outCap - 1) copy = outCap - 1;  // real bound (no overrun)
    if (copy > 0) memcpy(out, cursor, static_cast<size_t>(copy));
    out[copy] = '\0';
    if (cursor[i] == '\0') cursor += i;        // park ON the NUL (never past it)
    else                   cursor += i + 1;    // step past the ','/'\n' delimiter
    return copy;
}

inline int extractInt(char*& cursor) {
    char buf[1024];
    const int n = extractField(cursor, buf, 1024);
    return n > 0 ? atoi(buf) : -1;
}

inline float extractFloat(char*& cursor) {
    char buf[1024];
    const int n = extractField(cursor, buf, 1024);
    return n > 0 ? static_cast<float>(atof(buf)) : -1.0f;
}

}  // namespace logistics_csv

#endif  // MC2_LOGISTICS_CSV_H
