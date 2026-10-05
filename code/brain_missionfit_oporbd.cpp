// BRAIN-MISSIONFIT-OPORD-CONSUMER-1: pure parser for carver mission.fit PrimaryOPORD blocks.
// See brain_missionfit_oporbd.h. Raw line scan — FitIniFile cannot see brace-blocks.
#include "brain_missionfit_oporbd.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {

// Value token after "<key> =" on a line. Writes the trimmed token; returns true if non-empty.
bool lineKeyToken(const char* line, const char* key, char* out, size_t outsz) {
    const char* p = std::strstr(line, key);
    if (!p) return false;
    p += std::strlen(key);
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '=') return false;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n'
           && *p != '}' && *p != ';' && i + 1 < outsz) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

// Float after a single-char axis ('x'/'y') as in "x=3008.000". Boundary-aware: an axis char
// preceded by a letter is rejected (so "index=0" does NOT match 'x='). Returns true on success.
bool axisFloat(const char* line, char axis, float* out) {
    for (const char* p = std::strchr(line, axis); p; p = std::strchr(p + 1, axis)) {
        if (p > line && std::isalpha((unsigned char)p[-1])) continue;  // reject 'inde[x]='
        const char* q = p + 1;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q != '=') continue;
        ++q;
        while (*q == ' ' || *q == '\t') ++q;
        char* end = nullptr;
        float v = std::strtof(q, &end);
        if (end == q) continue;
        *out = v;
        return true;
    }
    return false;
}

void setType(MissionFitOpord& r, const char* tok) {
    std::strncpy(r.primaryType, tok, sizeof(r.primaryType) - 1);
    r.primaryType[sizeof(r.primaryType) - 1] = '\0';
}

// Copy a token into a fixed char buffer with NUL-termination.
void copyTok(char* dst, size_t dstsz, const char* tok) {
    std::strncpy(dst, tok, dstsz - 1);
    dst[dstsz - 1] = '\0';
}

// String value of `key = "quoted"` or `key = bareword`. Strips surrounding quotes.
bool lineKeyString(const char* line, const char* key, char* out, size_t outsz) {
    const char* p = std::strstr(line, key);
    if (!p) return false;
    // Ensure key boundary (preceding char not alnum) so "Help" != "AttackerHelp".
    if (p > line && (std::isalnum((unsigned char)p[-1]) || p[-1] == '_')) return false;
    p += std::strlen(key);
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '=') return false;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    size_t i = 0;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"' && i + 1 < outsz) out[i++] = *p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n'
               && *p != '}' && *p != ';' && i + 1 < outsz) {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return i > 0;
}

// Boundary-aware float for a named key: "EngageRadius = 300.0". Rejects substring keys.
bool lineKeyFloat(const char* line, const char* key, float* out) {
    const char* p = std::strstr(line, key);
    if (!p) return false;
    if (p > line && (std::isalnum((unsigned char)p[-1]) || p[-1] == '_')) return false;
    p += std::strlen(key);
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '=') return false;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    char* end = nullptr;
    float v = std::strtof(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

// Tri-state bool for a named key: present → 0/1 ("true"→1), absent → leaves *out untouched.
bool lineKeyBool(const char* line, const char* key, int* out) {
    char tok[16];
    const char* p = std::strstr(line, key);
    if (!p) return false;
    if (p > line && (std::isalnum((unsigned char)p[-1]) || p[-1] == '_')) return false;
    if (!lineKeyToken(line, key, tok, sizeof(tok))) return false;
    *out = (std::strcmp(tok, "true") == 0) ? 1 : 0;
    return true;
}

void slotReset(MissionFitOpordSlot& s) {
    s.type[0] = '\0';
    s.loop = false;
    s.waypointCount = 0;
    s.escortTargetIndex = -1;
}

// Parse one slot-body line (type/loop/Waypoint/target) into slot s.
void slotParseLine(MissionFitOpordSlot& s, const char* buf) {
    char tok[24];
    if (s.type[0] == '\0' && lineKeyToken(buf, "type", tok, sizeof(tok))) copyTok(s.type, sizeof(s.type), tok);
    if (lineKeyToken(buf, "loop", tok, sizeof(tok))) s.loop = (std::strcmp(tok, "true") == 0);
    if (std::strstr(buf, "Waypoint") && s.waypointCount < kMissionFitMaxWaypoints) {
        float x, y;
        if (axisFloat(buf, 'x', &x) && axisFloat(buf, 'y', &y)) {
            s.waypoints[s.waypointCount].x = x;
            s.waypoints[s.waypointCount].y = y;
            ++s.waypointCount;
        }
    }
    // Escort target: "target = WarriorN" or "EscortTarget = N".
    if (std::strstr(buf, "EscortTarget")) {
        if (lineKeyToken(buf, "EscortTarget", tok, sizeof(tok))) {
            const char* d = tok;
            if (std::strncmp(d, "Warrior", 7) == 0) d += 7;
            char* end = nullptr;
            long v = std::strtol(d, &end, 10);
            if (end != d) s.escortTargetIndex = (int)v;
        }
    } else if (std::strstr(buf, "target")) {
        if (lineKeyToken(buf, "target", tok, sizeof(tok))) {
            const char* d = tok;
            if (std::strncmp(d, "Warrior", 7) == 0) d += 7;
            char* end = nullptr;
            long v = std::strtol(d, &end, 10);
            if (end != d) s.escortTargetIndex = (int)v;
        }
    }
}

} // namespace

