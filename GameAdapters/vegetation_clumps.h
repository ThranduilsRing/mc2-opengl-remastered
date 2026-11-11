#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>
#include <optional>
#include <string_view>
#include "../3rdparty/include/nlohmann/json.hpp"

namespace mc2::vegetation {
enum class Biome { Temperate, Snow, Desert, Swamp };
struct ClumpRecipe {
    float radius = 9.0f, treeChance = 0.30f, moistureBias = 0.0f;
    int grass = 10;
};
// Invalid or absent mod recipes retain the shipped, bounded recipe.
inline std::optional<ClumpRecipe> readClumpRecipe(const nlohmann::json& doc) {
    if (!doc.is_object() || !doc.contains("clump") || !doc["clump"].is_object()) return std::nullopt;
    const auto& c = doc["clump"];
    for (const char* key : {"radius", "tree_chance", "moisture_bias", "grass"})
        if (!c.contains(key) || !c[key].is_number()) return std::nullopt;
    const float radius = c["radius"].get<float>(), tree = c["tree_chance"].get<float>();
    const float moisture = c["moisture_bias"].get<float>(), grass = c["grass"].get<float>();
    if (!(radius >= 4 && radius <= 16 && tree >= 0 && tree <= 1 &&
          moisture >= -1 && moisture <= 1 && grass >= 6 && grass <= 16)) return std::nullopt;
    return ClumpRecipe{radius, tree, moisture, static_cast<int>(grass)};
}
inline uint32_t mapSeed(std::string_view name) {
    uint32_t seed = 2166136261u;
    for (const char ch : name) seed = (seed ^ static_cast<uint8_t>(ch)) * 16777619u;
    return seed;
}
inline uint32_t clumpHash(int x, int y, uint32_t seed, uint32_t salt = 0) {
    uint32_t h = seed ^ (static_cast<uint32_t>(x) * 0x9e3779b9u)
                      ^ (static_cast<uint32_t>(y) * 0x85ebca6bu) ^ salt;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu;
    return h ^ (h >> 16);
}
inline float clumpRoll(int x, int y, uint32_t seed, uint32_t salt) {
    return static_cast<float>(clumpHash(x, y, seed, salt) >> 8) / 16777216.0f;
}
struct ClumpCentre { float x, y; uint32_t priority; };
inline ClumpCentre clumpCandidate(int x, int y, uint32_t seed) {
    return {static_cast<float>(x) + clumpRoll(x,y,seed,1),
            static_cast<float>(y) + clumpRoll(x,y,seed,2), clumpHash(x,y,seed,3)};
}
// Local-priority blue noise: one jittered candidate per cell, separated by
// 0.65 cells. Neighbours are checked even outside the map, so edges need no RNG.
inline bool clumpCentreAccepted(int x, int y, uint32_t seed) {
    const auto c = clumpCandidate(x,y,seed);
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        if (!dx && !dy) continue;
        const auto n = clumpCandidate(x+dx,y+dy,seed);
        const float sx = n.x-c.x, sy = n.y-c.y;
        const bool first = n.priority < c.priority ||
            (n.priority == c.priority && (dy < 0 || (dy == 0 && dx < 0)));
        if (sx*sx + sy*sy < 0.65f*0.65f && first) return false;
    }
    return true;
}
struct ClumpMember { float x, y, scale; unsigned frame; bool woody; };
struct Clump {
    std::array<ClumpMember, 22> members{};
    int count = 0;
};
inline Clump makeClump(int x, int y, uint32_t seed, const ClumpRecipe& recipe,
                       float moisture, float slope, bool single) {
    Clump result;
    const auto roll = [&](uint32_t salt) { return clumpRoll(x,y,seed,salt); };
    const float wet = std::clamp(moisture + recipe.moistureBias, 0.0f, 1.0f);
    const float radius = recipe.radius * (0.7f + 0.6f*wet);
    if (single) {
        result.members[result.count++] = {0,0,2.0f+roll(10)*3.0f, static_cast<unsigned>(roll(11)*4),false};
        return result;
    }
    if (slope < 28.0f && roll(12) < recipe.treeChance*(0.5f+wet))
        result.members[result.count++] = {0,0,15.0f+roll(13)*9.0f,6u+static_cast<unsigned>(roll(14)*2),true};
    // Species patches, not an even mix: every shrub in a clump is one species and
    // most of its grass is one dominant card, so neighbouring clumps differ.
    const unsigned bushSpecies = 4u+static_cast<unsigned>(roll(16)*2);
    const unsigned grassSpecies = static_cast<unsigned>(roll(17)*4);
    const int bushes = 2 + static_cast<int>(roll(15)*4);
    for (int i = 0; i < bushes; ++i) {
        const auto salt = static_cast<uint32_t>(20+i*4);
        const float angle = (static_cast<float>(i)+roll(salt))*6.2831853f/static_cast<float>(bushes);
        const float r = radius*(0.12f+0.24f*roll(salt+1));
        result.members[result.count++] = {std::cos(angle)*r,std::sin(angle)*r,
            5.0f+roll(salt+2)*4.0f,bushSpecies,true};
    }
    const int grass = std::clamp(static_cast<int>(static_cast<float>(recipe.grass)*(0.6f+0.4f*wet)),6,16);
    for (int i = 0; i < grass; ++i) {
        const auto salt = static_cast<uint32_t>(60+i*4);
        const float angle = (static_cast<float>(i)+roll(salt))*6.2831853f/static_cast<float>(grass);
        const float r = radius*(0.30f+0.70f*std::sqrt(roll(salt+1)));
        const float pick = roll(salt+3);
        const unsigned frame = pick < 0.75f ? grassSpecies : static_cast<unsigned>((pick-0.75f)*16.0f);
        result.members[result.count++] = {std::cos(angle)*r,std::sin(angle)*r,
            2.0f+roll(salt+2)*3.0f,std::min(frame,3u),false};
    }
    return result;
}
// Meadow field (grass-field 2026-11-10): an even carpet of single grass blades
// under the clumps, so open grass reads as one continuous field instead of
// isolated tufts. Atlas frame bit 3 marks a field blade (low bits keep the
// grass layer 0-3); the shader leans it downwind and rolls gust bands over it.
inline constexpr unsigned kFieldBladeBit = 8u;
inline constexpr unsigned kFieldFlowerBit = 4u;   // with kFieldBladeBit: white wildflower heads (low 2 bits = grass layer)
// Owner ref 3 (2026-11-10): the gameplay-camera sward is continuous and even. tuft is the
// 14 WU tuft noise (0..1) at a blade: below kFieldGapBelow is a small soft dirt gap (0 = no
// blade); above it the blade height swells gently from 0.85x to 1.1x instead of tall tufts.
inline constexpr float kFieldGapBelow = 0.14f;
inline float fieldTuftHeight(float tuft) {
    if (tuft < kFieldGapBelow) return 0.0f;
    return 0.85f + 0.25f * std::clamp((tuft - kFieldGapBelow) / 0.5f, 0.0f, 1.0f);
}
struct FieldRecipe { float spacing = 6.0f, height = 4.5f; };
// Absent "field" = no carpet (mods and other biomes keep their clumps only).
inline std::optional<FieldRecipe> readFieldRecipe(const nlohmann::json& doc) {
    if (!doc.is_object() || !doc.contains("field") || !doc["field"].is_object()) return std::nullopt;
    const auto& f = doc["field"];
    for (const char* key : {"spacing", "height"})
        if (!f.contains(key) || !f[key].is_number()) return std::nullopt;
    const float spacing = f["spacing"].get<float>(), height = f["height"].get<float>();
    if (!(spacing >= 3 && spacing <= 32 && height >= 1 && height <= 12)) return std::nullopt;
    return FieldRecipe{spacing, height};
}
// Blades per placement cell on a k x k stratified grid. The authored spacing is
// coarsened until every candidate cell fits the budget, so the cap never cuts
// the scan short (an empty south half) and density stays even across the map.
inline int fieldBladeSide(float gridStep, float spacing, long budget, long fieldCells) {
    int k = std::clamp(static_cast<int>(std::lround(gridStep / spacing)), 0, 8);
    while (k > 0 && static_cast<long>(k) * k * fieldCells > budget) --k;
    return k;
}
// Owner 2026-11-10: shrubs and saplings belong under and around the stock trees
// (forest edge, understory); open meadow keeps only a rare one. forestCells counts
// forest cells in the clump's neighbourhood window.
inline float understoryChance(int forestCells) {
    return forestCells > 0 ? 1.0f : 0.06f;
}
// Owner 2026-11-10: "still like 10x too many" shrubs. Clustered thinning, not uniform culling:
// each woody member survives with this chance. cluster is a low-frequency world mask (0..1,
// ~220 WU); shrubs gather where it is high. Forest edges (1..30 of the 7x7 window) keep the
// most, deep forest little (the stock trees already cover it), open meadow only sparse singles;
// slopes and rocky ground (> 18 degrees) add some back. Averages about 1/10 of the old count.
inline float shrubKeepChance(int forestCells, float cluster, float slopeDeg) {
    const float clustered = std::clamp((cluster - 0.55f) / 0.20f, 0.0f, 1.0f);
    const bool edge = forestCells > 0 && forestCells <= 30;
    float base = edge ? 0.28f : forestCells > 30 ? 0.08f : 0.25f;
    if (slopeDeg > 18.0f) base += 0.15f;
    return std::min(base * (0.12f + 0.88f * clustered), 1.0f);
}
struct FieldBlade { float x, y, scale, yaw, seed; unsigned frame; };
// Blade j of a k x k cell grid, offsets from the cell origin in world units.
inline FieldBlade fieldBlade(int col, int row, uint32_t seed, int j, int k,
                             float gridStep, const FieldRecipe& recipe) {
    const auto roll = [&](uint32_t salt) { return clumpRoll(col,row,seed,salt + static_cast<uint32_t>(j)*8u); };
    const float sub = gridStep / static_cast<float>(k);
    const float x = (static_cast<float>(j % k) + roll(300)) * sub;
    const float y = (static_cast<float>(j / k) + roll(301)) * sub;
    const float scale = recipe.height * (0.7f + 0.6f * roll(302));
    return {x, -y, scale, roll(303) * 6.2831853f, roll(304),
            kFieldBladeBit | static_cast<unsigned>(roll(305) * 4.0f)};
}
// MOVE overlay value 3 is ordinary ground in stock maps. The road flag is
// authoritative; build-only flags overlap runtime pathlocks and are not masks.
// allowForest: understory shrubs and the meadow carpet may grow among stock trees.
template<class Cell> inline bool clumpStaticOccupied(Cell cell, bool allowForest = false) {
    return cell.getOffMap() || !cell.getPassable() || (cell.getForest() && !allowForest) ||
           cell.getWall() || cell.getGate() || cell.getRoad();
}
// The adapter samples these masks at every member, including offset skirts.
inline bool clumpSurfaceAllowed(bool inBounds, bool water, bool occupied,
                                 float hardCoverage, float slope, bool woody) {
    return inBounds && !water && !occupied && hardCoverage < 0.15f &&
           slope < (woody ? 35.0f : 50.0f);
}
} // namespace mc2::vegetation
