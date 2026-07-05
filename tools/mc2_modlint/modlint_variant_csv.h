#ifndef MC2_MODLINT_VARIANT_CSV_H
#define MC2_MODLINT_VARIANT_CSV_H
// UNIT-VARIANT-ADD-1 (S4 of the ADD-path ladder, .claude/ADD-PATH-RECON-1.md).
//
// Pure, offline validator for the LEGACY per-chassis variant CSV
// (data/objects/<mech>.csv) — the file a mod whole-file-shadows to ADD a new
// unit variant of an existing chassis. The 23+97n row-block layout is fragile
// and a modder WILL botch it, so every rule below mirrors the EXACT engine
// consumer semantics (verified in this worktree):
//
//   * enumeration: LogisticsData::initVariants (code/logisticsdata.cpp:565-575)
//     walks `while (NO_ERR == readString(row=23+97n, col=2))` — the FIRST
//     empty/absent VariantName cell TERMINATES the walk; later blocks are
//     silently invisible.
//   * CSVFile rows/cols are 1-BASED; the column split is a NAIVE strstr(",")
//     (mclib/csvfile.cpp:687-733, getNextWord :112) — quotes are NOT
//     column-quoting (copyString truncates at '"'), so this parser is
//     deliberately naive too (engine-faithful, not RFC-4180).
//   * an empty cell reads as FAILURE for readString (getNextWord -1) and as
//     value 0 for readLong/readUChar.
//   * loadout: rows 26+off..96+off col 5 = compbas ordinal
//     (LogisticsVariant::init, code/logisticsvariant.cpp:429-442; in-sim twin
//     BattleMech::init, code/mech.cpp:1718). 0xff = empty-slot sentinel.
//     Unresolvable ids are DROPPED SILENTLY by both consumers.
//   * armor/internal-structure: rows 114+off / 115+off (code/mech.cpp:1656-1686,
//     1967) — a truncated block reads armor 0 => mech spawns paper-thin.
//   * getVariant(file, num) masks the variant index to 8 bits
//     (code/logisticsdata.cpp:1224 `(ID>>16)&0xff`) => >256 blocks collide.
//
// Pure by construction: no IO, no engine includes beyond the shared
// ModderFinding shape — doctest-covered in mc2_tests and reused verbatim by the
// mc2_modlint CLI (--variant-csv and the --mod chassis-shadow sweep).
#include <cctype>
#include <set>
#include <string>
#include <vector>

#include "modder_finding.h"

// ---------------------------------------------------------------- parsing ---

// One parsed CSV: rows[r][c], both 0-based internally; use cell(row,col) with
// the engine's 1-based coordinates.
struct VariantCsvTable {
    std::vector<std::vector<std::string>> rows;

    // 1-based, engine-faithful: absent row/col -> empty string.
    std::string cell(size_t row1, size_t col1) const {
        if (row1 == 0 || row1 > rows.size()) return std::string();
        const std::vector<std::string>& r = rows[row1 - 1];
        if (col1 == 0 || col1 > r.size()) return std::string();
        return r[col1 - 1];
    }
    size_t rowCount() const { return rows.size(); }
};

inline std::string variantCsvTrim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

inline std::string variantCsvLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Naive comma split (engine-faithful: quotes are NOT column-quoting).
inline VariantCsvTable parseVariantCsv(const std::string& text) {
    VariantCsvTable t;
    std::vector<std::string> row;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n') {
            row.push_back(variantCsvTrim(cur));
            t.rows.push_back(row);
            row.clear();
            cur.clear();
        } else if (c == ',') {
            row.push_back(variantCsvTrim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty() || !row.empty()) {
        row.push_back(variantCsvTrim(cur));
        t.rows.push_back(row);
    }
    return t;
}

// Heuristic the --mod sweep uses to tell a chassis CSV (atlas.csv, ...) from
// compbas.csv / pilots.csv / buildings.csv: chassis CSVs carry a literal
// "VariantName" label in column 1.
inline bool looksLikeChassisCsv(const VariantCsvTable& t) {
    for (const auto& r : t.rows)
        if (!r.empty() && variantCsvLower(variantCsvTrim(r[0])) == "variantname")
            return true;
    return false;
}

// compbas.csv -> set of POPULATED ordinals (col 1 = id, col 2 = type; a row is
// free/undefined when type == "undefined", cmponent.cpp initEXCEL semantics).
inline std::set<int> parseCompbasPopulatedIds(const std::string& text) {
    std::set<int> ids;
    VariantCsvTable t = parseVariantCsv(text);
    for (const auto& r : t.rows) {
        if (r.size() < 2) continue;
        const std::string idStr = variantCsvTrim(r[0]);
        if (idStr.empty()) continue;
        bool numeric = true;
        for (char c : idStr) if (!std::isdigit((unsigned char)c)) { numeric = false; break; }
        if (!numeric) continue;
        if (variantCsvLower(variantCsvTrim(r[1])) != "undefined")
            ids.insert(std::stoi(idStr));
    }
    return ids;
}

