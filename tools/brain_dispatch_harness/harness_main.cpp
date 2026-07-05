// BRAIN-DISPATCH-HARNESS-V2: offline dispatch harness main.
//
// V1 (BRAIN-DISPATCH-HARNESS-1): parse + bodyHasX + TraceOnly — 15 checks.
// V2 (BRAIN-DISPATCH-HARNESS-V2): adds Apply A/B effect-identity mode:
//   --apply-mode: run executeSpecialBody_Apply + commitBrainIntents per fixture,
//   capture TacOrderSink contents, assert apply_expectation from manifest.
//   Gate state (OFF/ON) is determined by MC2_BRAIN_INTENT_QUEUE env var at startup.
//   Run twice (gate-OFF then gate-ON) from scripts/run_brain_apply_ab.sh to prove
//   A/B identity: gate-OFF orders == gate-ON committed orders.
//
// V1 mode (default, no --apply-mode):
//   Same as BRAIN-DISPATCH-HARNESS-1: 15 parse/bodyHasX/TraceOnly checks.
//   Back-compat: v1 15/15 always pass; v2 is additive.
//
// CLI: brain_dispatch_harness --manifest <path> [--fixture-dir <dir>] [--json] [--apply-mode]
//
// Exit codes: 0 = all pass, 1 = one or more FAIL, 2 = manifest unreadable.

// Make sure stubs win over engine headers
#include "stubs/include/objmgr.h"
#include "stubs/include/warrior.h"
#include "stubs/include/tacordr.h"
#include "stubs/include/gameobj.h"
#include "stubs/include/inifile.h"

// V2 new stubs
#include "stubs/tac_order_sink.h"
#include "stubs/recording_warrior.h"
#include "stubs/configurable_objmgr.h"

// Real dispatch API (code/brain_special_dispatch.h — LEAF, no engine deps)
#include "brain_special_dispatch.h"
#include "mech_brain_runtime.h"
#include "fitblockwriter.h"   // FITBLOCK-WRITER-1: writer round-trip self-test

// BRAINSPECIAL-FLOW-WAIT-1: settable brain-time (stubs/brain_tick_stub.cpp).
extern void harnessSetBrainTimeMs(uint32_t ms);

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <io.h>      // _dup, _dup2, _close, _fileno
#include <cstdlib>   // _putenv
#endif

// ---------------------------------------------------------------------------
// Minimal JSON parser — enough for our manifest schema.
// ---------------------------------------------------------------------------

struct JsonValue;
using JsonObj  = std::map<std::string, JsonValue>;
using JsonArr  = std::vector<JsonValue>;

struct JsonValue {
    enum Kind { Null, Bool, Number, Str, Array, Object } kind = Null;
    bool        bval = false;
    double      nval = 0.0;
    std::string sval;
    JsonArr     aval;
    JsonObj     oval;

    bool isBool()   const { return kind == Bool; }
    bool isStr()    const { return kind == Str; }
    bool isArr()    const { return kind == Array; }
    bool isObj()    const { return kind == Object; }
    bool isBoolTrue() const { return kind == Bool && bval; }
    const std::string& str() const { return sval; }
    const JsonArr&     arr() const { return aval; }
    const JsonObj&     obj() const { return oval; }
    bool has(const std::string& k) const { return kind == Object && oval.count(k); }
    const JsonValue& at(const std::string& k) const {
        static JsonValue null{};
        auto it = oval.find(k);
        return it != oval.end() ? it->second : null;
    }
};

struct JsonParser {
    const char* p;
    const char* end;
    std::string error;

    JsonParser(const std::string& s) : p(s.c_str()), end(s.c_str() + s.size()) {}

    void skipWs() { while (p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p; }

    bool expect(char c) { skipWs(); if (p < end && *p == c) { ++p; return true; } return false; }

    std::string parseStr() {
        if (!expect('"')) { error = "expected '\"'"; return ""; }
        std::string s;
        while (p < end && *p != '"') {
            if (*p == '\\') { ++p; if (p<end) s += *p++; }
            else s += *p++;
        }
        if (p < end) ++p; // closing "
        return s;
    }

    JsonValue parseValue() {
        skipWs();
        if (p >= end) return {};
        if (*p == '"') {
            JsonValue v; v.kind = JsonValue::Str; v.sval = parseStr(); return v;
        }
        if (*p == '{') {
            ++p;
            JsonValue v; v.kind = JsonValue::Object;
            skipWs();
            while (p < end && *p != '}') {
                std::string key = parseStr();
                expect(':');
                v.oval[key] = parseValue();
                skipWs();
                if (p < end && *p == ',') { ++p; skipWs(); }
            }
            if (p < end) ++p;
            return v;
        }
        if (*p == '[') {
            ++p;
            JsonValue v; v.kind = JsonValue::Array;
            skipWs();
            while (p < end && *p != ']') {
                v.aval.push_back(parseValue());
                skipWs();
                if (p < end && *p == ',') { ++p; skipWs(); }
            }
            if (p < end) ++p;
            return v;
        }
        if (std::strncmp(p, "true", 4) == 0)  { p+=4; JsonValue v; v.kind=JsonValue::Bool; v.bval=true;  return v; }
        if (std::strncmp(p, "false", 5) == 0) { p+=5; JsonValue v; v.kind=JsonValue::Bool; v.bval=false; return v; }
        if (std::strncmp(p, "null", 4) == 0)  { p+=4; JsonValue v; v.kind=JsonValue::Null; return v; }
        // number
        char* np = nullptr;
        double d = std::strtod(p, &np);
        if (np != p) { JsonValue v; v.kind=JsonValue::Number; v.nval=d; p=np; return v; }
        {
            char ebuf[128];
            std::snprintf(ebuf, sizeof(ebuf), "unexpected char 0x%02x '%c' at offset %d",
                          (unsigned char)*p, (*p >= 32 ? *p : '?'), (int)(p - (end - (end-p))));
            error = ebuf;
        }
        return {};
    }
};

static JsonValue parseJsonFile(const std::string& path, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return {}; }
    std::fseek(f, 0, SEEK_END);
    long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string content(fsize, '\0');
    std::fread(&content[0], 1, fsize, f);
    std::fclose(f);
    JsonParser jp(content);
    JsonValue v = jp.parseValue();
    err = jp.error;
    return v;
}

