// tools/asset_viewer/ShapePanel.h
// Mechs / Vehicles / Trees: list + filter in the browser pane, variant table with triangle counts above the
// shared 3D preview in the inspector pane. One instance holds the three categories' state.
#pragma once
#include "ShapeCatalog.h"
#include <string>
#include <vector>

class MeshPreview3D;

class ShapePanel {
public:
    void drawBrowser(ShapeCategory cat);
    void drawDetails(ShapeCategory cat, MeshPreview3D& preview);
    void select(ShapeCategory cat, const std::string& name);   // scripted view / tests
    void markStale() { for (State& s : s_) s.dirty = s.selected >= 0; }   // another panel used the shared preview
private:
    struct State {
        bool built = false;
        std::vector<ShapeEntry> entries;
        std::vector<int> shown;                 // indices into entries
        char filter[128] = {0};
        int selected = -1;                      // index into entries
        int variant = 0;
        std::vector<ShapeStats> stats;          // per variant of the selected entry
        bool dirty = false;                     // preview needs (re)loading
    };
    State& st(ShapeCategory c) { return s_[(int)c]; }
    void ensureBuilt(State& s, ShapeCategory cat);
    void refilter(State& s);
    void pick(State& s, int entryIdx);
    State s_[3];
};
