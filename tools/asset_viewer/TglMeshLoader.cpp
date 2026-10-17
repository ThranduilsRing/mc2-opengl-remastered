/***************************************************************
 * FILENAME: TglMeshLoader.cpp
 * DESCRIPTION: CPU mesh data extraction from MC2 .tgl archives.
 *   Compiled into mc2_tglloader (gets engine defines + includes).
 *   No GL calls anywhere in this file.
 *
 * Task 1 of the asset-viewer model-preview slice.
 ***************************************************************/

// Engine headers — must come before imgui/SDL to avoid WIN32 guard collisions.
#include "heap.h"
#include "tgl.h"
#include "msl.h"
#include "fastfile.h"
#include "ffile.h"

#include "TglMeshLoader.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <cctype>

// ---------------------------------------------------------------------------
// Symbols exposed by engine_stubs.cpp (same link unit for the lib).
// ---------------------------------------------------------------------------
extern void          InstallTglHeap();

// FastFile globals (fastfile.cpp).
extern FastFile** fastFiles;
extern long       numFastFiles;
extern long       maxFastFiles;
extern long       ffLastError;

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
namespace {

bool   g_fastFileReady = false;
bool   g_initAttempted = false;
std::string g_initError;

// Lowercase a single character.
char tolowerC(char c) { return (char)std::tolower((unsigned char)c); }

// Case-insensitive check whether 'name' ends with ".tgl".
bool endsWith_tgl(const char* name)
{
    if (!name) return false;
    size_t len = std::strlen(name);
    if (len < 4) return false;
    return tolowerC(name[len-4]) == '.' &&
           tolowerC(name[len-3]) == 't' &&
           tolowerC(name[len-2]) == 'g' &&
           tolowerC(name[len-1]) == 'l';
}

} // namespace

// ---------------------------------------------------------------------------
// TglMeshLoader::ensureFastFile
// ---------------------------------------------------------------------------
bool TglMeshLoader::ensureFastFile(const char* deployDir)
{
    if (g_fastFileReady)   return true;
    if (g_initAttempted)   return false;  // already failed; don't retry
    g_initAttempted = true;

    // Install the stand-in systemHeap.
    InstallTglHeap();

    // Allocate the fastFiles pointer array if not already done.
    if (!fastFiles)
    {
        maxFastFiles = 4;
        fastFiles = static_cast<FastFile**>(
            std::malloc(maxFastFiles * sizeof(FastFile*)));
        if (!fastFiles)
        {
            g_initError = "malloc fastFiles failed";
            return false;
        }
        std::memset(fastFiles, 0, maxFastFiles * sizeof(FastFile*));
        numFastFiles = 0;
    }

    std::string fstPath = std::string(deployDir) + "/tgl.fst";
    if (!FastFileInit(fstPath.c_str()))
    {
        g_initError = std::string("FastFileInit('") + fstPath + "') failed"
                    + " (ffLastError=" + std::to_string(ffLastError) + ")";
        return false;
    }

    if (numFastFiles < 1 || !fastFiles[0])
    {
        g_initError = "FastFileInit succeeded but no fastFile registered";
        return false;
    }

    g_fastFileReady = true;
    return true;
}

// ---------------------------------------------------------------------------
// TglMeshLoader::listTgl
// ---------------------------------------------------------------------------
std::vector<std::string> TglMeshLoader::listTgl()
{
    std::vector<std::string> result;
    if (!g_fastFileReady) return result;

    long total = fastFiles[0]->getNumFiles();
    const FILE_HANDLE* handles = fastFiles[0]->getFilesInfo();
    if (!handles) return result;

    for (long i = 0; i < total; ++i)
    {
        if (!handles[i].pfe) continue;
        const char* name = handles[i].pfe->name;
        if (endsWith_tgl(name))
            result.push_back(name);
    }

    std::sort(result.begin(), result.end());
    return result;
}