// ---------------------------------------------------------------------------
// stderr capture helpers
// ---------------------------------------------------------------------------

static std::string captureStderr(const std::function<void()>& fn) {
    static int s_captureId = 0;
    char tmpName[64];
    std::snprintf(tmpName, sizeof(tmpName), "_harness_stderr_%d.tmp", ++s_captureId);

#ifdef _WIN32
    int oldStderr = _dup(2);
    if (oldStderr < 0) { fn(); return ""; }
    FILE* tmp = std::fopen(tmpName, "w+b");
    if (!tmp) { _close(oldStderr); fn(); return ""; }
    if (_dup2(_fileno(tmp), 2) < 0) {
        std::fclose(tmp); _close(oldStderr); fn(); return "";
    }
    fn();
    _fflush_nolock(stderr);
    std::fflush(stderr);
    _dup2(oldStderr, 2);
    _close(oldStderr);
    std::rewind(tmp);
    std::string result;
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), tmp))
        result += buf;
    std::fclose(tmp);
    std::remove(tmpName);
    return result;
#else
    int oldStderr = dup(2);
    if (oldStderr < 0) { fn(); return ""; }
    FILE* tmp = fopen(tmpName, "w+b");
    if (!tmp) { close(oldStderr); fn(); return ""; }
    if (dup2(fileno(tmp), 2) < 0) {
        fclose(tmp); close(oldStderr); fn(); return "";
    }
    fn();
    fflush(stderr);
    dup2(oldStderr, 2);
    close(oldStderr);
    rewind(tmp);
    std::string result;
    char buf[1024];
    while (fgets(buf, sizeof(buf), tmp))
        result += buf;
    fclose(tmp);
    remove(tmpName);
    return result;
#endif
}

// ---------------------------------------------------------------------------
// V2: Apply expectation
// ---------------------------------------------------------------------------

// Object table role for ATTACK guard tests.
enum class AttackRole {
    None,           // no object in table (absent WID → bad-wid)
    EnemyTarget,    // different team from warrior → should produce ATTACK_OBJECT
    SelfTarget,     // same WID as warrior → ATTACK_SELF guard fires
    FriendlyTarget, // same team as warrior → ATTACK_FRIENDLY guard fires
};

struct ApplyExpectation {
    bool          hasExpectation     = false;

    // HARNESS-INTENT-GATE-SCOPE-1: fixture's order emission REQUIRES the intent queue
    // (e.g. patrol — emits only via emitBrainIntent, no direct setGeneralTacOrder fallback).
    // When true, apply assertions are SKIPPED in a gate-OFF process (the order legitimately
    // cannot be emitted without the queue), keeping the A/B gate-OFF control meaningful for
    // the 6 dual-path effect verbs.
    bool          requiresIntentQueue = false;

    // Expected TacticalOrderCode for gate-OFF and gate-ON (must match for A/B identity).
    // -1 means "no order expected" (guards fire).
    int           expectedOrderCode  = -1;  // TACTICAL_ORDER_* or -1
    int           expectedCount      = 1;   // expected number of orders in sink (0 or 1)
    long          expectedTargetWID  = 0;   // for ATTACK_OBJECT

    // MOVETO: expected waypoint (approximate comparison).
    bool          checkWaypoint      = false;
    float         expectedX = 0, expectedY = 0, expectedZ = 0;

    // ATTACK guard scenario: how to configure the fake object table.
    AttackRole    attackRole         = AttackRole::None;
    long          attackFakeObjWID   = 0;   // WID of the fake object to put in table

    // Once-guard: body fires exactly once per run.
    // (proven by expectedCount=1 even if body has the verb)

    // BRAIN-OPORD-COREPATROL-1: apply-path trace substring checks.
    // Checked against ar.traceOutput (Apply stderr capture), NOT TraceOnly.
    // Use this for verbs whose trace is emitted by executeSpecialBody_Apply only.
    std::vector<std::string> applyTraceSubstrings;
};

// ---------------------------------------------------------------------------
// Fixture entry (V1 + V2)
// ---------------------------------------------------------------------------
struct FixtureEntry {
    std::string name;
    std::string file;
    std::string mission;
    bool        partial = false;
    // V1 assertions
    bool expectedLoaded               = true;
    bool expectedBodyHasPowerdown     = false;
    bool expectedBodyHasUnitEject     = false;
    bool expectedBodyHasCoreGuard     = false;
    bool expectedBodyHasCoreMoveTo    = false;
    bool expectedBodyHasCoreAttack    = false;
    bool expectedBodyHasUnitRetreat   = false;
    bool expectedBodyHasEffect        = false;
    std::vector<std::string> expectedTraceSubstrings;

    // V2 apply expectation
    ApplyExpectation applyExp;

