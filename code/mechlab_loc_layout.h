#ifndef MECHLAB_LOC_LAYOUT_H
#define MECHLAB_LOC_LAYOUT_H
// MECHLAB-LOC-1: the location view of the mech lab (package / BT2018 chassis).
//
// PURE (std + package_chassis.h only). One rect table serves BOTH drawing and hit-testing: every
// rect is defined once in legacy 800x600 canvas coordinates (the space userInput's mouse reports),
// and drawing maps them through the same Canvas transform the lab already uses for its ImGui layer.
// There is no art-rect arithmetic, so a draw rect and its hit rect cannot drift apart.
//
// Panels are built from PackageChassis regions (cells = slots) and hardpoints (mount badges); every
// legality verdict comes from mc2pkg::checkAdd, and explain() names why a panel refuses a part.

#include "package_chassis.h"

#include <algorithm>
#include <optional>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace mc2::mechlab_loc {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
};

// Canvas transform: screen = legacy * scale + origin (aObject::getCanvasTransform).
struct Canvas {
    float sx = 1, sy = 1, ox = 0, oy = 0;
};

inline Rect toScreen(const Rect& r, const Canvas& c) { return {r.x * c.sx + c.ox, r.y * c.sy + c.oy, r.w * c.sx, r.h * c.sy}; }


inline constexpr int kPanelCount = 9;   // 8 BT locations (Head..RightLeg, in panelName order) + the doll
inline constexpr int kDoll = 8;
inline constexpr int kLocationCount = 8;

inline const char* panelName(int p) {   // matches the BT location names in package regions
    static constexpr const char* n[] = {"Head", "CenterTorso", "LeftTorso", "RightTorso", "LeftArm", "RightArm", "LeftLeg", "RightLeg", "Doll"};
    return (p >= 0 && p < kPanelCount) ? n[p] : "";
}
inline const char* panelTitle(int p) {
    static constexpr const char* n[] = {"HEAD", "CENTER TORSO", "LEFT TORSO", "RIGHT TORSO", "LEFT ARM", "RIGHT ARM", "LEFT LEG", "RIGHT LEG", ""};
    return (p >= 0 && p < kPanelCount) ? n[p] : "";
}

// Dedicated loadout column; the mech owns the unobstructed column to its left.
inline constexpr float kAreaX = 217, kAreaY = 150, kColW = 156, kRowH = 78, kGap = 4;
inline constexpr int kCellW = 52;
inline constexpr Rect kPreview{25, 116, 184, 377};
// Title + mount badges, four normal-font item rows, then a refusal line.
inline constexpr float kHeaderH = 28, kRowItemH = 12, kRefusalH = 10;
inline constexpr int kRowsPerColumn = 4;

inline Rect panelRect(int p) {
    // Paired locations, then a compact mass/special-slot strip beneath them.
    static constexpr int col[] = {0, 1, 0, 1, 0, 1, 0, 1};
    static constexpr int row[] = {0, 0, 1, 1, 2, 2, 3, 3};
    if (p < 0 || p >= kPanelCount) return {};
    if (p == kDoll) return {kAreaX, 476, 316, 14};
    return {kAreaX + col[p] * (kColW + kGap), kAreaY + row[p] * (kRowH + kGap), kColW, kRowH};
}
inline Rect itemRowRect(int p, int k, int count) {
    const Rect r = panelRect(p);
    const int columns = std::max(1, (count + kRowsPerColumn - 1) / kRowsPerColumn);
    const float width = (r.w - 4) / columns;
    const float height = (r.h - kHeaderH - kRefusalH) / kRowsPerColumn;
    return {r.x + 2 + (k / kRowsPerColumn) * width,
            r.y + kHeaderH + (k % kRowsPerColumn) * height, width, height};
}
// LOCATIONS | CLASSIC switch, above the panels.
inline Rect toggleRect() { return {kAreaX, 113, 134, 15}; }

// Panel under a legacy-canvas point. Doll is not a drop target but is hit-testable.
inline std::optional<int> panelAt(float lx, float ly) {
    for (int p = 0; p < kPanelCount; ++p)
        if (panelRect(p).contains(lx, ly)) return p;
    return std::nullopt;
}
// Item row index under a point inside panel p.
inline std::optional<int> itemRowAt(int p, float lx, float ly, int count) {
    for (int k = 0; k < count; ++k)
        if (itemRowRect(p, k, count).contains(lx, ly)) return k;
    return std::nullopt;
}