// ---------------------------------------------------------------------------
// TglMeshLoader::loadMesh
// ---------------------------------------------------------------------------
MeshData TglMeshLoader::loadMesh(const std::string& tglName)
{
    MeshData out;

    TG_TypeMultiShape ms;
    long rc = ms.LoadBinaryCopy(tglName.c_str());
    if (rc != 0)
    {
        out.error = "LoadBinaryCopy('" + tglName + "') returned " + std::to_string(rc);
        return out;
    }

    long numShapes = ms.GetNumShapes();
    if (numShapes <= 0)
    {
        out.error = "GetNumShapes() == 0 for '" + tglName + "'";
        return out;
    }

    // Per-texture-handle SubMesh accumulator across all shapes.
    // Key = uint64_t combining localTextureHandle (low 32 bits) and a spotlight
    // bit (bit 32) so that spotlight-shape triangles are always isolated into
    // their own flagged submesh(es) and never merged with normal geometry that
    // happens to share the same texture handle.
    std::map<uint64_t, SubMesh> subMap;

    // Accumulated bounds (init from first real vertex).
    bool boundsInited = false;
    float bmin[3] = {0.f,0.f,0.f}, bmax[3] = {0.f,0.f,0.f};

    auto expandBounds = [&](float x, float y, float z) {
        if (!boundsInited) {
            bmin[0] = bmax[0] = x;
            bmin[1] = bmax[1] = y;
            bmin[2] = bmax[2] = z;
            boundsInited = true;
        } else {
            if (x < bmin[0]) bmin[0] = x;
            if (y < bmin[1]) bmin[1] = y;
            if (z < bmin[2]) bmin[2] = z;
            if (x > bmax[0]) bmax[0] = x;
            if (y > bmax[1]) bmax[1] = y;
            if (z > bmax[2]) bmax[2] = z;
        }
    };

    for (long si = 0; si < numShapes; ++si)
    {
        TG_TypeNodePtr node = ms.GetTypeNode(si);
        if (!node) continue;

        // TG_TypeShape extends TG_TypeNode (polymorphic); dynamic_cast is safe.
        TG_TypeShape* shape = dynamic_cast<TG_TypeShape*>(node);
        if (!shape) continue;

        int  numV = shape->GetNumTypeVertices();
        long numT = shape->GetNumTypeTriangles();
        if (numV <= 0 || numT <= 0) continue;

        const TG_TypeVertex*   V = shape->GetTypeVertices();
        const TG_TypeTriangle* T = shape->GetTypeTriangles();
        if (!V || !T) continue;

        // Determine if this shape is a spotlight beam by checking its node name.
        // Node names of the form "SpotLight_*" (case-insensitive prefix, 10 chars)
        // identify the forward headlight/beam geometry that can obscure the model.
        const char* nodeId = node->getNodeId();
        bool spot = false;
        if (nodeId) {
            // Manual case-insensitive prefix compare for "SpotLight_" (10 chars).
            static const char kSpotPrefix[] = "SpotLight_";
            static const int  kSpotLen      = 10;
            bool match = true;
            for (int ci = 0; ci < kSpotLen && match; ++ci) {
                char nc = nodeId[ci];
                if (nc == '\0') { match = false; break; }
                // tolower
                if (nc >= 'A' && nc <= 'Z') nc = (char)(nc + ('a' - 'A'));
                char pc = kSpotPrefix[ci];
                if (pc >= 'A' && pc <= 'Z') pc = (char)(pc + ('a' - 'A'));
                if (nc != pc) match = false;
            }
            spot = match;
        }

        // Walk triangles; group by combined key so spotlight tris are isolated.
        for (long ti = 0; ti < numT; ++ti)
        {
            const TG_TypeTriangle& tri = T[ti];
            DWORD handle = tri.localTextureHandle;

            // Combined key: low 32 bits = texture handle, bit 32 = spotlight flag.
            uint64_t k = (uint64_t)handle | (spot ? (1ull << 32) : 0ull);

            // Lazy-create the submesh and populate its textureName once.
            // isSpotlight is set unconditionally (all tris from the same shape share
            // the same spot value, and the map key already encodes it).
            SubMesh& sub = subMap[k];
            sub.isSpotlight = spot;
            if (sub.textureName.empty() && sub.verts.empty())
            {
                char texBuf[256] = {0};
                ms.GetTextureName(handle, texBuf, (long)sizeof(texBuf));
                sub.textureName = texBuf;  // may be empty if name is blank
            }

            // Per-corner UV expansion: push 3 MeshVertex, indices sequential.
            const float* uvPairs[3][2] = {
                { &tri.uvdata.u0, &tri.uvdata.v0 },
                { &tri.uvdata.u1, &tri.uvdata.v1 },
                { &tri.uvdata.u2, &tri.uvdata.v2 },
            };

            for (int c = 0; c < 3; ++c)
            {
                DWORD vi = tri.Vertices[c];
                if ((int)vi >= numV) {
                    // Out-of-range index — skip whole triangle (degenerate data).
                    // Roll back the 2 already-pushed verts if c > 0.
                    sub.verts.resize(sub.verts.size() - c);
                    sub.idx.resize(sub.idx.size() - c);
                    goto nextTri;
                }

                const TG_TypeVertex& tv = V[vi];

                // Stuff -> GL coordinate transform.
                // MC2/Stuff is right-handed Z-up (X=right, Y=forward, Z=up).
                // GL convention used here is right-handed Y-up (X=right, Y=up, Z=toward-viewer).
                // Mapping: glX = -stuffX (mirror), glY = stuffZ (up), glZ = stuffY (depth).
                // The mirrored X preserves MC2 front-facing winding order for GL.
                float px = -tv.position.x;
                float py =  tv.position.z;
                float pz =  tv.position.y;

                float nx = -tv.normal.x;
                float ny =  tv.normal.z;
                float nz =  tv.normal.y;

                MeshVertex mv;
                mv.px = px; mv.py = py; mv.pz = pz;
                mv.nx = nx; mv.ny = ny; mv.nz = nz;
                mv.u  = *uvPairs[c][0];
                mv.v  = *uvPairs[c][1];

                uint32_t newIdx = static_cast<uint32_t>(sub.verts.size());
                sub.verts.push_back(mv);
                sub.idx.push_back(newIdx);

                expandBounds(px, py, pz);
            }
            nextTri:;
        }
    }

    // Collect non-empty submeshes in handle order.
    for (auto& kv : subMap)
    {
        SubMesh& sub = kv.second;
        if (!sub.verts.empty())
            out.submeshes.push_back(std::move(sub));
    }

    if (out.submeshes.empty())
    {
        out.error = "No geometry extracted from '" + tglName + "'";
        return out;
    }

    out.bmin[0] = bmin[0]; out.bmin[1] = bmin[1]; out.bmin[2] = bmin[2];
    out.bmax[0] = bmax[0]; out.bmax[1] = bmax[1]; out.bmax[2] = bmax[2];
    out.ok = true;
    return out;
}