    // BRAIN-FSM-1K-A: fsm_sequential fixture type.
    // When true, harness runs ALL TechSpecial bodies in the index sequentially
    // on one FsmMechWarrior (so state persists across bodies).
    // Trace substrings are checked against the combined Apply trace output.
    bool fsmSequential = false;

    // BRAINSPECIAL-FLOW-WAIT-1: flow_sequential fixture type.
    // When true, harness runs ALL index bodies on one persistent FsmMechWarrior for
    // THREE passes with the stub brain time advanced 0 -> 1000 -> 6000 ms (so a
    // "WAIT 5" arms on pass 1 and latches open on pass 3). Trace substrings are
    // checked against the combined 3-pass Apply trace; apply_expectation.expected_count
    // is asserted against the warrior's total committed order count.
    bool flowSequential = false;
};

static ApplyExpectation applyExpFromJson(const JsonValue& jv) {
    ApplyExpectation e;
    if (!jv.isObj()) return e;
    e.hasExpectation = true;

    if (jv.has("expected_order_code")) {
        const std::string& s = jv.at("expected_order_code").str();
        if (s == "TACTICAL_ORDER_POWERDOWN")     e.expectedOrderCode = (int)TACTICAL_ORDER_POWERDOWN;
        else if (s == "TACTICAL_ORDER_EJECT")    e.expectedOrderCode = (int)TACTICAL_ORDER_EJECT;
        else if (s == "TACTICAL_ORDER_GUARD")    e.expectedOrderCode = (int)TACTICAL_ORDER_GUARD;
        else if (s == "TACTICAL_ORDER_MOVETO_POINT") e.expectedOrderCode = (int)TACTICAL_ORDER_MOVETO_POINT;
        else if (s == "TACTICAL_ORDER_ATTACK_OBJECT") e.expectedOrderCode = (int)TACTICAL_ORDER_ATTACK_OBJECT;
        else if (s == "TACTICAL_ORDER_WITHDRAW") e.expectedOrderCode = (int)TACTICAL_ORDER_WITHDRAW;
        else if (s == "NONE")                    e.expectedOrderCode = -1;
    }
    if (jv.has("expected_count")) e.expectedCount = (int)jv.at("expected_count").nval;
    if (jv.has("expected_target_wid")) e.expectedTargetWID = (long)jv.at("expected_target_wid").nval;

    if (jv.has("expected_waypoint")) {
        const JsonValue& wp = jv.at("expected_waypoint");
        if (wp.isArr() && wp.arr().size() >= 3) {
            e.checkWaypoint = true;
            e.expectedX = (float)wp.arr()[0].nval;
            e.expectedY = (float)wp.arr()[1].nval;
            e.expectedZ = (float)wp.arr()[2].nval;
        }
    }

    if (jv.has("attack_role")) {
        const std::string& r = jv.at("attack_role").str();
        if (r == "enemy")    e.attackRole = AttackRole::EnemyTarget;
        else if (r == "self")     e.attackRole = AttackRole::SelfTarget;
        else if (r == "friendly") e.attackRole = AttackRole::FriendlyTarget;
        else if (r == "absent")   e.attackRole = AttackRole::None;
    }
    if (jv.has("attack_fake_obj_wid")) e.attackFakeObjWID = (long)jv.at("attack_fake_obj_wid").nval;

    // BRAIN-OPORD-COREPATROL-1: apply-path trace substring checks.
    if (jv.has("apply_trace_substrings")) {
        for (const JsonValue& sv : jv.at("apply_trace_substrings").arr())
            e.applyTraceSubstrings.push_back(sv.str());
    }

    // HARNESS-INTENT-GATE-SCOPE-1: intent-queue-only fixtures (patrol) opt out of the gate-OFF check.
    if (jv.has("requires_intent_queue"))
        e.requiresIntentQueue = jv.at("requires_intent_queue").isBoolTrue();

    return e;
}

static FixtureEntry entryFromJson(const JsonValue& jv) {
    FixtureEntry e;
    if (jv.has("name"))    e.name    = jv.at("name").str();
    if (jv.has("file"))    e.file    = jv.at("file").str();
    if (jv.has("mission")) e.mission = jv.at("mission").str();
    if (jv.has("partial")) e.partial = jv.at("partial").isBoolTrue();

    if (jv.has("expected_bodyHasPowerdown"))  e.expectedBodyHasPowerdown  = jv.at("expected_bodyHasPowerdown").isBoolTrue();
    if (jv.has("expected_bodyHasUnitEject"))  e.expectedBodyHasUnitEject  = jv.at("expected_bodyHasUnitEject").isBoolTrue();
    if (jv.has("expected_bodyHasCoreGuard"))  e.expectedBodyHasCoreGuard  = jv.at("expected_bodyHasCoreGuard").isBoolTrue();
    if (jv.has("expected_bodyHasCoreMoveTo")) e.expectedBodyHasCoreMoveTo = jv.at("expected_bodyHasCoreMoveTo").isBoolTrue();
    if (jv.has("expected_bodyHasCoreAttack")) e.expectedBodyHasCoreAttack = jv.at("expected_bodyHasCoreAttack").isBoolTrue();
    if (jv.has("expected_bodyHasUnitRetreat"))e.expectedBodyHasUnitRetreat= jv.at("expected_bodyHasUnitRetreat").isBoolTrue();
    if (jv.has("expected_bodyHasEffect"))     e.expectedBodyHasEffect     = jv.at("expected_bodyHasEffect").isBoolTrue();

    if (jv.has("expected_trace_substrings")) {
        for (const JsonValue& sv : jv.at("expected_trace_substrings").arr())
            e.expectedTraceSubstrings.push_back(sv.str());
    }

    if (jv.has("apply_expectation"))
        e.applyExp = applyExpFromJson(jv.at("apply_expectation"));

    // BRAIN-FSM-1K-A: fsm_sequential flag.
    if (jv.has("fsm_sequential"))
        e.fsmSequential = jv.at("fsm_sequential").isBoolTrue();

    // BRAINSPECIAL-FLOW-WAIT-1: flow_sequential flag.
    if (jv.has("flow_sequential"))
        e.flowSequential = jv.at("flow_sequential").isBoolTrue();

    return e;
}

