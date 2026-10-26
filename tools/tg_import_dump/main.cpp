// game-free inspector over the REAL engine import path.
//
// Runs the actual mclib ImportGeometryFromFile on a GLB/FBX and dumps the
// resulting TG_TypeMultiShape as JSON: shape/texture counts, per-shape verts/
// tris/bbox/slot-0 texture, aggregate bbox, and red-flag heuristics. Catches the
// engine import->TG bugs the Assimp-only harness can't see (empty shapes,
// slot-0 = blip texture -> black, merge result, bbox/scale, orientation) at CLI
// speed — no GL, no game, no deploy.
//
//   tg_import_dump <model.glb|.fbx> [--no-ground]
//
// JSON -> stdout. Importer traces (MC2_MECH_SKEL_TRACE) -> stderr, so stdout
// stays pure JSON.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <set>

#include "heap.h"
#include "tgl.h"
#include "msl.h"
#include "assimp_importer.h"

extern void InstallTglHeap();  // stubs.cpp — sets the CRT-backed systemHeap

namespace {

std::string lower(const char* s) {
    std::string r = s ? s : "";
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <model.glb|.fbx> [--no-ground]\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];
    bool ground = true;
    bool staticProp = false;
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], "--no-ground") == 0) ground = false;
        else if (std::strcmp(argv[i], "--static-prop") == 0) staticProp = true;

    InstallTglHeap();

    TG_TypeMultiShape ms;
    long rc = ImportGeometryFromFile(path, &ms, ground, staticProp, staticProp);

    std::printf("{\n");
    std::printf("  \"source\": \"%s\",\n", path);
    std::printf("  \"autoGround\": %s,\n", ground ? "true" : "false");
    std::printf("  \"importResult\": %ld,\n", rc);
    if (rc != 0) {
        std::printf("  \"ok\": false,\n  \"error\": \"ImportGeometryFromFile returned nonzero\"\n}\n");
        return 1;
    }

    const long numShapes = ms.GetNumShapes();
    const long numTex = ms.GetNumTextures();

    // Texture slots.
    std::printf("  \"ok\": true,\n  \"shapeCount\": %ld,\n  \"textureCount\": %ld,\n", numShapes, numTex);
    std::printf("  \"textureSlots\": [");
    std::string slot0Name;
    for (long t = 0; t < numTex; ++t) {
        char buf[256] = {0};
        ms.GetTextureName((DWORD)t, buf, (long)sizeof(buf));
        if (t == 0) slot0Name = buf;
        std::printf("%s{\"index\": %ld, \"name\": \"%s\"}", t ? ", " : "", t, buf);
    }
    std::printf("],\n");

    // Per-shape + aggregate.
    float agLo[3] = {1e30f, 1e30f, 1e30f}, agHi[3] = {-1e30f, -1e30f, -1e30f};
    long totalV = 0, totalT = 0, emptyShapes = 0, shapeNodes = 0;
    std::printf("  \"shapes\": [\n");
    bool firstShape = true;
    for (long i = 0; i < numShapes; ++i) {
        TG_TypeNodePtr node = ms.GetTypeNode(i);
        if (!node || node->GetNodeType() != SHAPE_NODE) continue;
        ++shapeNodes;
        TG_TypeShape* s = static_cast<TG_TypeShape*>(node);
        const int nv = s->GetNumTypeVertices();
        const long nt = s->GetNumTypeTriangles();
        totalV += nv; totalT += nt;
        if (nv == 0) ++emptyShapes;

        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        const TG_TypeVertex* verts = s->GetTypeVertices();
        std::set<DWORD> windowCells;
        unsigned whiteSeeds = 0, blackSeeds = 0;
        for (int v = 0; v < nv && verts; ++v) {
            const DWORD light = verts[v].aRGBLight;
            if ((light >> 24) == 0x7d || (light >> 24) == 0x7e) windowCells.insert(light & 0xffffff);
            whiteSeeds += light == 0xffffffff;
            blackSeeds += light == 0xff000000;
            float c[3] = {verts[v].position.x, verts[v].position.y, verts[v].position.z};
            for (int k = 0; k < 3; ++k) {
                if (c[k] < lo[k]) lo[k] = c[k];
                if (c[k] > hi[k]) hi[k] = c[k];
                if (c[k] < agLo[k]) agLo[k] = c[k];
                if (c[k] > agHi[k]) agHi[k] = c[k];
            }
        }
        // Shape's primary texture = first triangle's slot.
        char texbuf[256] = "(none)";
        const TG_TypeTriangle* tris = s->GetTypeTriangles();
        if (nt > 0 && tris) ms.GetTextureName((DWORD)tris[0].localTextureHandle, texbuf, (long)sizeof(texbuf));

        std::printf("%s    {\"index\": %ld, \"node\": \"%s\", \"verts\": %d, \"tris\": %ld, \"slot0Tex\": \"%s\"",
                    firstShape ? "" : ",\n", i, s->getNodeId(), nv, nt, texbuf);
        const auto pivot = s->GetNodeCenter(), relative = s->GetRelativeNodeCenter();
        std::printf(", \"parent\": \"%s\", \"pivot\": [%.4f, %.4f, %.4f], \"relativePivot\": [%.4f, %.4f, %.4f]",
                    s->getParentId(), pivot.x, pivot.y, pivot.z, relative.x, relative.y, relative.z);
        std::printf(", \"windowCellCount\": %zu, \"whiteLightSeeds\": %u, \"blackLightSeeds\": %u",
                    windowCells.size(), whiteSeeds, blackSeeds);
        if (nv > 0)
            std::printf(", \"bbox\": {\"min\": [%.4f, %.4f, %.4f], \"max\": [%.4f, %.4f, %.4f]}",
                        lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        std::printf("}");
        firstShape = false;
    }
    std::printf("\n  ],\n");

    // Aggregate + flags.
    float dim[3] = {agHi[0]-agLo[0], agHi[1]-agLo[1], agHi[2]-agLo[2]};
    int tallest = 0;
    for (int k = 1; k < 3; ++k) if (dim[k] > dim[tallest]) tallest = k;
    const char* axisName[3] = {"X", "Y", "Z"};

    std::printf("  \"totalVerts\": %ld,\n  \"totalTris\": %ld,\n", totalV, totalT);
    std::printf("  \"shapeNodeCount\": %ld,\n  \"emptyShapeCount\": %ld,\n", shapeNodes, emptyShapes);
    if (totalV > 0) {
        std::printf("  \"aggregateBBox\": {\"min\": [%.4f, %.4f, %.4f], \"max\": [%.4f, %.4f, %.4f], \"dims\": [%.4f, %.4f, %.4f]},\n",
                    agLo[0], agLo[1], agLo[2], agHi[0], agHi[1], agHi[2], dim[0], dim[1], dim[2]);
        std::printf("  \"tallestAxis\": \"%s\",\n  \"derivedHeight\": %.4f,\n", axisName[tallest], dim[tallest]);
    }

    const std::string s0 = lower(slot0Name.c_str());
    const bool slot0Blip = s0.find("blip") != std::string::npos;
    const bool slot0Missing = slot0Name.empty() || s0 == "nulltxm";
    const bool orientSuspect = (totalV > 0 && tallest != 1);  // a standing mech is Y-tallest
    std::printf("  \"flags\": {\"slot0_blip\": %s, \"slot0_missing\": %s, \"empty_shapes_present\": %s, \"orientation_suspect\": %s}\n",
                slot0Blip ? "true" : "false", slot0Missing ? "true" : "false",
                emptyShapes > 0 ? "true" : "false", orientSuspect ? "true" : "false");
    std::printf("}\n");
    return 0;
}