// ------------------------------------------------------------- validation ---

// Stable reason tokens (VARC_ = variant-csv), same convention as UPV_/EQV_.
#define MC2_VARC_NO_VARIANT_BLOCK       "VARC_NO_VARIANT_BLOCK"
#define MC2_VARC_OFF_STRIDE             "VARC_OFF_STRIDE"
#define MC2_VARC_UNREACHABLE_BLOCK      "VARC_UNREACHABLE_BLOCK"
#define MC2_VARC_DUPLICATE_NAME         "VARC_DUPLICATE_NAME"
#define MC2_VARC_COMPONENT_ID_MALFORMED "VARC_COMPONENT_ID_MALFORMED"
#define MC2_VARC_COMPONENT_ID_UNKNOWN   "VARC_COMPONENT_ID_UNKNOWN"
#define MC2_VARC_BLOCK_TRUNCATED        "VARC_BLOCK_TRUNCATED"
#define MC2_VARC_TOO_MANY_VARIANTS      "VARC_TOO_MANY_VARIANTS"

// The engine's block geometry, in ONE place.
inline int variantNameRow(int block)  { return 23 + 97 * block; }   // col 2
inline int variantItemRowFirst(int block) { return 26 + 97 * block; }  // ..96+off, col 5
inline int variantItemRowLast(int block)  { return 96 + 97 * block; }
inline int variantArmorRow(int block) { return 115 + 97 * block; }  // CurrentArmorPoints

