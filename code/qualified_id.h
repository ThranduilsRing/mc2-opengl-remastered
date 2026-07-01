#ifndef QUALIFIED_ID_H
#define QUALIFIED_ID_H
// UNIT-ID-NAMESPACE-1 (Phase-3 slice 6 of the UNIT-PROFILE arc).
//
// The STRING-CONVENTION + MERGE-RULE layer that sits ABOVE the existing hashed
// id types (HardpointId, WeaponId, etc). BT2018's #1 modder-friendliness lesson:
// every join is a greppable readable string id, never a positional index / atlas
// column / DLL-string. This header formalizes the `namespace:name` convention the
// converter already emits (`core:<name>`) and the deterministic multi-mod
// override/merge rule, so units AND weapons AND hardpoints all speak one id
// language.
//
// SCOPE — this is a CONVENTION + PRECEDENCE utility, not a new id type. It does
// NOT replace HardpointId/WeaponId hashing; it validates/canonicalizes the
// human-readable string those hashes are computed from, and resolves which mod's
// definition wins when several define the same `namespace:name`.
//
// Pure, header-only (std::string only — no engine/GL/IO drag), so it compiles
// into the offline `mc2_tests` target and into the Python-mirroring tools.
//
// FORMAT: `namespace:name`
//   namespace : starts with [a-z], then [a-z0-9_], length 1..32
//   name      : starts with [a-z], then [a-z0-9_], length 1..64
//   The single ':' separates them. Everything is lowercase-canonical.
//   A bare `name` (no ':') canonicalizes to `core:name` (stock reserved ns).
//   `core` is the RESERVED stock namespace — mods must NOT author into `core:`.
//
// OVERRIDE PRECEDENCE (deterministic): `core` < earlier-mod < later-mod.
//   Load order is authoritative. `core` (stock) is always the base layer (load
//   order 0 by convention). A mod loaded later overrides one loaded earlier when
//   both define the same canonical `namespace:name`. Ties never occur because a
//   given qualified id is authored by exactly one owning namespace; the merge is
//   over the SET of ids (later source's id-set wins per-id), keyed by the
//   canonical string. This mirrors qualified_id.py's precedence exactly.

#include <string>

namespace mc2id {

// The reserved stock namespace. Anything the base game emits is `core:*`.
inline const char* kCoreNamespace() { return "core"; }

// Load-order rank of the stock/base layer. Mods use ranks > 0 (their index in
// the mod load order). Higher rank overrides lower rank.
inline int coreLoadOrder() { return 0; }

// --- charset predicates -----------------------------------------------------

inline bool idIsLowerAlpha(char c) { return c >= 'a' && c <= 'z'; }
inline bool idIsDigit(char c)      { return c >= '0' && c <= '9'; }

// A body char (after the leading letter): lowercase letter, digit, or '_'.
inline bool idIsBodyChar(char c) {
    return idIsLowerAlpha(c) || idIsDigit(c) || c == '_';
}

// ASCII-lowercase a single char (leaves non-uppercase untouched).
inline char idToLower(char c) {
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

// --- segment validation -----------------------------------------------------
//
// Validate a namespace or name segment against its charset + length rule.
// `maxLen` is 32 for namespace, 64 for name. The segment must already be
// lowercase-canonical (call canonicalize first if unsure). Empty -> invalid.
inline bool isValidSegment(const std::string& seg, size_t maxLen) {
    if (seg.empty() || seg.size() > maxLen) return false;
    if (!idIsLowerAlpha(seg[0])) return false;   // must start with a letter
    for (size_t i = 1; i < seg.size(); ++i) {
        if (!idIsBodyChar(seg[i])) return false;
    }
    return true;
}

inline bool isValidNamespace(const std::string& ns) { return isValidSegment(ns, 32); }
inline bool isValidName(const std::string& nm)      { return isValidSegment(nm, 64); }

// --- parse / canonicalize ---------------------------------------------------

struct QualifiedId {
    std::string ns;    // canonical namespace (never empty on a valid id)
    std::string name;  // canonical name
    bool        valid = false;

    // Canonical `namespace:name` string. Empty when !valid.
    std::string str() const { return valid ? (ns + ":" + name) : std::string(); }

    bool isCore() const { return valid && ns == kCoreNamespace(); }

    bool operator==(const QualifiedId& o) const {
        return valid == o.valid && ns == o.ns && name == o.name;
    }
};

// Parse an id string into a canonical QualifiedId.
//   "core:hunchback"      -> {core, hunchback}
//   "MyMod:UrbanMech"     -> {mymod, urbanmech}   (lowercased)
//   "hunchback"           -> {core, hunchback}    (bare name -> core: reserved)
//   ""  / "a:b:c" / bad-charset / over-length -> valid=false
// At most ONE ':' is allowed. Leading/trailing/empty segments are invalid.
inline QualifiedId parseQualifiedId(const std::string& raw) {
    QualifiedId q;
    if (raw.empty()) return q;

    // Lowercase-canonicalize first, then split on the single ':'.
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) s.push_back(idToLower(c));

    size_t colon = s.find(':');
    if (colon == std::string::npos) {
        // Bare name -> core namespace (stock reserved default).
        if (!isValidName(s)) return q;
        q.ns = kCoreNamespace();
        q.name = s;
        q.valid = true;
        return q;
    }

    // Reject a second ':' — exactly one separator allowed.
    if (s.find(':', colon + 1) != std::string::npos) return q;

    std::string ns = s.substr(0, colon);
    std::string nm = s.substr(colon + 1);
    if (!isValidNamespace(ns) || !isValidName(nm)) return q;

    q.ns = ns;
    q.name = nm;
    q.valid = true;
    return q;
}

// True iff `raw` is a well-formed qualified id (bare names accepted -> core:).
inline bool isValidQualifiedId(const std::string& raw) {
    return parseQualifiedId(raw).valid;
}

// Canonical form of `raw`, or empty string when invalid.
inline std::string canonicalizeQualifiedId(const std::string& raw) {
    return parseQualifiedId(raw).str();
}

// A mod authoring into `core:` is a policy violation (stock is reserved). This
// is the single predicate every author-time validator / modlint should call.
// Returns true when the id is valid AND does NOT sit in the reserved namespace.
inline bool isModAuthorable(const std::string& raw) {
    QualifiedId q = parseQualifiedId(raw);
    return q.valid && !q.isCore();
}

// --- deterministic override precedence --------------------------------------
//
// Given two definitions of the SAME canonical id from two sources at known load
// orders, return true if `candidate` (at `candidateOrder`) overrides `incumbent`
// (at `incumbentOrder`). Rule: strictly-higher load order wins; equal orders do
// NOT override (first writer at a given rank stays — sources at the same rank are
// a config error the caller should have de-duplicated). `core` is load order 0,
// so any mod (order > 0) overrides stock, and later mods override earlier mods.
inline bool overrides(int candidateOrder, int incumbentOrder) {
    return candidateOrder > incumbentOrder;
}

// Resolve the winning load order for an id defined at several load orders.
// Returns the MAXIMUM order (last-loaded wins). `count == 0` -> coreLoadOrder().
inline int resolveWinningOrder(const int* orders, size_t count) {
    if (count == 0 || !orders) return coreLoadOrder();
    int best = orders[0];
    for (size_t i = 1; i < count; ++i) {
        if (orders[i] > best) best = orders[i];
    }
    return best;
}

} // namespace mc2id

#endif // QUALIFIED_ID_H
