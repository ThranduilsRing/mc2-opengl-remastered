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
#include <cmath>
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

// Record-sheet geometry, shared by drawing and input in legacy canvas coordinates.
inline constexpr float kDx = 72;   // record sheet shifted right by kDx; the freed width goes to the mech preview (narrower component list)
inline constexpr float kAreaX = 217 + kDx, kAreaY = 150, kColW = 102, kGap = 5;
inline constexpr int kCellW = 52;
inline constexpr Rect kPreview{25,130,184 + kDx,356};   // below the LOCATIONS | CLASSIC strip, ends above the armor/heat counters (y 488)
inline constexpr float kHeaderH = 58, kRowItemH = 11, kRefusalH = 0;
inline constexpr int kRowsPerColumn = 8, kMaxItemRows = 8;
inline Rect panelRect(int p) {
    static constexpr Rect boxes[]{
        {324+kDx,150,102,96}, {324+kDx,250,102,150},
        {217+kDx,258,102,118}, {431+kDx,258,102,118},
        {217+kDx,150,102,104}, {431+kDx,150,102,104},
        {217+kDx,380,102,96}, {431+kDx,380,102,96}, {217+kDx,476,316,14}};
    return p >= 0 && p < kPanelCount ? boxes[p] : Rect{};
}
inline constexpr Rect summaryRect() { return {324+kDx,404,102,72}; }
inline bool torso(int p) { return p >= 1 && p <= 3; }
inline int visibleRows(int p) { return p == 1 ? 8 : (torso(p) ? 5 : (p == 4 || p == 5 ? 4 : 3)); }
inline float equipmentY(int p) { return torso(p) ? 68.f : 56.f; }
inline Rect armorRowRect(int p, bool rear) {
    const auto r = panelRect(p);
    return {r.x+3, r.y+12+(rear ? 13.f : 0.f), r.w-6,13};
}
inline Rect stepperRect(int p, bool rear, bool plus) {
    const auto r = armorRowRect(p,rear);
    return {r.x+(plus ? 59.f : 26.f),r.y,11,11};
}
// Armor value: centred in the panel (and between the steppers), vertically on the stepper row.
inline Rect armorValueRect(int p, bool rear) {
    const auto r = armorRowRect(p,rear);
    return {r.x+37.f,r.y-.5f,22,12};
}
inline Rect itemRowRect(int p, int k, int) {
    const auto r = panelRect(p);
    return {r.x+2,r.y+equipmentY(p)+k*(torso(p) ? 9.f : kRowItemH),r.w-4,torso(p) ? 9.f : kRowItemH};
}// LOCATIONS | CLASSIC switch, above the panels.
inline Rect toggleRect() { return {217, 113, 134, 15}; }   // stock x: stays clear of REMOVE

// Panel under a legacy-canvas point. Doll is not a drop target but is hit-testable.
inline std::optional<int> panelAt(float lx, float ly) {
    for (int p = 0; p < kPanelCount; ++p)
        if (panelRect(p).contains(lx, ly)) return p;
    return std::nullopt;
}
// Item row index under a point inside panel p.
inline std::optional<int> itemRowAt(int p, float lx, float ly, int count) {
    for (int k = 0; k < std::min(count, visibleRows(p)); ++k)
        if (itemRowRect(p, k, count).contains(lx, ly)) return k;
    return std::nullopt;
}

// ---- overflow scrolling: a 12-cell location can hold more than kMaxItemRows items -------------
// `scroll` is the index of the first visible item. Wheel over a panel, or the small arrows that
// sit on the (empty) left of the mount-badge row, move it; items are addressed by (scroll + row),
// so drawing and hit-testing cannot disagree.
inline int maxScroll(int count, int p = 1) { return std::max(0, count - visibleRows(p)); }
inline int clampScroll(int count, int scroll, int p = 1) {
    const int m = maxScroll(count,p);
    return scroll < 0 ? 0 : (scroll > m ? m : scroll);
}
inline int scrollByWheel(int count, int scroll, long wheelDelta, int p = 1) {   // engine wheel: positive = back = later items
    return clampScroll(count, scroll + (wheelDelta > 0 ? 1 : (wheelDelta < 0 ? -1 : 0)),p);
}
inline int scrollToReveal(int count, int scroll, int index, int p = 1) {
    scroll = std::clamp(scroll, index - visibleRows(p) + 1, index);
    return clampScroll(count, scroll,p);
}
inline Rect scrollArrowRect(int p, bool down) {
    const Rect r = panelRect(p);
    return {r.x + r.w - (down ? 12.f : 24.f), r.y + equipmentY(p)-10,10,9};
}
// Item index (not row) under a point inside panel p, honouring the scroll; nullopt when no item is there.
inline std::optional<int> itemIndexAt(int p, int count, int scroll, float lx, float ly) {
    const auto row = itemRowAt(p, lx, ly, std::min(count, visibleRows(p)));
    if (!row) return std::nullopt;
    const int i = clampScroll(count, scroll,p) + *row;
    return i < count ? std::optional<int>(i) : std::nullopt;
}
// The highlighted item is the placed INSTANCE (its cell), not every copy of the same component.
inline bool isSelectedInstance(long itemX, long itemY, long selX, long selY) { return itemX == selX && itemY == selY; }