// ---------------------------------------------------------------------------
// V2: Run Apply + Commit for a fixture, capture sink contents.
// warrior.warriorWID = 1, warrior.warriorTeam = &s_warriorTeam
// Returns: trace captured, sink populated.
// ---------------------------------------------------------------------------

// Shared team instances for guard testing.
static Team s_warriorTeam;    // warrior's own team
static Team s_enemyTeam;      // distinct team (different pointer)

static ConfigurableGameObjectManager s_configObjMgr;
static TacOrderSink                  s_sink;

struct ApplyRunResult {
    TacOrderSink sink;
    std::string  traceOutput;
    bool         applyReturnedTrue; // executeSpecialBody_Apply return value
};

static ApplyRunResult runApply(const BrainSpecialBody& body, const SpecialIndex& index,
                               const ApplyExpectation& exp, bool intentQueueGateIsOn) {
    ApplyRunResult res;

    // Configure object manager based on attack_role.
    s_configObjMgr.clear();
    static FakeGameObject s_fakeObj1, s_fakeObj2;

    if (exp.attackRole != AttackRole::None && exp.attackFakeObjWID > 0) {
        long wid = exp.attackFakeObjWID;
        FakeGameObject* obj = &s_fakeObj1;
        obj->fakeWID = wid;
        if (exp.attackRole == AttackRole::FriendlyTarget)
            obj->fakeTeam = &s_warriorTeam;   // same team = friendly
        else if (exp.attackRole == AttackRole::SelfTarget)
            obj->fakeTeam = &s_warriorTeam;   // same team for self (WID check fires first)
        else // EnemyTarget
            obj->fakeTeam = &s_enemyTeam;     // different team = enemy
        s_configObjMgr.addObject(wid, obj);
    }
    ObjectManager = &s_configObjMgr;

    // Set up recording warrior.
    RecordingMechWarrior warrior;
    warrior.warriorWID  = 1;
    warrior.warriorTeam = &s_warriorTeam;

    // For SelfTarget: the fake obj WID must match warrior WID.
    if (exp.attackRole == AttackRole::SelfTarget)
        warrior.warriorWID = exp.attackFakeObjWID;

    warrior.resetRuntime();

    // Set up sink.
    s_sink.clear();
    g_activeSink = &s_sink;

    res.traceOutput = captureStderr([&]() {
        res.applyReturnedTrue = executeSpecialBody_Apply(body, &warrior, (int)warrior.warriorWID,
                                                         nullptr,  // varStore
                                                         &index,
                                                         nullptr); // callerKey

        // commitBrainIntents only if gate is ON.
        // Gate is determined by MC2_BRAIN_INTENT_QUEUE env var (static inside dispatch.cpp).
        // When gate is ON, runtime != nullptr inside Apply, intents are queued.
        // commitBrainIntents drains them.
        if (intentQueueGateIsOn) {
            commitBrainIntents(&warrior, &warrior.brainRuntime);
        }
    });

    res.sink = s_sink;
    g_activeSink = nullptr;
    return res;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// FITBLOCK-WRITER-1: writer round-trip self-test (--fitwriter-selftest).
//
// 1. Emit a specials file (Aliases block + TechSpecial blocks with alias= /
//    variantOf= / quoted DO args / WAIT / STOP) with FitBlockWriter.
// 2. Re-parse it with the REAL raw scanner (parseBrainSpecialBodyFromPath) and
//    assert keys, entry selection, verb round-trip, alias lookup and variantOf
//    inheritance all hold on the writer's output.
// 3. Golden-string compare of a carver-shaped Brain{} block (deterministic output).
// 4. Negative test: saveToFile refuses unbalanced blocks.
// Exit 0 = all checks pass; 1 = failure (each failure printed).
static int runFitWriterSelfTest() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  %-64s %s\n", what, ok ? "PASS" : "FAIL");
        if (!ok) ++fails;
    };

    // --- 1. Emit a specials file. ---
    FitBlockWriter w;
    w.writeRaw("FITini");
    w.blankLine();
    w.writeComment("FITBLOCK-WRITER-1 self-test emission (round-trip fixture)");
    w.beginBlock("Aliases");
    w.writeString("holdfast", "OPORD.CoreGuard");
    w.endBlock();
    w.blankLine();
    w.beginBlock("TechSpecial");
    w.writeString("key",   "writer.scenario_main");
    w.writeString("alias", "Writer.Main");
    w.writeString("type",  "MissionSpecial");
    w.beginBlock("Body");
    w.writeRaw("DO Var.Set \"ScenarioResult\" PLAYING scope=Mission");
    w.writeRaw("WAIT 5");
    w.writeRaw("DO holdfast");
    w.writeRaw("STOP");
    w.endBlock();
    w.endBlock();
    w.blankLine();
    w.beginBlock("TechSpecial");
    w.writeString("key", "writer.variant.child");
    w.writeString("variantOf", "writer.scenario_main");
    w.endBlock();
    check(w.depth() == 0, "writer: blocks balanced after emission");

    const char* tmpPath = "fitwriter_roundtrip_tmp.fit";
    check(w.saveToFile(tmpPath), "writer: saveToFile succeeds on balanced writer");

    // --- 2. Re-parse with the real scanner (gates were set in main before this). ---
    BrainSpecialBody body;
    SpecialIndex     index;
    bool parsed = parseBrainSpecialBodyFromPath(tmpPath, body, &index);
    check(parsed, "roundtrip: raw scanner parses writer output");
    check(index.size() == 2, "roundtrip: index has 2 TechSpecial blocks");
    const SpecialIndexEntry* mainE = specialIndexFind(index, "writer.scenario_main");
    check(mainE != nullptr, "roundtrip: key= field round-trips");
    const SpecialIndexEntry* byAlias = specialIndexFind(index, "Writer.Main");
    check(byAlias == mainE && byAlias != nullptr, "roundtrip: block alias= resolves (MC2_BRAIN_ALIAS)");
    check(body.verbs.size() == 4, "roundtrip: entry body has 4 verbs (DO/WAIT/DO/STOP)");
    check(body.verbs.size() >= 1 &&
          body.verbs[0] == "Var.Set \"ScenarioResult\" PLAYING scope=Mission",
          "roundtrip: inline-quoted DO args survive verbatim");
    check(body.verbs.size() >= 2 && body.verbs[1] == "WAIT 5",
          "roundtrip: WAIT flow line collected (MC2_BRAIN_FLOW)");
    check(body.verbs.size() >= 4 && body.verbs[3] == "STOP",
          "roundtrip: STOP flow verb collected");
    const SpecialIndexEntry* childE = specialIndexFind(index, "writer.variant.child");
    check(childE != nullptr && childE->body.verbs.size() == body.verbs.size(),
          "roundtrip: empty-body variant inherited parent verbs (MC2_BRAIN_VARIANTOF)");
    std::remove(tmpPath);

    // --- 3. Brain{} golden-string compare (carver shape). ---
    FitBlockWriter b;
    b.beginBlock("Brain");
    b.writeString("sourceABLBrain", "PBrain");
    b.writeString("compatibilityMode", "Enhanced");
    b.writeInlineBlock("PrimaryOPORD", "type = PlayerControlled");
    b.beginBlock("Tactics");
    b.writeFloat("Standard", 1.0f);
    b.writeFloat("Suppress", 0.5f);
    b.writeFloat("EngageRadius", 250.0f, 1);
    b.endBlock();
    b.endBlock();
    const char* kGolden =
        "Brain {\n"
        "\tsourceABLBrain = \"PBrain\"\n"
        "\tcompatibilityMode = \"Enhanced\"\n"
        "\tPrimaryOPORD { type = PlayerControlled }\n"
        "\tTactics {\n"
        "\t\tStandard = 1.00\n"
        "\t\tSuppress = 0.50\n"
        "\t\tEngageRadius = 250.0\n"
        "\t}\n"
        "}\n";
    check(b.str() == kGolden, "golden: Brain{} block matches carver shape exactly");

    // --- 4. Negative: unbalanced save refused. ---
    FitBlockWriter u;
    u.beginBlock("Brain");
    check(!u.saveToFile("fitwriter_should_not_exist.fit"),
          "negative: saveToFile refuses unbalanced blocks");

    std::printf("fitwriter-selftest: %s (%d failure(s))\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    // --- Parse CLI ---
    std::string manifestPath;
    std::string fixtureDir = "tests/fixtures/brain_runtime";
    bool        jsonOutput  = false;
    bool        applyMode   = false;
    bool        fitWriterSelfTest = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--manifest") == 0 && i+1 < argc)
            manifestPath = argv[++i];
        else if (std::strcmp(argv[i], "--fixture-dir") == 0 && i+1 < argc)
            fixtureDir = argv[++i];
        else if (std::strcmp(argv[i], "--json") == 0)
            jsonOutput = true;
        else if (std::strcmp(argv[i], "--apply-mode") == 0)
            applyMode = true;
        else if (std::strcmp(argv[i], "--fitwriter-selftest") == 0)
            fitWriterSelfTest = true;
    }

    if (manifestPath.empty() && !fitWriterSelfTest) {
        std::fprintf(stderr, "Usage: brain_dispatch_harness --manifest <path> [--fixture-dir <dir>] [--json] [--apply-mode] [--fitwriter-selftest]\n");
        return 2;
    }

    // Set required env gates BEFORE any dispatch call (static initializers).
    // In apply mode we set MC2_BRAIN_DISPATCH_APPLY=1 as well.
    (void)_putenv("MC2_BRAIN_DISPATCH=1");
    (void)_putenv("MC2_BRAIN_DISPATCH_CALL=1");
    (void)_putenv("MC2_BRAIN_DISPATCH_VAR=1");
    // TECHSCRIPT-DISPATCH-1D-M: enable mission-scope Var store for varmission-roundtrip fixture.
    (void)_putenv("MC2_BRAIN_VAR_MISSION=1");
    // BRAIN-OPORD-COREPATROL-1: enable patrol verb handler for patrol fixtures.
    (void)_putenv("MC2_BRAIN_PATROL=1");
    // HARNESS-INTENT-GATE-SCOPE-1: do NOT force MC2_BRAIN_INTENT_QUEUE globally — that
    // overrode the A/B runner's gate-OFF pass. Default it ON only when the caller did not
    // set it (so a bare harness run still exercises patrol + the intent path), but an
    // explicit caller value (the A/B runner sets =0 for the OFF pass) is respected.
    if (std::getenv("MC2_BRAIN_INTENT_QUEUE") == nullptr)
        (void)_putenv("MC2_BRAIN_INTENT_QUEUE=1");
    // BRAIN-FSM-1K-A/B: enable FSM verbs for fsm-* fixtures.
    (void)_putenv("MC2_BRAIN_FSM=1");
    // BRAINSPECIAL-ALIAS-1: enable the alias registry for the alias-registry fixture.
    // Safe for all other fixtures: resolution only rewrites tokens that match the
    // registry or the catalog case-insensitively; exact canonical spellings pass through.
    (void)_putenv("MC2_BRAIN_ALIAS=1");
    // BRAINSPECIAL-SCOPE-GLOBAL-1: merge global_specials.fit into every fixture parse.
    // Safe for all other fixtures: global keys are namespaced global.* (no collisions),
    // and merge only appends Call targets — entry-body selection is unaffected.
    (void)_putenv("MC2_BRAIN_SCOPE_GLOBAL=1");
    // BRAINSPECIAL-VARIANTOF-1: resolve variantOf inheritance for the variantof-* fixtures.
    // Safe for all other fixtures: resolution only touches entries carrying a variantOf field.
    (void)_putenv("MC2_BRAIN_VARIANTOF=1");
    // BRAINSPECIAL-FLOW-WAIT-1: enable flow verbs for the flow-* fixtures.
    // Other fixtures gain a trailing "STOP" verb (their STOP sentinels become verbs) —
    // harmless: STOP is the last body line by convention and is not an effect verb.
    (void)_putenv("MC2_BRAIN_FLOW=1");
    if (applyMode) {
        (void)_putenv("MC2_BRAIN_DISPATCH_APPLY=1");
    }
    // MC2_BRAIN_INTENT_QUEUE is read from the environment as-is.
    // The caller sets it before invoking the harness for gate-ON runs.

    // Detect intent queue gate state (read BEFORE first static-init call).
    const char* intentQueueEnv = std::getenv("MC2_BRAIN_INTENT_QUEUE");
    bool intentQueueGateOn = (intentQueueEnv && std::atoi(intentQueueEnv) != 0);

    // FITBLOCK-WRITER-1: writer round-trip self-test (needs the gates set above:
    // ALIAS for alias= lookup, VARIANTOF for inheritance, FLOW for WAIT/STOP verbs).
    if (fitWriterSelfTest)
        return runFitWriterSelfTest();

    // --- Load manifest ---
    std::string jsonErr;
    JsonValue manifest = parseJsonFile(manifestPath, jsonErr);
    if (!jsonErr.empty()) {
        std::fprintf(stderr, "ERROR: manifest parse failed: %s\n", jsonErr.c_str());
        return 2;
    }
    if (!manifest.has("fixtures")) {
        std::fprintf(stderr, "ERROR: manifest missing 'fixtures' array\n");
        return 2;
    }

    std::vector<FixtureEntry> fixtures;
    for (const JsonValue& jv : manifest.at("fixtures").arr())
        fixtures.push_back(entryFromJson(jv));

    // V1: set up simple fake object manager (returns nullptr for all lookups).
    static class SimpleFakeObjMgr : public GameObjectManager {
    public:
        GameObjectPtr getByWatchID(long) override { return nullptr; }
    } s_simpleFakeObjMgr;

    // --- Results ---
    int passCount = 0, failCount = 0, skipCount = 0;
    struct Result {
        std::string name;
        std::string status; // PASS / FAIL / SKIP
        std::string mode;   // "v1" or "v2-apply"
        std::vector<std::string> failures;
    };
    std::vector<Result> results;

    // --- Run fixtures ---
    for (const FixtureEntry& fix : fixtures) {
        Result res;
        res.name = fix.name;
        res.mode = "v1";

        // Build path to fixture file
        std::string fitPath = fixtureDir + "/" + fix.file;

        BrainSpecialBody body;
        SpecialIndex     index;

        // V1: parse + TraceOnly (always runs, even in apply-mode for back-compat).
        std::string traceOutput = captureStderr([&]() {
            // Reset object manager for v1 pass.
            ObjectManager = &s_simpleFakeObjMgr;
            parseBrainSpecialBodyFromPath(fitPath.c_str(), body, &index);
            if (body.loaded) {
                executeSpecialBody_TraceOnly(body, /*wid=*/1, nullptr, &index, nullptr);
            }
        });

        // --- V1 Assertions ---
        if (fix.partial) {
            res.status = "SKIP";
            ++skipCount;
            results.push_back(res);
            goto next_fixture;
        }

        // 1. loaded
        if (fix.expectedLoaded && !body.loaded) {
            res.failures.push_back("expected loaded=true but body.loaded=false (trace: " + traceOutput.substr(0, 200) + ")");
        }

        if (body.loaded) {
            // 2. bodyHasX checks
            if (fix.expectedBodyHasPowerdown  && !bodyHasPowerdown(body))
                res.failures.push_back("expected bodyHasPowerdown=true");
            if (fix.expectedBodyHasUnitEject  && !bodyHasUnitEject(body))
                res.failures.push_back("expected bodyHasUnitEject=true");
            if (fix.expectedBodyHasCoreGuard  && !bodyHasCoreGuard(body))
                res.failures.push_back("expected bodyHasCoreGuard=true");
            if (fix.expectedBodyHasCoreMoveTo && !bodyHasCoreMoveTo(body))
                res.failures.push_back("expected bodyHasCoreMoveTo=true");
            if (fix.expectedBodyHasCoreAttack && !bodyHasCoreAttack(body))
                res.failures.push_back("expected bodyHasCoreAttack=true");
            if (fix.expectedBodyHasUnitRetreat&& !bodyHasUnitRetreat(body))
                res.failures.push_back("expected bodyHasUnitRetreat=true");
            if (fix.expectedBodyHasEffect     && !bodyHasEffect(body))
                res.failures.push_back("expected bodyHasEffect=true");
        }

        // 3. trace substring checks (V1 TraceOnly; FSM/flow sequential fixtures check
        // their combined Apply traces below instead)
        if (!fix.fsmSequential && !fix.flowSequential) {
        for (const std::string& sub : fix.expectedTraceSubstrings) {
            if (traceOutput.find(sub) == std::string::npos) {
                res.failures.push_back("trace missing substring: '" + sub + "'");
                if (res.failures.size() == 1)
                    res.failures.push_back("  trace preview: " + traceOutput.substr(0, 400));
            }
        }
        }

        // --- BRAIN-FSM-1K-A: FSM sequential run ---
        // Runs ALL index bodies in order on one FsmMechWarrior (state persists across bodies).
        // Checks trace substrings against combined Apply trace.  Only runs when applyMode=true
        // and fix.fsmSequential=true.
        if (applyMode && fix.fsmSequential && body.loaded && !index.empty()) {
            res.mode = "v2-fsm-sequential";
            FsmMechWarrior fsmWarrior;
            // fsmWarrior.fsmRuntime already zero-inited by MechBrainRuntime default ctor.
            std::string fsmTrace = captureStderr([&]() {
                for (const SpecialIndexEntry& entry : index) {
                    executeSpecialBody_Apply(entry.body, &fsmWarrior, /*wid=*/1,
                                             &fsmWarrior.fsmRuntime.varStore, &index, entry.key.c_str());
                    // Drain intent buffer after each body so orderCount is correct
                    // regardless of MC2_BRAIN_INTENT_QUEUE gate state.
                    if (fsmWarrior.fsmRuntime.pendingIntentCount > 0)
                        commitBrainIntents(&fsmWarrior, &fsmWarrior.fsmRuntime);
                }
            });
            for (const std::string& sub : fix.expectedTraceSubstrings) {
                if (fsmTrace.find(sub) == std::string::npos) {
                    res.failures.push_back("fsm trace missing substring: '" + sub + "'");
                    if (res.failures.size() == 1)
                        res.failures.push_back("  fsm trace preview: " + fsmTrace.substr(0, 600));
                }
            }
            // Additional gated-verb check: CoreGuard should fire for instate_match (orders>0)
            // but NOT fire for instate_mismatch (gated out). FsmMechWarrior.orderCount tracks this.
            // For the fsm fixture: SetState exits early (0 orders), instate_match fires CoreGuard (1 order),
            // instate_mismatch gated (0 orders), SetStatePrev exits early (0 orders). Total = 1 order.
            if (fsmWarrior.orderCount != 1) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "fsm: expected 1 order from gate-open CoreGuard, got %d", fsmWarrior.orderCount);
                res.failures.push_back(buf);
            }
        }

        // --- BRAINSPECIAL-FLOW-WAIT-1: flow sequential run ---
        // Three passes at stub brain time 0 / 1000 / 6000 ms over ALL index bodies (in
        // index order) on one persistent FsmMechWarrior. WAIT latches open once the stub
        // time passes its deadline; WAIT_UNTIL latches when its Var condition holds; the
        // per-verb-index flowFired guard must keep the total order count at expected_count.
        if (applyMode && fix.flowSequential && body.loaded && !index.empty()) {
            res.mode = "v2-flow-sequential";
            FsmMechWarrior flowWarrior;
            static const uint32_t kFlowPassTimesMs[3] = { 0u, 1000u, 6000u };
            std::string flowTrace = captureStderr([&]() {
                for (int pass = 0; pass < 3; ++pass) {
                    harnessSetBrainTimeMs(kFlowPassTimesMs[pass]);
                    for (const SpecialIndexEntry& entry : index) {
                        executeSpecialBody_Apply(entry.body, &flowWarrior, /*wid=*/1,
                                                 &flowWarrior.fsmRuntime.varStore, &index,
                                                 entry.key.c_str());
                        if (flowWarrior.fsmRuntime.pendingIntentCount > 0)
                            commitBrainIntents(&flowWarrior, &flowWarrior.fsmRuntime);
                    }
                }
            });
            harnessSetBrainTimeMs(0);  // reset for subsequent fixtures
            for (const std::string& sub : fix.expectedTraceSubstrings) {
                if (flowTrace.find(sub) == std::string::npos) {
                    res.failures.push_back("flow trace missing substring: '" + sub + "'");
                    if (res.failures.size() == 1)
                        res.failures.push_back("  flow trace preview: " + flowTrace.substr(0, 600));
                }
            }
            if (fix.applyExp.hasExpectation && flowWarrior.orderCount != fix.applyExp.expectedCount) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                    "flow: expected %d order(s) across 3 passes, got %d (gate_%s)",
                    fix.applyExp.expectedCount, flowWarrior.orderCount,
                    intentQueueGateOn ? "ON" : "OFF");
                res.failures.push_back(buf);
            }
        }

        // --- V2 Apply assertions (only in --apply-mode, only if fixture has apply_expectation) ---
        if (applyMode && fix.applyExp.hasExpectation && body.loaded && !fix.flowSequential
            && !(fix.applyExp.requiresIntentQueue && !intentQueueGateOn)) {
            // HARNESS-INTENT-GATE-SCOPE-1: skip intent-queue-only fixtures (patrol) in a
            // gate-OFF process — their order can't be emitted without the queue, so asserting
            // it there would falsely fail. The 6 dual-path effect verbs still run both gates.
            res.mode = "v2-apply";
            const ApplyExpectation& ae = fix.applyExp;

            ApplyRunResult ar = runApply(body, index, ae, intentQueueGateOn);

            // Assert expected order count.
            int actualCount = (int)ar.sink.orders.size();
            if (actualCount != ae.expectedCount) {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    "apply: expected %d order(s) in sink, got %d (gate_%s) trace: %.200s",
                    ae.expectedCount, actualCount,
                    intentQueueGateOn ? "ON" : "OFF",
                    ar.traceOutput.c_str());
                res.failures.push_back(buf);
            }

            // Assert expected order code (if orders expected).
            if (ae.expectedCount > 0 && ae.expectedOrderCode >= 0 && !ar.sink.orders.empty()) {
                TacticalOrderCode expectedCode = (TacticalOrderCode)ae.expectedOrderCode;
                if (ar.sink.orders[0].code != expectedCode) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                        "apply: expected order code %d, got %d (gate_%s)",
                        ae.expectedOrderCode, (int)ar.sink.orders[0].code,
                        intentQueueGateOn ? "ON" : "OFF");
                    res.failures.push_back(buf);
                }
            }

            // Assert ATTACK target WID.
            if (ae.expectedCount > 0 && ae.expectedOrderCode == (int)TACTICAL_ORDER_ATTACK_OBJECT
                && ae.expectedTargetWID != 0 && !ar.sink.orders.empty()) {
                if (ar.sink.orders[0].targetWID != ae.expectedTargetWID) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                        "apply: ATTACK targetWID expected %ld got %ld (gate_%s)",
                        ae.expectedTargetWID, ar.sink.orders[0].targetWID,
                        intentQueueGateOn ? "ON" : "OFF");
                    res.failures.push_back(buf);
                }
            }

            // Assert MOVETO waypoint (approximate).
            if (ae.checkWaypoint && ae.expectedCount > 0 && !ar.sink.orders.empty()) {
                const RecordedOrder& ro = ar.sink.orders[0];
                float dx = ro.waypointX - ae.expectedX;
                float dy = ro.waypointY - ae.expectedY;
                float dz = ro.waypointZ - ae.expectedZ;
                if (dx*dx + dy*dy + dz*dz > 0.01f) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                        "apply: MOVETO waypoint expected (%.1f %.1f %.1f) got (%.1f %.1f %.1f) (gate_%s)",
                        ae.expectedX, ae.expectedY, ae.expectedZ,
                        ro.waypointX, ro.waypointY, ro.waypointZ,
                        intentQueueGateOn ? "ON" : "OFF");
                    res.failures.push_back(buf);
                }
            }

            // Assert no order for guard-fire cases.
            if (ae.expectedCount == 0 && !ar.sink.orders.empty()) {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    "apply: expected 0 orders (guard should fire), got %d order(s) (gate_%s)",
                    (int)ar.sink.orders.size(), intentQueueGateOn ? "ON" : "OFF");
                res.failures.push_back(buf);
            }

            // BRAIN-OPORD-COREPATROL-1: apply-path trace substring checks.
            // Checked against ar.traceOutput (executeSpecialBody_Apply stderr capture).
            for (const std::string& sub : ae.applyTraceSubstrings) {
                if (ar.traceOutput.find(sub) == std::string::npos) {
                    res.failures.push_back("apply trace missing substring: '" + sub + "'");
                    if (res.failures.size() == 1)
                        res.failures.push_back("  apply trace preview: " + ar.traceOutput.substr(0, 400));
                }
            }
        }

        if (res.failures.empty()) {
            res.status = "PASS";
            ++passCount;
        } else {
            res.status = "FAIL";
            ++failCount;
        }
        results.push_back(res);
        continue;

        next_fixture:;
    }

    // --- Output ---
    const char* modeLabel = applyMode
        ? (intentQueueGateOn ? "v2-apply gate=ON" : "v2-apply gate=OFF")
        : "v1-parse";

    if (jsonOutput) {
        std::printf("{\n  \"mode\": \"%s\",\n  \"pass\": %d, \"fail\": %d, \"skip\": %d,\n  \"results\": [\n",
                    modeLabel, passCount, failCount, skipCount);
        for (size_t i = 0; i < results.size(); ++i) {
            const Result& r = results[i];
            std::printf("    {\"name\": \"%s\", \"status\": \"%s\"", r.name.c_str(), r.status.c_str());
            if (!r.failures.empty()) {
                std::printf(", \"failures\": [");
                for (size_t j = 0; j < r.failures.size(); ++j) {
                    std::string f = r.failures[j];
                    std::string esc;
                    for (char c : f) { if (c=='"') esc+="\\\""; else esc+=c; }
                    std::printf("\"%s\"%s", esc.c_str(), j+1<r.failures.size()?", ":"");
                }
                std::printf("]");
            }
            std::printf("}%s\n", i+1<results.size()?",":"");
        }
        std::printf("  ]\n}\n");
    } else {
        std::printf("brain_dispatch_harness [%s]: %d fixtures (%d pass, %d fail, %d skip)\n",
                    modeLabel,
                    (int)results.size(), passCount, failCount, skipCount);
        for (const Result& r : results) {
            std::printf("  %-50s  %s\n", r.name.c_str(), r.status.c_str());
            for (const std::string& f : r.failures)
                std::printf("    FAIL: %s\n", f.c_str());
        }
    }

    return (failCount > 0) ? 1 : 0;
}
