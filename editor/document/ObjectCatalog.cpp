#include "ObjectCatalog.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <memory>
#include <unordered_set>

namespace editor::document {
std::vector<std::uint32_t> ObjectCatalog::navMarkerObjects() const
{
    std::vector<std::uint32_t> result;
    for (const auto& entry : entries_)
        if (entry.family == ObjectFamily::marker && identifier(entry.special) == "navmarker" &&
            std::ranges::find(result, entry.id) == result.end()) result.push_back(entry.id);
    return result;
}
namespace {
std::string trim(std::string_view text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == text.npos) return {};
    return std::string(text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1));
}
std::string text(std::span<const std::byte> bytes)
{
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::vector<std::string_view> lines(const std::string& text)
{
    std::vector<std::string_view> result;
    std::size_t start{};
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        result.push_back(std::string_view(text).substr(start, end == text.npos ? text.size() - start : end - start));
        if (end == text.npos) break;
        start = end + 1;
    }
    return result;
}
std::optional<std::vector<std::string>> csv(std::string_view line)
{
    std::vector<std::string> cells;
    std::string cell;
    bool quoted{}, closed{};
    for (std::size_t i = 0; i < line.size(); ++i) {
        const auto c = line[i];
        if (quoted) {
            if (c != '"') cell += c;
            else if (i + 1 < line.size() && line[i + 1] == '"') { cell += c; ++i; }
            else { quoted = false; closed = true; }
        } else if (c == ',') { cells.push_back(trim(cell)); cell.clear(); closed = false; }
        else if (c == '"' && trim(cell).empty() && !closed) { cell.clear(); quoted = true; }
        else if (c == '"' || (closed && c != ' ' && c != '\t' && c != '\r')) return {};
        else cell += c;
    }
    if (quoted) return {};
    cells.push_back(trim(cell));
    return cells;
}
template<class T> bool number(std::string_view text, T& value)
{
    int base = 10;
    if constexpr (std::is_integral_v<T>) if (text.starts_with("0x") || text.starts_with("0X")) { base = 16; text.remove_prefix(2); }
    const auto parsed = [&] {
        if constexpr (std::is_integral_v<T>) return std::from_chars(text.data(), text.data() + text.size(), value, base);
        else return std::from_chars(text.data(), text.data() + text.size(), value);
    }();
    const auto [end, error] = parsed;
    if (error != std::errc{} || end != text.data() + text.size()) return false;
    if constexpr (std::is_floating_point_v<T>) return std::isfinite(value);
    return true;
}
std::optional<ObjectFamily> family(std::int64_t value, std::string_view special)
{
    if (value != 0 && value != 1 && value != 2 && value != 3 && value != 11 && value != 15 && value != 18 && value != 21 && value != 22 && value != 23) return {};
    if (special == "navmarker" || special == "dropzone") return ObjectFamily::marker;
    if (special == "bridge") return ObjectFamily::bridge;
    if (special == "spotlight") return ObjectFamily::light;
    switch (value) {
    case 0: case 18: return ObjectFamily::tree;
    case 1: case 11: return ObjectFamily::building;
    case 2: return ObjectFamily::mech;
    case 3: return ObjectFamily::vehicle;
    case 15: return ObjectFamily::bridge;
    case 21: return ObjectFamily::turret;
    case 22: return ObjectFamily::gate;
    case 23: return ObjectFamily::light;
    default: return {};
    }
}
std::uint32_t word(std::span<const std::byte> bytes, std::size_t p)
{
    std::uint32_t result{};
    for (unsigned i = 0; i < 4; ++i) result |= std::to_integer<std::uint32_t>(bytes[p + i]) << (8 * i);
    return result;
}
void put(std::vector<std::byte>& bytes, std::size_t p, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[p + i] = static_cast<std::byte>(value >> (8 * i));
}
std::expected<Syntax, LoadError> packetFit(const ResolvedFile& pak, std::uint32_t id)
{
    const auto bad = [](std::string reason) { return std::unexpected(LoadError{"object2.pak", std::move(reason), 0}); };
    if (pak.bytes.size() < 8) return bad("invalid_packet_table");
    const auto tableEnd = word(pak.bytes, 4);
    if (tableEnd < 12 || tableEnd > pak.bytes.size() || tableEnd % 4 || id >= tableEnd / 4 - 2) return bad("missing_packet");
    const auto entry = word(pak.bytes, 8 + id * 4);
    const auto storage = entry >> 29;
    if (storage != 0 && storage != 1 && storage != 2 && storage != 4) return bad(storage == 7 ? "missing_definition" : "unsupported_compression");
    const auto begin = entry & 0x1fffffff;
    const auto end = id + 1 == tableEnd / 4 - 2 ? pak.bytes.size() : word(pak.bytes, 12 + id * 4) & 0x1fffffff;
    if (begin < tableEnd || end < begin || end > pak.bytes.size() || end - begin > packetByteLimit) return bad("packet_bounds");
    // A bounded single-packet archive isolates bad definitions from healthy catalog rows.
    std::vector<std::byte> bytes(28);
    put(bytes, 0, 0xfeedface); put(bytes, 4, 28); put(bytes, 8, 28 | (entry & 0xe0000000));
    bytes.insert(bytes.end(), pak.bytes.begin() + begin, pak.bytes.begin() + end);
    for (std::size_t i = 1; i < 5; ++i) put(bytes, 8 + i * 4, 0xe0000000 | static_cast<std::uint32_t>(bytes.size()));
    auto decoded = decodePak(std::move(bytes));
    if (!decoded) return std::unexpected(decoded.error());
    const auto original = text(decoded->packets[0].decoded);
    if (original.find('\0') != original.npos || original.size() > fitByteLimit) return bad("invalid_definition_text");
    // Catalog validation covers catalog facts; unused combat tuning is the game reader's domain.
    std::string projection;
    for (auto line : lines(original)) {
        const auto content = trim(line);
        if (content.empty()) continue;
        if (content.starts_with('[') || identifier(content).starts_with("fitini") || identifier(content).starts_with("fitend")) projection += content + '\n';
        else {
            const auto equal = content.find('=');
            if (equal == content.npos) continue;
            const auto left = trim(std::string_view(content).substr(0, equal));
            const auto split = left.find_first_of(" \t");
            if (split == left.npos) continue;
            const auto name = identifier(trim(std::string_view(left).substr(split + 1)));
            if (name == "objecttypenum" || name == "name" || name == "appearancename" || name == "profilename" ||
                name == "hovercraft" || name == "lowtemplate" || name == "hightemplate" || name == "blockslineoffire") {
                auto declaration = content;
                if (name == "hovercraft" || name == "blockslineoffire") {
                    const auto boolean = identifier(trim(std::string_view(content).substr(equal + 1, content.find("//", equal) - equal - 1)));
                    if (boolean == "t" || boolean == "f") declaration = left + " = " + (boolean == "t" ? "TRUE" : "FALSE");
                }
                if ((name == "lowtemplate" || name == "hightemplate") && identifier(std::string_view(content).substr(0, split)) == "l") declaration.replace(0, split, "ul");
                projection += declaration + '\n';
            }
        }
    }
    return decodeFit(std::move(projection));
}
const Field* field(const Syntax& syntax, std::string_view sectionName, std::string_view fieldName)
{
    for (const auto& section : syntax.sections) if (identifier(section.name) == identifier(sectionName))
        for (const auto id : section.fields) for (const auto& f : syntax.fields)
            if (id == f.id && identifier(f.name) == identifier(fieldName)) return &f;
    return nullptr;
}
template<class T> std::optional<T> value(const Syntax& syntax, std::string_view section, std::string_view name)
{
    const auto* f = field(syntax, section, name);
    if (!f || f->value.size() != 1) return {};
    if (const auto* result = std::get_if<T>(&f->value[0])) return *result;
    return {};
}
bool stemValid(std::string_view stem)
{
    return !stem.empty() && stem.size() < 64 && stem.find_first_of("/\\:\r\n\0", 0, 6) == stem.npos && stem != "." && stem != "..";
}
}