// Panel index of a BT location name.
inline std::optional<int> panelOfLocation(std::string_view name) {
    for (int p = 0; p < kLocationCount; ++p)
        if (name == panelName(p)) return p;
    return std::nullopt;
}

// ---- refusal reasons --------------------------------------------------------------------------

// OVERWEIGHT applies to package chassis only (their payload rule is enforced). Stock MC2 does not
// enforce weight ("weight no longer matters", canAddComponent) and its 20%+7 base is an estimate, so
// shipped stock loadouts (Bushwacker Prime: 38 t equipment + 18 t base vs 55 t) can read over.
inline bool overweight(bool packageChassis, float massUsed, float payload) { return packageChassis && massUsed > payload + 1e-3f; }
// Layout/armor warnings only after the player edited the loadout; an untouched stock mech reads READY.
inline bool showLayoutArmorWarnings(bool layoutEdited, bool armorExplicit) { return layoutEdited || armorExplicit; }

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

// ---- saved-layout migration ---------------------------------------------------------------------
// A loadout saved against an older grid/mount layout of the same chassis id must not restore its old
// coordinates blindly, and one displaced item must not push out items that could have stayed. The whole
// loadout is solved at once (weapons first, so an unrestricted item can never hold the only cell under a
// mount): a legal loadout comes back unchanged; otherwise items keep their saved cell when still legal, the
// rest move to the first legal cell (regions in declared order, rows, then columns), and only what truly
// has no legal cell or mass budget left is dropped. Deterministic: depends only on the chassis and the list.
struct Cell { int x = -1, y = -1; };
enum class SavedLayoutAction { Restore, Migrate, PackDefault };
inline SavedLayoutAction savedLayoutAction(long savedRev, int installedRev) {
    return savedRev > installedRev ? SavedLayoutAction::PackDefault
        : savedRev < installedRev ? SavedLayoutAction::Migrate : SavedLayoutAction::Restore;
}

// readIdLong writes 0 and returns an error when the key is absent: an absent LayoutRev is the grandfathered rev 1.
inline long savedLayoutRev(long readResult, long value) { return readResult == 0 ? value : 1; }

struct LoadoutItem { mc2pkg::Placed p; bool slot = false; };   // slot = legacy -2/-3 item (jump jet, ECM, sensor): mass only
struct LoadoutResult {
    std::vector<Cell> cells;     // parallel to the input; saved cell, new cell, or {-1,-1} when dropped
    std::vector<char> kept;      // parallel to the input
    int moved = 0, dropped = 0, droppedWeapons = 0;
};

inline bool loadoutLegal(const mc2pkg::PackageChassis& c, const std::vector<LoadoutItem>& items) {
    std::vector<mc2pkg::Placed> grid, all;
    for (const LoadoutItem& it : items) {
        all.push_back(it.p);
        if (it.slot) continue;
        if (!mc2pkg::regionForFootprint(c, it.p.x, it.p.y, it.p.w, it.p.h)) return false;
        if (!cellsFree(grid, it.p.x, it.p.y, it.p.w, it.p.h)) return false;
        grid.push_back(it.p);
    }
    bool ok = false;
    mc2pkg::assignMounts(c, grid, &ok);
    return ok && mc2pkg::massUsed(all) <= c.payload + 1e-3f;
}

