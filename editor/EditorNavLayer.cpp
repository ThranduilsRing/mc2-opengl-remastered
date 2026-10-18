#include "EditorNavLayer.h"
#include "terrain.h"

// Defined in terrain.cpp — shared globals read by quad.cpp draw path.
extern uint8_t* gEditorNavFlags;
extern int      gEditorNavCellSide;
extern bool     drawEditorPassability;
extern TerrainPtr land;

static EditorNavLayer sInstance;

EditorNavLayer& EditorNavLayer::Get()
{
    return sInstance;
}

void EditorNavLayer::Clear()
{
    flags.clear();
    cellSide           = 0;
    gEditorNavFlags    = nullptr;
    gEditorNavCellSide = 0;
    drawEditorPassability = false;
}

bool EditorNavLayer::BuildFromTerrain()
{
    if (!land) return false;

    const int vertexSide = land->realVerticesMapSide;
    if (vertexSide < 2) return false;

    cellSide = vertexSide - 1;
    flags.assign(static_cast<size_t>(cellSide) * cellSide, 0);

    for (int r = 0; r < cellSide; ++r)
    {
        for (int c = 0; c < cellSide; ++c)
        {
            // Air always passable.
            uint8_t f = EDITOR_NAV_AIR_PASSABLE;

            // Query water type via center MOVE sub-cell of this terrain tile.
            // cellToWorld() takes MOVE-cell coords (1 terrain cell = 3×3 MOVE cells).
            glm::vec3 worldPos;
            land->cellToWorld(r * MAPCELL_DIM + 1, c * MAPCELL_DIM + 1, worldPos);

            long waterType = land->getWater(worldPos);
            if (waterType == 2)       // WATER_TYPE_DEEP
            {
                f |= EDITOR_NAV_DEEP_WATER | EDITOR_NAV_HOVER_PASSABLE | EDITOR_NAV_BLOCKED;
            }
            else if (waterType == 1)  // WATER_TYPE_SHALLOW
            {
                f |= EDITOR_NAV_SHALLOW_WATER | EDITOR_NAV_GROUND_PASSABLE | EDITOR_NAV_HOVER_PASSABLE;
            }
            else
            {
                f |= EDITOR_NAV_GROUND_PASSABLE | EDITOR_NAV_HOVER_PASSABLE;
            }

            flags[static_cast<size_t>(r) * cellSide + c] = f;
        }
    }

    gEditorNavFlags    = flags.data();
    gEditorNavCellSide = cellSide;
    return true;
}

uint8_t EditorNavLayer::GetCellFlags(int row, int col) const
{
    if (row < 0 || col < 0 || row >= cellSide || col >= cellSide)
        return 0;
    return flags[static_cast<size_t>(row) * cellSide + col];
}