std::string_view familyName(ObjectFamily family)
{
    switch (family) {
    case ObjectFamily::mech: return "mech";
    case ObjectFamily::vehicle: return "vehicle";
    case ObjectFamily::building: return "building";
    case ObjectFamily::turret: return "turret";
    case ObjectFamily::tree: return "tree";
    case ObjectFamily::bridge: return "bridge";
    case ObjectFamily::gate: return "gate";
    case ObjectFamily::light: return "light";
    case ObjectFamily::marker: return "marker";
    }
    return "unknown";
}

ObjectCatalog ObjectCatalog::load(const FileResolver& resolver, Diagnostic log, ResourceName names)
{
    ObjectCatalog result;
    const auto reject = [&](std::size_t row, std::string reason) {
        ++result.rejections_;
        if (log) log("[EDITOR_CATALOG_REJECT v1] row=" + std::to_string(row) + " reason=" + reason);
    };
    auto enumeration = resolver.read("data/art/Buildings.csv");
    auto definitions = resolver.read("data/objects/object2.pak");
    if (!enumeration) { reject(0, "Buildings.csv:" + enumeration.error().reason); return result; }
    if (!definitions) { reject(0, "object2.pak:" + definitions.error().reason); return result; }
    if (enumeration->bytes.size() > fitByteLimit || definitions->bytes.size() > pakByteLimit) { reject(0, "catalog_size_limit"); return result; }
    if (definitions->bytes.size() >= 8 && word(definitions->bytes, 0) != 0xfeedface) {
        std::uint32_t checksum{};
        for (std::size_t i = 4; i < definitions->bytes.size(); ++i) checksum += std::to_integer<unsigned>(definitions->bytes[i]);
        if (checksum != word(definitions->bytes, 0)) { reject(0, "object2.pak:checksum_mismatch"); return result; }
    }
    if (!names) names = resourceNames(resolver, log);
    const auto content = text(enumeration->bytes);
    const auto rows = lines(content);
    if (rows.size() > nodeLimit) { reject(0, "row_count_limit"); return result; }
    std::unordered_set<std::string> stems;
    for (std::size_t row = 1; row < rows.size(); ++row) {
        if (trim(rows[row]).empty()) continue;
        auto cells = csv(rows[row]);
        if (!cells || cells->size() != 12 || rows[row].size() > 4096 || rows[row].find('\0') != rows[row].npos) { reject(row + 1, "invalid_csv_row"); continue; }
        const auto& c = *cells;
        CatalogEntry entry;
        std::int64_t capturable{}, alignable{}, tac{};
        if (!stemValid(c[0]) || !number(c[1], entry.group) || !number(c[2], entry.nameId) || entry.nameId < 0 ||
            !number(c[4], entry.id) || !number(c[6], capturable) || !number(c[7], alignable) || !number(c[8], tac) ||
            !number(c[11], entry.scale) || entry.scale <= 0 || capturable < 0 || capturable > 1 || alignable < 0 || alignable > 1 || tac < INT32_MIN || tac > UINT32_MAX) {
            reject(row + 1, "invalid_csv_value"); continue;
        }
        const auto csvType = identifier(c[3]);
        if (csvType != "mech" && csvType != "vehicle" && csvType != "building" && csvType != "tree") { reject(row + 1, "invalid_appearance_type"); continue; }
        entry.appearanceType = csvType == "mech" ? CatalogAppearance::mech : csvType == "vehicle" ? CatalogAppearance::vehicle : csvType == "tree" ? CatalogAppearance::tree : CatalogAppearance::building;
        if (stems.contains(identifier(c[0]))) { reject(row + 1, "duplicate_stem"); continue; }
        auto fit = packetFit(*definitions, entry.id);
        if (!fit) { reject(row + 1, fit.error().reason); continue; }
        bool invalidType{};
        for (const auto& fact : fit->fields) {
            const auto name = identifier(fact.name);
            const bool string = name == "name" || name == "appearancename" || name == "profilename";
            const bool boolean = name == "hovercraft" || name == "blockslineoffire";
            if (fact.value.size() != 1 || (string ? !std::holds_alternative<std::string>(fact.value[0]) :
                boolean ? !std::holds_alternative<bool>(fact.value[0]) : !std::holds_alternative<std::int64_t>(fact.value[0]))) invalidType = true;
        }
        if (invalidType) { reject(row + 1, "invalid_fact_type"); continue; }
        const auto runtime = value<std::int64_t>(*fit, "ObjectClass", "ObjectTypeNum");
        const auto resolvedFamily = runtime ? family(*runtime, identifier(c[5])) : std::nullopt;
        if (!resolvedFamily) { reject(row + 1, "unsupported_or_missing_runtime_class"); continue; }
        entry.runtimeClass = *runtime;
        entry.family = *resolvedFamily;
        entry.stem = c[0]; entry.special = c[5]; entry.icon = c[9];
        entry.capturable = capturable != 0; entry.alignable = alignable != 0; entry.onTacMap = tac != 0;
        entry.tacMapValue = static_cast<std::uint32_t>(tac);
        entry.appearance = value<std::string>(*fit, "ObjectType", "AppearanceName").value_or(entry.stem);
        entry.profile = value<std::string>(*fit, "MechProfile", "ProfileName").value_or("");
        entry.hover = value<bool>(*fit, "General", "HoverCraft").value_or(false);
        for (const auto& section : fit->sections) {
            const auto low = value<std::int64_t>(*fit, section.name, "LowTemplate");
            const auto high = value<std::int64_t>(*fit, section.name, "HighTemplate");
            if (low || high) {
                if (!low || !high || *low < INT32_MIN || *high < INT32_MIN || *low > UINT32_MAX || *high > UINT32_MAX) { entry.scale = 0; break; }
                entry.impassability = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(*high)) << 32) | static_cast<std::uint32_t>(*low);
            }
            if (const auto blocks = value<bool>(*fit, section.name, "BlocksLineOfFire")) entry.blocksLineOfFire = *blocks;
        }
        if (entry.scale == 0) { reject(row + 1, "invalid_template"); continue; }
        if (identifier(entry.special).starts_with("tree") && !number(std::string_view(entry.special).substr(4), entry.forestGroup)) { reject(row + 1, "invalid_forest_group"); continue; }
        if (names) entry.displayName = names(entry.nameId).value_or("");
        if (entry.displayName.empty() || entry.displayName.find("Not defined") != entry.displayName.npos || entry.displayName.find("mc2res.dll:") != entry.displayName.npos)
            entry.displayName = entry.stem + "_" + std::to_string(entry.nameId);
        auto variants = resolver.read("data/objects/" + entry.stem + ".csv");
        if (variants) {
            if (variants->bytes.size() > fitByteLimit) { reject(row + 1, "variant_size_limit"); continue; }
            const auto variantText = text(variants->bytes);
            const auto variantRows = lines(variantText);
            bool malformed{};
            for (unsigned i = 0; i < 16; ++i) {
                const auto index = 22 + 97 * i;
                if (index >= variantRows.size()) continue;
                auto columns = csv(variantRows[index]);
                if (!columns) { malformed = true; break; }
                if (columns->size() > 1 && (*columns)[1].size() > 255) { malformed = true; break; }
                if (columns->size() > 1 && !(*columns)[1].empty()) entry.variants.push_back({i, (*columns)[1]});
            }
            if (malformed) { reject(row + 1, "invalid_variant_csv"); continue; }
            entry.variantFile = CatalogSource{variants->key, variants->layer, variants->source};
        } else if (variants.error().reason != "missing_file") { reject(row + 1, "variant:" + variants.error().reason); continue; }
        entry.enumeration = { enumeration->key, enumeration->layer, enumeration->source };
        entry.definition = { definitions->key, definitions->layer, definitions->source };
        stems.insert(identifier(entry.stem));
        result.entries_.push_back(std::move(entry));
    }
    return result;
}

} // namespace editor::document