namespace detail {
inline LoadoutResult solveLoadout(const mc2pkg::PackageChassis& c, const std::vector<LoadoutItem>& items, bool sticky) {
    const size_t n = items.size();
    LoadoutResult r;
    r.cells.assign(n, Cell{});
    r.kept.assign(n, 0);
    std::vector<mc2pkg::Placed> existing;
    auto order = [&](auto pred) {   // larger footprint first, then saved index
        std::vector<size_t> v;
        for (size_t i = 0; i < n; ++i) if (pred(items[i])) v.push_back(i);
        std::stable_sort(v.begin(), v.end(), [&](size_t a, size_t b) { return items[a].p.w * items[a].p.h > items[b].p.w * items[b].p.h; });
        return v;
    };
    auto place = [&](size_t i, int x, int y) {
        mc2pkg::Placed p = items[i].p;
        p.x = x; p.y = y;
        existing.push_back(p);
        r.cells[i] = {x, y};
        r.kept[i] = 1;
    };
    auto keep = [&](size_t i) {
        const mc2pkg::Placed& p = items[i].p;
        if (p.x < 0 || p.y < 0 || !cellsFree(existing, p.x, p.y, p.w, p.h) || mc2pkg::checkAdd(c, existing, p) != mc2pkg::Verdict::Legal) return;
        place(i, p.x, p.y);
    };
    auto move = [&](size_t i) {
        const mc2pkg::Placed& p = items[i].p;
        Candidate cd;
        cd.w = p.w; cd.h = p.h; cd.category = p.category; cd.ordinal = p.ordinal; cd.weight = p.weight;
        for (size_t ri = 0; ri < c.regions.size(); ++ri) {
            const Explained e = explain(c, existing, cd, static_cast<int>(ri));
            if (e.reason == Reason::Legal) { place(i, e.x, e.y); return; }
        }
    };
    auto runClass = [&](const std::vector<size_t>& cls) {
        if (sticky) for (size_t i : cls) keep(i);
        for (size_t i : cls) if (!r.kept[i]) move(i);
    };
    runClass(order([](const LoadoutItem& it) { return !it.slot && it.p.category != mc2pkg::Mount::None; }));   // weapons
    for (size_t i : order([](const LoadoutItem& it) { return it.slot; })) {                                       // slot items: mass only
        mc2pkg::Placed probe = items[i].p;
        probe.x = probe.y = -1; probe.category = mc2pkg::Mount::None;
        if (mc2pkg::checkAdd(c, existing, probe) == mc2pkg::Verdict::Legal) place(i, -1, -1);
    }
    runClass(order([](const LoadoutItem& it) { return !it.slot && it.p.category == mc2pkg::Mount::None; }));   // unrestricted equipment
    for (size_t i = 0; i < n; ++i) {
        if (!r.kept[i]) {
            ++r.dropped;
            if (!items[i].slot && items[i].p.category != mc2pkg::Mount::None) ++r.droppedWeapons;
        } else if (!items[i].slot && (r.cells[i].x != items[i].p.x || r.cells[i].y != items[i].p.y)) {
            ++r.moved;
        }
    }
    return r;
}
} // namespace detail

inline LoadoutResult migrateLoadout(const mc2pkg::PackageChassis& c, const std::vector<LoadoutItem>& items) {
    if (loadoutLegal(c, items)) {
        LoadoutResult r;
        for (const LoadoutItem& it : items) { r.cells.push_back({it.p.x, it.p.y}); r.kept.push_back(1); }
        return r;
    }
    LoadoutResult a = detail::solveLoadout(c, items, true);
    if (a.dropped == 0) return a;
    LoadoutResult b = detail::solveLoadout(c, items, false);
    return (b.droppedWeapons < a.droppedWeapons || (b.droppedWeapons == a.droppedWeapons && b.dropped < a.dropped)) ? b : a;
}

// C-bills returned for the components a migration dropped: the full cost, once per owned mech of that variant.
inline long layoutRefund(long droppedCost, long mechs) { return droppedCost * mechs; }

// ---- view state (mech-lab location view) --------------------------------------------------------
// Per-panel scroll offsets and the doll icon key. Reset on entering/leaving the lab and on a variant swap so a
// stale scroll or a reused variant address can never leak into the next mech.
struct LocViewState {
    int scroll[kPanelCount] = {};
    std::string dollKey;
    void reset() { for (int& s : scroll) s = 0; dollKey.clear(); }
    bool needsIcon(std::string_view key) const { return dollKey != key; }
};

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
