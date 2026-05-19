/***************************************************************
 * FILENAME: MC2Strings.h
 * DESCRIPTION: Declares the shared data/defs string lookup bridge for
 *              MC2R engine text migration.
 *
 * AUTHOR: Microsoft Corporation
 * CREATED: Unknown
 *
 * UPDATED BY: Methuselas
 * UPDATED: 2026-05-05
 *
 * CHANGES:
 * - Added central FIT-backed string lookup declarations for replacing
 *   mc2res.dll string resolution in the engine resource-string path.
 * - Preserved legacy/resource/menu IDs as lookup aliases while keeping
 *   display words in data/defs.
 * - Scoped Phase 1 to engine/game/viewer string lookup; Editor MFC
 *   bindings remain outside this archive.
 ***************************************************************/

#ifndef MC2_STRINGS_H
#define MC2_STRINGS_H

namespace MC2Strings
{
    /*
        Text ownership rule:

        FIT owns the words.
        Code owns behavior.
        Bindings connect legacy/resource/menu/control IDs to shared text keys.
 * - Added LegacyTextAlias support so clean localization can override
 *   mc2res_legacy text without losing direct legacy coverage.

        Exact identical display text should resolve to one shared String key.
        LegacyTextAlias records may redirect old mc2res IDs to clean localization keys.
        Resource IDs, command IDs, control IDs, dialog IDs, and legacy IDs must
        remain distinct because they may drive behavior outside text display.
    */

    bool Load();
    bool IsLoaded();

    const char* Get(const char* key);

    /*
        Legacy mc2res lookup. This is the primary replacement path for
        gos_GetResourceString(..., id).
    */
    const char* GetByLegacyId(long legacyId);

    /*
        Resource aliases are optional. Numeric aliases resolve directly.
        Symbol aliases are kept for diagnostics/future tooling because
        gos_GetResourceString receives numeric IDs.
    */
    const char* GetByResourceId(unsigned int resourceId);
    const char* GetByResourceSymbol(const char* resourceSymbol);

    /*
        Menu binding lookup is included because game/viewer menus may need the
        same bridge, but MFC dialog/control localization stays outside the
        engine pass.
    */
    const char* GetMenuText(const char* menuId, const char* commandId);
    const char* GetMenuTextByIds(unsigned int menuId, unsigned int commandId);

    bool HasKey(const char* key);
    bool HasLegacyId(long legacyId);
    bool HasResourceId(unsigned int resourceId);
    bool HasResourceSymbol(const char* resourceSymbol);
    bool HasMenuBinding(const char* menuId, const char* commandId);

    const char* MissingKey(const char* key);
    const char* MissingLegacyId(long legacyId);
    const char* MissingResourceId(unsigned int resourceId);
    const char* MissingResourceSymbol(const char* resourceSymbol);
    const char* MissingMenuBinding(const char* menuId, const char* commandId);
}

#endif /* MC2_STRINGS_H */