// Validate one chassis CSV the way the engine will consume it.
// `validComponentIds`: populated compbas ordinals; pass nullptr to skip the
// loadout-resolution check (no compbas available offline).
// `fileLabel`/`modId` flow into the shared ModderFinding context.
inline std::vector<ModderFinding> validateVariantCsvAdd(
        const VariantCsvTable& t,
        const std::string& fileLabel,
        const std::string& modId,
        const std::set<int>* validComponentIds) {
    std::vector<ModderFinding> out;
    auto mk = [&](const char* reason, const std::string& msg) {
        ModderFinding f;
        f.reason = reason;
        f.message = msg;
        f.source = modId;
        f.file = fileLabel;
        return f;
    };

    // 1) Enumerate exactly like initVariants: stop at first empty name cell.
    std::vector<std::pair<int, std::string>> blocks;   // {block, name}
    int block = 0;
    while (true) {
        const int row = variantNameRow(block);
        if ((size_t)row > t.rowCount()) break;
        const std::string name = t.cell(row, 2);
        if (name.empty()) break;
        blocks.push_back({block, name});
        ++block;
    }
    const int stopRow = variantNameRow(block);   // first row the walk rejected

    if (blocks.empty()) {
        ModderFinding f = mk(MC2_VARC_NO_VARIANT_BLOCK,
            "no variant block found -- the chassis will load with ZERO variants");
        f.field = "row 23 col 2";
        f.expected = "non-empty VariantName at row 23, column 2";
        f.actual = (t.rowCount() < 23) ? "file has fewer than 23 rows" : "empty cell";
        f.fallback = "chassis absent from mech bay";
        out.push_back(f);
    }

    // 2) Every "VariantName"-labeled row must sit EXACTLY on the 23+97n grid,
    //    and on-grid rows past the enumeration stop are unreachable.
    for (size_t r1 = 1; r1 <= t.rowCount(); ++r1) {
        if (variantCsvLower(t.cell(r1, 1)) != "variantname") continue;
        const int r = (int)r1;
        if ((r - 23) % 97 != 0) {
            // nearest legal rows
            const int n = (r - 23 + 48) / 97;               // rounded block
            const int nearest = variantNameRow(n < 0 ? 0 : n);
            ModderFinding f = mk(MC2_VARC_OFF_STRIDE,
                "VariantName row is OFF the 23+97*n grid -- the engine will never read this block");
            f.field = "row " + std::to_string(r) + " col 1";
            f.expected = "VariantName label at row " + std::to_string(nearest) +
                         " (nearest legal 23+97*n row)";
            f.actual = "row " + std::to_string(r) +
                       " (offset " + std::to_string((r - 23) % 97) + " into a block)";
            f.fallback = "block silently invisible";
            out.push_back(f);
        } else if (r >= stopRow) {
            ModderFinding f = mk(MC2_VARC_UNREACHABLE_BLOCK,
                "variant block is on-grid but UNREACHABLE -- enumeration already stopped at an earlier empty name row");
            f.field = "row " + std::to_string(r) + " col 2";
            f.expected = "non-empty VariantName at row " + std::to_string(stopRow) +
                         " (enumeration stops at the first empty name cell)";
            f.actual = (t.cell((size_t)r, 2).empty()
                        ? std::string("empty name cell")
                        : "name '" + t.cell((size_t)r, 2) + "'");
            f.fallback = "block silently invisible";
            out.push_back(f);
        }
    }

    // 3) Duplicate variant names within the chassis (getVariant(name) and the
    //    post-mission inventory re-add resolve by NAME -- first match wins).
    for (size_t a = 0; a < blocks.size(); ++a) {
        for (size_t b = a + 1; b < blocks.size(); ++b) {
            if (variantCsvLower(blocks[a].second) == variantCsvLower(blocks[b].second)) {
                ModderFinding f = mk(MC2_VARC_DUPLICATE_NAME,
                    "duplicate variant name within one chassis -- name-keyed lookups (save recovery, mechlab) resolve to the FIRST block only");
                f.id = blocks[a].second;
                f.field = "row " + std::to_string(variantNameRow(blocks[b].first)) + " col 2";
                f.expected = "variant name unique per chassis";
                f.actual = "'" + blocks[b].second + "' already used at row " +
                           std::to_string(variantNameRow(blocks[a].first));
                f.fallback = "second block shadowed by the first";
                out.push_back(f);
            }
        }
    }

    // 4) Per enumerated block: loadout refs + truncation.
    for (const auto& bk : blocks) {
        const int n = bk.first;
        for (int r = variantItemRowFirst(n); r <= variantItemRowLast(n); ++r) {
            const std::string label = variantCsvLower(t.cell((size_t)r, 1));
            if (label.rfind("item", 0) != 0) continue;     // only ItemNMasterId rows
            const std::string idStr = t.cell((size_t)r, 5);
            if (idStr.empty()) continue;                   // empty cell = engine skips
            bool numeric = !idStr.empty();
            for (char c : idStr) if (!std::isdigit((unsigned char)c)) { numeric = false; break; }
            if (!numeric) {
                ModderFinding f = mk(MC2_VARC_COMPONENT_ID_MALFORMED,
                    "loadout CompBas# is not a number -- the engine parses garbage as 0 and drops the slot silently");
                f.id = bk.second;
                f.field = "row " + std::to_string(r) + " col 5";
                f.expected = "compbas ordinal 0..254, or 255 (0xff) for empty slot";
                f.actual = "'" + idStr + "'";
                f.fallback = "slot dropped";
                out.push_back(f);
                continue;
            }
            const long id = std::stol(idStr);
            if (id == 255) continue;                       // 0xff empty-slot sentinel
            const bool outOfRange = (id < 0 || id > 254);
            const bool unknown = !outOfRange && validComponentIds &&
                                 validComponentIds->find((int)id) == validComponentIds->end();
            if (outOfRange || unknown) {
                ModderFinding f = mk(MC2_VARC_COMPONENT_ID_UNKNOWN,
                    "loadout CompBas# does not resolve to a populated component -- both consumers (mechlab + in-sim) drop it silently");
                f.id = bk.second;
                f.field = "row " + std::to_string(r) + " col 5";
                f.expected = outOfRange ? "ordinal 0..254 (255 reserved as empty-slot sentinel)"
                                        : "an ordinal populated in compbas.csv";
                f.actual = idStr;
                f.fallback = "slot dropped";
                out.push_back(f);
            }
        }
        if ((size_t)variantArmorRow(n) > t.rowCount()) {
            ModderFinding f = mk(MC2_VARC_BLOCK_TRUNCATED,
                "variant block is truncated before its armor rows -- the mech would spawn with 0 armor");
            f.id = bk.second;
            f.field = "row " + std::to_string(variantArmorRow(n)) + " (CurrentArmorPoints)";
            f.expected = "file extends to row " + std::to_string(variantArmorRow(n)) +
                         " for variant block " + std::to_string(n);
            f.actual = "file ends at row " + std::to_string((int)t.rowCount());
            f.fallback = "armor/internal-structure read as 0";
            out.push_back(f);
        }
    }

    // 5) getVariant() masks the per-chassis variant index to 8 bits.
    if (blocks.size() > 256) {
        ModderFinding f = mk(MC2_VARC_TOO_MANY_VARIANTS,
            "more than 256 variants on one chassis -- getVariant() masks the index to 8 bits, so indices collide");
        f.field = "row " + std::to_string(variantNameRow(256)) + " col 2";
        f.expected = "at most 256 variant blocks per chassis";
        f.actual = std::to_string(blocks.size()) + " blocks";
        f.fallback = "index-keyed lookups resolve to the wrong variant";
        out.push_back(f);
    }

    return out;
}

// Formatter (matches formatUnitFindings/formatEquipmentFindings shape).
inline std::string formatVariantCsvFindings(const std::string& label,
                                            const std::vector<ModderFinding>& findings,
                                            const std::string& source = "") {
    std::string out = "chassis-csv " + (label.empty() ? std::string("<unnamed>") : label);
    if (!source.empty()) out += " [" + source + "]";
    if (findings.empty()) {
        out += ": OK (0 findings)\n";
        return out;
    }
    out += ": " + std::to_string(findings.size()) + " finding(s)\n";
    for (const ModderFinding& f : findings)
        out += "  " + formatModderFinding(f) + "\n";
    return out;
}

#endif // MC2_MODLINT_VARIANT_CSV_H
