#ifndef UI_PROFILE_H
#define UI_PROFILE_H
// UI-PROFILE-1 (Phase 2, slice 6 of the UNIT-PROFILE arc).
//
// PURE FACET. A unit's PRESENTATION-UI layer: how it appears in menus/HUD --
// icon-file / portrait / minimap / silhouette by NAME, plus an optional name /
// encyclopedia string-id override. The recon (PRESENTATION-LOADOUT-STRANGLER-
// RECON-1.md "UIProfile seams") found UI assets joined by THREE separate index
// spaces today: atlas COLUMN index (Mover::getIconPictureIndex), filename-based
// mechlab icons (LogisticsChassis::iconFileNames[3]), and numeric IDS_ resource
// ids (getNameID / getEncyclopediaID) into a hand-built mc2res.dll. A UIProfile
// names each asset so the accessor seams can prefer a declared value over the
// legacy index/filename, decoupling the three spaces -- WITHOUT touching the
// icon-atlas blit math (the mechicon.cpp unitIconX/Y * s_MechTextures->width
// column arithmetic is a known CANARY: only WHICH slot/asset is selected may be
// overridden, never the blit layout).
//
// Header-only, GL/IO-free (fixed char buffers + a couple of longs), so it
// compiles into the offline `mc2_tests` target and drags no engine/GL/IO code,
// exactly like hardpoint.h / damage_schema.h. EMPTY / sentinel means "unmapped"
// -> the accessor seam keeps its legacy lookup (stock parity: stock units
// declare no UIProfile, so every field is empty and legacy behavior is
// byte-identical).

// -------------------------------------------------------------------------
// UIAssetName -- a fixed, trivially-copyable string field naming a UI asset.
// -------------------------------------------------------------------------
// Mirrors HardpointSocketName's shape (a small char buffer, no heap) so the
// facet stays a POD safe in the offline test target and any future data blob.
// Empty (`v[0]=='\0'`) is the "unmapped" sentinel the accessor seams test.
enum { UI_ASSET_NAME_CAP = 64 };   // includes the NUL terminator

struct UIAssetName {
    char v[UI_ASSET_NAME_CAP];
    UIAssetName() { v[0] = '\0'; }
    explicit UIAssetName(const char* s) {
        int i = 0;
        if (s) { for (; s[i] && i < UI_ASSET_NAME_CAP - 1; ++i) v[i] = s[i]; }
        v[i] = '\0';
    }
    const char* c_str() const { return v; }
    bool empty() const { return v[0] == '\0'; }
};

// The sentinel a string-id field carries when it declares NO override. Legacy
// resource ids are non-negative (IDS_ values); -1 is never a real id, so it is
// an unambiguous "use the legacy id" marker at the accessor seam.
enum { UI_STRING_ID_UNSET = -1 };

// -------------------------------------------------------------------------
// UIProfile -- the per-unit UI facet.
// -------------------------------------------------------------------------
// Optional named fields. Each icon/portrait/minimap/silhouette is a filename
// (UIAssetName, empty == unmapped == fall back to iconFileNames[]/atlas). The
// two string ids override the numeric IDS_ name/encyclopedia reads; UNSET (-1)
// == fall back to the legacy nameID / encyclopediaID. The facet OWNS no engine
// state and no atlas math -- it only carries the modder's declared choices.
//
// Slot meaning (maps to the legacy iconFileNames[3] triple + the two id reads):
//   icon        -> small mechlab icon   (legacy iconFileNames[0])
//   portrait    -> large mechlab icon / unit portrait (legacy iconFileNames[2])
//   minimap     -> minimap / HUD marker asset (no single legacy field today)
//   silhouette  -> medium mechlab icon / silhouette (legacy iconFileNames[1])
//   nameStringId       -> overrides getNameID() (chassisNameID / nameID)
//   encyclopediaStringId -> overrides getEncyclopediaID()
struct UIProfile {
    UIAssetName icon;         // small icon file    (legacy iconFileNames[0])
    UIAssetName portrait;     // large icon/portrait (legacy iconFileNames[2])
    UIAssetName minimap;      // minimap / HUD marker
    UIAssetName silhouette;   // medium icon/silhouette (legacy iconFileNames[1])

    long nameStringId         = UI_STRING_ID_UNSET;  // overrides legacy nameID
    long encyclopediaStringId = UI_STRING_ID_UNSET;  // overrides legacy encyclopediaID
};

// -------------------------------------------------------------------------
// Pure accessor-override helpers -- the ONE decision each seam makes.
// -------------------------------------------------------------------------
// Every seam is the same shape: "if the profile declared a value, use it; else
// legacy." Factored here so the verdict is engine/GL/IO-free and doctest-covered
// in isolation; the engine seam only supplies the legacy value and consumes the
// result. Stock units declare nothing -> every override returns the legacy value
// -> byte-identical parity.

// Filename seam: return the declared asset filename if non-empty, else `legacy`.
// Used at getSmallIconFileName / getMediumIconFileName / getLargeIconFileName.
inline const char* chooseUIAsset(const UIAssetName& declared, const char* legacy) {
    return declared.empty() ? legacy : declared.c_str();
}

// String-id seam: return the declared id if set (>= 0), else `legacy`. Used at
// getNameID / getEncyclopediaID.
inline long chooseUIStringId(long declared, long legacy) {
    return (declared == UI_STRING_ID_UNSET) ? legacy : declared;
}

#endif // UI_PROFILE_H