int parseMissionFitOpords(const char* text, MissionFitOpord* out, int maxOut) {
    if (!text || !out || maxOut <= 0) return 0;

    int count = 0;
    int curWarrior = -1;
    bool inPrimary = false;
    int depth = 0;
    MissionFitOpord cur;
    char buf[512];
    char tok[24];

    const char* line = text;
    while (*line) {
        const char* nl = line;
        while (*nl && *nl != '\n') ++nl;
        size_t len = (size_t)(nl - line);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        std::memcpy(buf, line, len);
        buf[len] = '\0';
        line = (*nl) ? nl + 1 : nl;

        const char* lb = buf;
        while (*lb == ' ' || *lb == '\t') ++lb;

        // Section header [WarriorN] (or any [Section]) — resets OPORD scope.
        if (lb[0] == '[') {
            inPrimary = false;
            depth = 0;
            curWarrior = (std::strncmp(lb, "[Warrior", 8) == 0)
                       ? (int)std::strtol(lb + 8, nullptr, 10) : -1;
            continue;
        }

        if (!inPrimary) {
            if (curWarrior >= 0 && std::strstr(buf, "PrimaryOPORD")) {
                cur.warriorIndex = curWarrior;
                cur.primaryType[0] = '\0';
                cur.loop = false;
                cur.waypointCount = 0;
                inPrimary = true;
                depth = 0;
                for (const char* q = buf; *q; ++q) { if (*q == '{') ++depth; else if (*q == '}') --depth; }
                if (lineKeyToken(buf, "type", tok, sizeof(tok))) setType(cur, tok);
                if (depth <= 0) {                         // single-line PrimaryOPORD { … }
                    if (count < maxOut) out[count++] = cur;
                    inPrimary = false;
                    depth = 0;
                }
            }
            continue;
        }

        // Inside a multi-line PrimaryOPORD body.
        for (const char* q = buf; *q; ++q) { if (*q == '{') ++depth; else if (*q == '}') --depth; }
        if (cur.primaryType[0] == '\0' && lineKeyToken(buf, "type", tok, sizeof(tok))) setType(cur, tok);
        if (lineKeyToken(buf, "loop", tok, sizeof(tok))) cur.loop = (std::strcmp(tok, "true") == 0);
        if (std::strstr(buf, "Waypoint") && cur.waypointCount < kMissionFitMaxWaypoints) {
            float x, y;
            if (axisFloat(buf, 'x', &x) && axisFloat(buf, 'y', &y)) {
                cur.waypoints[cur.waypointCount].x = x;
                cur.waypoints[cur.waypointCount].y = y;
                ++cur.waypointCount;
            }
        }
        if (depth <= 0) {                                 // PrimaryOPORD closing brace
            if (count < maxOut) out[count++] = cur;
            inPrimary = false;
            depth = 0;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------------------------
// BRAIN-FULL-PARSE-1: full declarative Brain{} block parse.

namespace {

// Which sub-block (slot or Tactics) we're currently accumulating into, by brace depth.
enum class SubKind { None, Primary, Secondary, Tertiary, Tactics };

void brainResetRecord(MissionFitBrain& b, int warrior) {
    std::memset(&b, 0, sizeof(b));
    b.warriorIndex        = warrior;
    b.archetype[0]        = '\0';
    b.compatibilityMode[0]= '\0';
    b.engageRadius        = -1.0f;
    b.attackerHelpRadius  = -1.0f;
    b.defenderHelpRadius  = -1.0f;
    b.requestHelp         = -1;
    b.returnToPost        = -1;
    b.wakeOnAttack        = -1;
    b.poweredDown         = -1;
    b.posX                = -1.0f;
    b.posY                = -1.0f;
    slotReset(b.primary);
    slotReset(b.secondary);
    slotReset(b.tertiary);
    b.tacticCount         = 0;
}

MissionFitOpordSlot* slotFor(MissionFitBrain& b, SubKind k) {
    switch (k) {
        case SubKind::Primary:   return &b.primary;
        case SubKind::Secondary: return &b.secondary;
        case SubKind::Tertiary:  return &b.tertiary;
        default:                 return nullptr;
    }
}

// A non-"type" tactic line "Word = float" → name/weight.
void tacticParseLine(MissionFitBrain& b, const char* buf) {
    if (b.tacticCount >= kMissionFitMaxTactics) return;
    char name[24];
    // Grab the leading word.
    const char* p = buf;
    while (*p == ' ' || *p == '\t') ++p;
    if (!(std::isalpha((unsigned char)*p))) return;
    size_t i = 0;
    while ((std::isalnum((unsigned char)*p) || *p == '_') && i + 1 < sizeof(name)) name[i++] = *p++;
    name[i] = '\0';
    if (i == 0) return;
    if (std::strcmp(name, "type") == 0) return;            // never a tactic
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '=') return;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    char* end = nullptr;
    float w = std::strtof(p, &end);
    if (end == p) return;
    copyTok(b.tacticName[b.tacticCount], sizeof(b.tacticName[0]), name);
    b.tacticWeight[b.tacticCount] = w;
    ++b.tacticCount;
}

int countBraces(const char* s) {
    int d = 0;
    for (const char* q = s; *q; ++q) { if (*q == '{') ++d; else if (*q == '}') --d; }
    return d;
}

} // namespace

int parseMissionFitBrains(const char* text, MissionFitBrain* out, int maxOut) {
    if (!text || !out || maxOut <= 0) return 0;

    int count = 0;
    int curWarrior = -1;
    int curPart = -1;            // N from a [PartN] placement block (carries the unit's spawn pos)
    bool inBrain = false;
    // [PartN] positions collected in a pass-independent table (a Part block may appear before OR
    // after its Warrior brain block), assigned onto the brain records by index at the end.
    static const int kMaxPart = 128;
    float partX[kMaxPart], partY[kMaxPart];
    for (int i = 0; i < kMaxPart; ++i) { partX[i] = -1e9f; partY[i] = -1e9f; }
    bool haveRecord = false;
    int brainDepth = 0;          // brace depth relative to inside the Brain block (Brain body = 1)
    SubKind sub = SubKind::None; // active sub-block
    int subDepth = 0;            // brainDepth at which the active sub-block lives (its body)
    MissionFitBrain cur;

    char buf[512];

    const char* line = text;
    while (*line) {
        const char* nl = line;
        while (*nl && *nl != '\n') ++nl;
        size_t len = (size_t)(nl - line);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        std::memcpy(buf, line, len);
        buf[len] = '\0';
        line = (*nl) ? nl + 1 : nl;

        const char* lb = buf;
        while (*lb == ' ' || *lb == '\t') ++lb;

        // Section header resets brain scope (and flushes a pending record).
        if (lb[0] == '[') {
            if (inBrain && haveRecord && count < maxOut) out[count++] = cur;
            inBrain = false; haveRecord = false; brainDepth = 0;
            sub = SubKind::None; subDepth = 0;
            curWarrior = (std::strncmp(lb, "[Warrior", 8) == 0)
                       ? (int)std::strtol(lb + 8, nullptr, 10) : -1;
            // [PartN] placement block (N is a digit immediately after "[Part"); carries the unit's
            // spawn position, which the editor matches against the selected unit to pick its brain.
            curPart = (std::strncmp(lb, "[Part", 5) == 0 && lb[5] >= '0' && lb[5] <= '9')
                    ? (int)std::strtol(lb + 5, nullptr, 10) : -1;
            continue;
        }

        // Collect spawn position from a [PartN] block into the part table (assigned to brains below).
        if (!inBrain && curPart >= 0 && curPart < kMaxPart) {
            float pv;
            if (lineKeyFloat(buf, "PositionX", &pv)) partX[curPart] = pv;
            if (lineKeyFloat(buf, "PositionY", &pv)) partY[curPart] = pv;
        }

        if (!inBrain) {
            // Look for the start of a Brain{} block. "st Brain = \"\"" is NOT a block (no '{').
            if (curWarrior >= 0 && std::strstr(buf, "Brain") && std::strchr(buf, '{')) {
                brainResetRecord(cur, curWarrior);
                inBrain = true; haveRecord = true;
                brainDepth = 0; sub = SubKind::None; subDepth = 0;
                // The '{' opens the Brain body → brainDepth becomes 1.
                brainDepth += countBraces(buf);
                if (brainDepth <= 0) {                       // pathological single-line Brain{}
                    if (count < maxOut) out[count++] = cur;
                    inBrain = false; haveRecord = false; brainDepth = 0;
                }
            }
            continue;
        }

        // --- Inside a Brain block. Determine this line's depth contribution. ---
        int before = brainDepth;
        int delta  = countBraces(buf);
        int after  = before + delta;

        // If we are not in a sub-block, this line may OPEN one or set a Brain switch.
        if (sub == SubKind::None) {
            // Detect sub-block openers (they contain '{').
            SubKind opening = SubKind::None;
            if (std::strchr(buf, '{')) {
                if      (std::strstr(buf, "PrimaryOPORD"))   opening = SubKind::Primary;
                else if (std::strstr(buf, "SecondaryOPORD")) opening = SubKind::Secondary;
                else if (std::strstr(buf, "TertiaryOPORD"))  opening = SubKind::Tertiary;
                else if (std::strstr(buf, "Tactics"))        opening = SubKind::Tactics;
            }
            if (opening != SubKind::None) {
                MissionFitOpordSlot* s = slotFor(cur, opening);
                if (s) slotParseLine(*s, buf);               // single-line content (type=, etc.)
                else /* Tactics */ tacticParseLine(cur, buf);
                if (after > before) {                        // multi-line sub-block stays open
                    sub = opening;
                    subDepth = before + 1;                   // the sub-block body lives here
                }
                brainDepth = after;
                if (brainDepth <= 0) {                       // Brain block closed on this line
                    if (count < maxOut) out[count++] = cur;
                    inBrain = false; haveRecord = false;
                    sub = SubKind::None; subDepth = 0; brainDepth = 0;
                }
                continue;
            }
            // Plain Brain switch line (no sub-block).
            {
                char str[48];
                if (lineKeyString(buf, "archetype", str, sizeof(str)))
                    copyTok(cur.archetype, sizeof(cur.archetype), str);
                if (lineKeyString(buf, "compatibilityMode", str, sizeof(str)))
                    copyTok(cur.compatibilityMode, sizeof(cur.compatibilityMode), str);
                float f;
                if (lineKeyFloat(buf, "EngageRadius", &f))        cur.engageRadius       = f;
                if (lineKeyFloat(buf, "AttackerHelpRadius", &f))  cur.attackerHelpRadius = f;
                if (lineKeyFloat(buf, "DefenderHelpRadius", &f))  cur.defenderHelpRadius = f;
                int bv;
                if (lineKeyBool(buf, "RequestHelp", &bv))   cur.requestHelp  = bv;
                if (lineKeyBool(buf, "ReturnToPost", &bv))  cur.returnToPost = bv;
                if (lineKeyBool(buf, "WakeOnAttack", &bv))  cur.wakeOnAttack = bv;
                if (lineKeyBool(buf, "PoweredDown", &bv))   cur.poweredDown  = bv;
            }
            brainDepth = after;
            if (brainDepth <= 0) {                           // Brain closing brace
                if (count < maxOut) out[count++] = cur;
                inBrain = false; haveRecord = false;
                sub = SubKind::None; subDepth = 0; brainDepth = 0;
            }
            continue;
        }

        // --- Inside a sub-block (slot or Tactics). ---
        // A line that only closes the sub-block (brace back below subDepth) ends it.
        if (after < subDepth) {
            sub = SubKind::None; subDepth = 0;
            brainDepth = after;
            if (brainDepth <= 0) {                           // sub-block close also closed Brain
                if (count < maxOut) out[count++] = cur;
                inBrain = false; haveRecord = false; brainDepth = 0;
            }
            continue;
        }
        // Body content line.
        if (sub == SubKind::Tactics) {
            tacticParseLine(cur, buf);
        } else {
            MissionFitOpordSlot* s = slotFor(cur, sub);
            if (s) slotParseLine(*s, buf);
        }
        brainDepth = after;
        if (after < subDepth) {                              // (handled above) safety
            sub = SubKind::None; subDepth = 0;
        }
    }

    // Flush trailing record (text ended inside a Brain block).
    if (inBrain && haveRecord && count < maxOut) out[count++] = cur;

    // Assign collected [PartN] spawn positions onto the brain records by warrior index
    // (order-independent: a Part block may precede or follow its Warrior brain block).
    for (int r = 0; r < count; ++r) {
        int n = out[r].warriorIndex;
        if (n >= 0 && n < kMaxPart) {
            out[r].posX = partX[n];
            out[r].posY = partY[n];
        }
    }
    return count;
}

// BRAIN-PATROL-VIEWER-BRIDGE-1: see brain_missionfit_oporbd.h for the shared-consumer rationale.
const MissionFitBrain* matchBrainForPos(const MissionFitBrain* brains, int brainCount,
                                         float ux, float uy, float tol) {
    const MissionFitBrain* best = nullptr;
    float bestD2 = tol * tol;
    for (int i = 0; i < brainCount; ++i) {
        const MissionFitBrain& b = brains[i];
        if (b.posX <= -1e8f) continue;          // no spawn position parsed for this brain
        float dx = b.posX - ux;
        float dy = b.posY - uy;
        float d2 = dx * dx + dy * dy;
        if (d2 <= bestD2) {
            bestD2 = d2;
            best = &b;
        }
    }
    return best;
}