// Panel index of a BT location name.
inline std::optional<int> panelOfLocation(std::string_view name) {
    for (int p = 0; p < kLocationCount; ++p)
        if (name == panelName(p)) return p;
    return std::nullopt;
}

// ---- refusal reasons --------------------------------------------------------------------------

enum class Reason : int { Legal = 0, NoRegion, TooBig, RegionFull, NoFreeMount, OverPayload };

struct Candidate {
    int w = 1, h = 1;
    mc2pkg::Mount category = mc2pkg::Mount::None;
    int ordinal = -1;
    float weight = 0.0f;
};

struct Explained {
    Reason reason = Reason::NoRegion;
    int x = -1, y = -1;        // first legal cell when reason == Legal
    float overage = 0.0f;      // tons over payload when reason == OverPayload
};

inline bool cellsFree(const std::vector<mc2pkg::Placed>& existing, int x, int y, int w, int h) {
    for (const mc2pkg::Placed& p : existing) {
        if (p.x < 0) continue;
        if (x < p.x + p.w && p.x < x + w && y < p.y + p.h && p.y < y + h) return false;
    }
    return true;
}

// Where (and whether) `cand` can go in region `regionIndex` of `c`; the first legal cell scanning
// rows then columns, or the first rule that refuses it. Cell occupancy + mc2pkg::checkAdd only:
// heat and jump-jet rules stay with the legacy caller (LogisticsVariant::canAddComponent).
inline Explained explain(const mc2pkg::PackageChassis& c, const std::vector<mc2pkg::Placed>& existing,
                         const Candidate& cand, int regionIndex) {
    Explained e;
    if (regionIndex < 0 || regionIndex >= static_cast<int>(c.regions.size())) return e;
    const mc2pkg::Region& r = c.regions[regionIndex];
    if (cand.w > r.w || cand.h > r.h) { e.reason = Reason::TooBig; return e; }
    float mass = cand.weight;
    for (const mc2pkg::Placed& p : existing) mass += p.weight;
    if (mass > c.payload + 1e-3f) { e.reason = Reason::OverPayload; e.overage = mass - c.payload; return e; }
    bool anyFree = false;
    for (int j = r.y; j + cand.h <= r.y + r.h; ++j)
        for (int i = r.x; i + cand.w <= r.x + r.w; ++i) {
            if (!cellsFree(existing, i, j, cand.w, cand.h)) continue;
            anyFree = true;
            mc2pkg::Placed p;
            p.x = i; p.y = j; p.w = cand.w; p.h = cand.h;
            p.category = cand.category; p.ordinal = cand.ordinal; p.weight = cand.weight;
            if (mc2pkg::checkAdd(c, existing, p) == mc2pkg::Verdict::Legal) {
                e.reason = Reason::Legal; e.x = i; e.y = j;
                return e;
            }
        }
    e.reason = anyFree ? Reason::NoFreeMount : Reason::RegionFull;
    return e;
}

// One short line for a panel (101 legacy px wide at ~10 px text: keep under ~22 characters).
inline std::string reasonText(const Explained& e, mc2pkg::Mount m, const mc2pkg::Region* r, const Candidate& cand) {
    switch (e.reason) {
        case Reason::Legal: return "fits";
        case Reason::NoRegion: return "no slots here";
        case Reason::TooBig:
            return "too big: needs " + std::to_string(cand.w) + "x" + std::to_string(cand.h) +
                   (r ? ", has " + std::to_string(r->w) + "x" + std::to_string(r->h) : std::string());
        case Reason::RegionFull: return "no free slots";
        case Reason::NoFreeMount: return std::string("no free ") + mc2pkg::mountName(m) + " mount";
        case Reason::OverPayload: {
            char buf[48];
            std::snprintf(buf, sizeof buf, "over payload by %.1f t", static_cast<double>(e.overage));
            return buf;
        }
    }
    return "";
}

} // namespace mc2::mechlab_loc

#endif // MECHLAB_LOC_LAYOUT_H
