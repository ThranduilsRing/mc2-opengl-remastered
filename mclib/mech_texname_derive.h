// mclib/mech_texname_derive.h
//
// Pure GLB/material -> MC2 texture-name derivation, extracted
// (GLB-TEXNAME-DERIVE-EXTRACT-1) from assimp_importer.cpp so the name rules are
// exercised game-free by tools/mech_texname_harness/ (no Assimp, no GL, no engine
// types). The Assimp-querying half (GetTexture, embedded "*N" resolve, glTF
// alphaMode / material-name lookup -> wantAlpha) stays in the importer; it calls
// these with the already-resolved source path + the wantAlpha decision.
//
// Catches the "asset exists but the derived atlas name is wrong" class (magenta /
// black mech) — 256-clamp off-by-one, a missed "a_" alpha-cutout prefix, a
// sanitize-rule slip — none of which a 30s tier1 smoke can see (import is gated
// off, and the failure is visual, not a crash).
//
// Firewall: header-only, <string>/<cstring> only, no engine headers.

#ifndef MC2_MECH_TEXNAME_DERIVE_H
#define MC2_MECH_TEXNAME_DERIVE_H

#include <cstring>
#include <string>
#include <string_view>

namespace mech_texname {

// Strip directory to the basename (last '/' or '\\'). Replica of StripPath.
inline std::string stripPath(std::string_view p) {
    const size_t s1 = p.rfind('/');
    const size_t s2 = p.rfind('\\');
    size_t pos;
    if (s1 == std::string::npos) pos = s2;
    else if (s2 == std::string::npos) pos = s1;
    else pos = (s1 > s2) ? s1 : s2;
    return std::string((pos == std::string::npos) ? p : p.substr(pos + 1));
}

// Damage/destruction/UI parts dropped from the intact bind-pose import. Replica
// of skelMeshDropped.
inline bool isDroppedMeshName(const char* n) {
    if (!n) return false;
    std::string s = n;
    for (char& c : s) c = static_cast<char>(((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
    return s.find("_explode") != std::string::npos || s.find("_dmg") != std::string::npos
        || s.find("blip") != std::string::npos || s.find("indc") != std::string::npos
        || s.find("uix") != std::string::npos;
}

// Derive the MC2 texture name from an already-resolved source image path and the
// alpha-cutout decision: strip dir+ext, sanitize to [a-z0-9_-] (else '_'),
// lowercase, clamp so stem+".tga"+NUL fits textureName[256], append ".tga", and
// (if wantAlpha) prefix "a_" (re-clamped). Returns "" if the sanitized stem is
// empty (caller treats as no-texture). Byte-identical to the original tail.
inline std::string deriveName(std::string_view src, bool wantAlpha) {
    std::string stem = stripPath(src);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.erase(dot);

    for (char & i : stem) {
        char c = i;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>((c - 'A' + 'a'));
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                      || c == '_' || c == '-';
        i = ok ? c : '_';
    }
    if (stem.empty()) return std::string();

    const size_t kMaxName = 256;
    const size_t kExt = 4;  // ".tga"
    if (stem.size() + kExt + 1 > kMaxName) stem.erase(kMaxName - kExt - 1);

    stem += ".tga";

    if (wantAlpha && stem.compare(0, 2, "a_") != 0) {
        if (stem.size() + 2 + 1 > 256) stem.erase(256 - 2 - 1);
        stem.insert(0, "a_");
    }
    return stem;
}

}  // namespace mech_texname

#endif  // MC2_MECH_TEXNAME_DERIVE_H
