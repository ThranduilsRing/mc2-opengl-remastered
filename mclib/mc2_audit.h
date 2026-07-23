#pragma once
//===========================================================================//
// mclib/mc2_audit.h — META-FIX INFRA-1: gated audit emitter.
//
// One tiny helper shared by every recurrence-proofing design (META-FIX plan
// 2026-07-16): gated, machine-parseable stdout marker lines. Lives in mclib
// so mc2 / EditRel / mc2_launcher all link it.
//
// NO grammar is imposed here — each design's reviewed line format
// ("[MOVER_COHERENCE] VIOLATION rule=...", "[ASSET_LOAD v1] event=...",
// "[SPLIT_PATH_PARITY] ...", "[APPEAR_AUDIT v1] ...", "[ORDER_FIXTURE] ...",
// "ORDER_LIVENESS_SUMMARY ...") is passed in pre-formatted; those grammars
// are contract, do not normalize them.
//
// Leaf TU: no engine includes, no GameOS dependency. The optional diagnostic
// JSONL mirror is a REGISTERED SINK (GameOS registers one right after
// mc2_diag::init in gos_CreateRenderer) so binaries that do not link
// diagnostic_trace.cpp (e.g. tools) still link this TU cleanly.
//
// NOT thread-safe (same contract as the other per-subsystem trace latches);
// callers emit from the main/render thread.
//===========================================================================//

// Static-getenv latch. Call-site pattern:
//     static bool on = mc2AuditLatch("MC2_FOO_AUDIT");
//     if (on) { ... }
// Gate-off path is one bool test — byte-identical behavior when the var is
// unset (proven pattern, .claude/ISSUE-FIXES-STATUS.md gate-off byte-identity).
// True iff the env var is set, non-empty, and not "0".
bool mc2AuditLatch(const char* envVar);

// Emit one marker line to stdout (appends '\n', flushes so smoke runners see
// it before a crash) AND mirrors the raw line to the registered sink, if any.
void mc2AuditLine(const char* line);

// printf-style convenience over mc2AuditLine (formatted line capped at 1024).
void mc2AuditLinef(const char* fmt, ...);

// Bounded dedup for per-(entity,rule) / per-path rate limiting.
// Returns true the FIRST time (tag,key) is seen — "emit now" — and false on
// every repeat. Per-tag capacity is 64 keys (kLogLineCap precedent,
// GameOS/gameos/mc2_verify.cpp): once a tag's set is full, unseen keys also
// return false, so a hot loop cannot flood stdout.
bool mc2AuditOnce(const char* tag, unsigned long long key);

// Optional diagnostic mirror (weak coupling — see header comment). The sink
// receives the raw marker line (no trailing newline). Pass nullptr to clear.
typedef void (*Mc2AuditSink)(const char* line);
void mc2AuditSetSink(Mc2AuditSink sink);
