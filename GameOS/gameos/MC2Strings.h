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
#include "mc2_czstring.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MC2Strings
{
    // Decodes the escapes of a quoted FIT string value (\n \r \t \\ \"); the stock backslash line-break code ("\\\n") loses its backslash.
    std::string UnescapeFitString(std::string_view value);

    /*
        Typed resource messages. The shipped catalogs hold printf templates ("%ld MAX PLAYERS");
        a template is imported through a restricted, signature-checked parser instead of being
        handed to sprintf. Accepted: %d %i %u %x %X %o (Int), %s (Str), %f %F %e %E %g %G (Float),
        flags "-+ #0", numeric width and precision, and the h/hh/l/ll length modifiers; %% is a
        literal percent. Rejected: %n, %p, %c, '*', positional '$', unknown conversions, any
        conversion whose kind differs from the message signature, and more conversions than the
        signature has arguments. Braces are ordinary text.
    */
    enum class ArgKind : unsigned char { Int, Str, Float };

    struct MessageArg
    {
        ArgKind kind = ArgKind::Int;
        long long i = 0;          // Int: the value widened to 64 bits (sign-extended for signed types)
        double d = 0;             // Float
        std::string_view s;       // Str
    };

    enum class IntWidth : unsigned char { Int, Char, Short, Long, LongLong };

    struct MessagePiece
    {
        std::string literal;      // text emitted before the conversion
        int arg = -1;             // argument index; -1 for the trailing literal
        char conv = 0;
        IntWidth width = IntWidth::Int;
        std::string spec;         // flags, width and precision, e.g. "-5.2"
    };

    struct CompiledMessage
    {
        std::vector<MessagePiece> pieces;
    };

    // False (and `why` set) when `text` is malformed or does not match `signature`.
    bool CompileMessage(std::string_view text, std::span<const ArgKind> signature, CompiledMessage& out, std::string* why = nullptr);

    // Renders like the equivalent correctly-typed printf call, including platform `long` width.
    std::string FormatMessage(const CompiledMessage& message, std::span<const MessageArg> args);

    // Counts catalog changes (initial load, each overlay file, alias reload). Template caches key on it.
    unsigned long long Revision();

    // The legacy-id text as the stock (pre-mod) catalog had it, or nullptr.
    const char* GetStockByLegacyId(long legacyId);

    // Load one overlay FIT into the live catalog (the mod path and tests).
    void LoadOverlayFile(mc2::czstring path);

    // The catalog message for a legacy id, compiled once per catalog revision. A malformed or
    // type-mismatched override falls back to the stock template with one log line.
    std::string FormatLegacyMessage(long legacyId, std::span<const ArgKind> signature, std::span<const MessageArg> args);

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

    // Inject mod-overlay text FITs into the already-loaded catalog.
    // Call this from mechcmd2.cpp after InitModSearchPaths() has populated the
    // mod index — MC2Strings::Load() may have fired first with an empty index.
    void LoadModFits();

    const char* Get(mc2::czstring key);

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
    const char* GetByResourceSymbol(mc2::czstring resourceSymbol);

    /*
        Menu binding lookup is included because game/viewer menus may need the
        same bridge, but MFC dialog/control localization stays outside the
        engine pass.
    */
    const char* GetMenuText(mc2::czstring menuId, mc2::czstring commandId);
    const char* GetMenuTextByIds(unsigned int menuId, unsigned int commandId);

    bool HasKey(mc2::czstring key);
    bool HasLegacyId(long legacyId);
    bool HasResourceId(unsigned int resourceId);
    bool HasResourceSymbol(mc2::czstring resourceSymbol);
    bool HasMenuBinding(mc2::czstring menuId, mc2::czstring commandId);

    const char* MissingKey(mc2::czstring key);
    const char* MissingLegacyId(long legacyId);
    const char* MissingResourceId(unsigned int resourceId);
    const char* MissingResourceSymbol(mc2::czstring resourceSymbol);
    const char* MissingMenuBinding(mc2::czstring menuId, mc2::czstring commandId);
}

#endif /* MC2_STRINGS_H */
