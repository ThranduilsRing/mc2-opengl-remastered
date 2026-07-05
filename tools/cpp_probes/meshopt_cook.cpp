// meshopt_cook (MESHOPT-COOK-1) — OFFLINE LOD-simplification cook CLI.
//
// Truth-First arc P2 #4, honestly-rescoped: LOD SIMPLIFICATION ONLY.
//
// Loads a binary glTF (.glb) with cgltf, and for each mesh primitive runs
// meshopt_simplify at one or more target ratios (default LOD1=0.5, LOD2=0.25).
// ALL vertex attributes are preserved (POSITION/NORMAL/TEXCOORD and, for skinned
// mechs, JOINTS/WEIGHTS) so a simplified skinned mesh keeps valid bone bindings.
// Each requested LOD is written to a NEW output .glb (originals are never
// mutated). Per-primitive before/after index+vertex counts and the achieved
// meshopt error are reported to stdout.
//
// SCOPE / HONESTY:
//   - This delivers LOD simplify ONLY. Vertex-cache / vertex-fetch reorder GPU
//     wins are explicitly OUT: the MC2 runtime re-welds GLB verts via Assimp
//     aiProcess_JoinIdenticalVertices, so any reorder we baked would not reach
//     the GPU. We therefore do NOT reorder and do NOT claim a reorder win.
//   - OFFLINE tool: links ONLY the vendored meshoptimizer static lib + cgltf
//     header. Never links mclib / gameos / renderer. No runtime behavior change.
//
// GLB writer note: the vendored cgltf.h is PARSE-ONLY (no cgltf_write). We
// therefore assemble the output GLB by hand: a single interleaved BIN buffer
// plus a minimal glTF-2.0 JSON chunk. All attributes are re-emitted as FLOAT
// accessors, which is valid glTF for every attribute including JOINTS/WEIGHTS.
//
// Usage:
//   meshopt_cook --in <src.glb> [--out-dir <dir>] [--ratios 0.5,0.25]
//                [--target-error 0.01] [--lod-suffix _lod]
//   meshopt_cook --self-test-skinned   (synthetic skinned mesh, proves JOINTS/
//                                        WEIGHTS survive simplify; writes nothing)
//
// Output files: <out-dir>/<stem><suffix><N>.glb  (e.g. tree_lush_lod1.glb).

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <meshoptimizer.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---- A decoded attribute stream (always float components) -------------------
struct Attr {
    std::string          name;        // glTF semantic, e.g. "POSITION", "JOINTS_0"
    cgltf_attribute_type type;
    int                  comps;       // 1..4 (float components per vertex)
    std::vector<float>   data;        // comps * vertexCount, tightly packed
};

struct Prim {
    std::vector<Attr>          attrs;
    std::vector<unsigned int>  indices;
    size_t                     vertexCount = 0;
};

const char* attrTypeString(cgltf_type t) {
    switch (t) {
        case cgltf_type_scalar: return "SCALAR";
        case cgltf_type_vec2:   return "VEC2";
        case cgltf_type_vec3:   return "VEC3";
        case cgltf_type_vec4:   return "VEC4";
        default:                return "SCALAR";
    }
}
const char* compTypeForCount(int c) {
    switch (c) { case 1: return "SCALAR"; case 2: return "VEC2";
                 case 3: return "VEC3"; default: return "VEC4"; }
}

// Round a byte length up to a 4-byte boundary (glTF alignment requirement).
size_t pad4(size_t n) { return (n + 3u) & ~size_t(3u); }

// Little-endian append helpers for hand-assembling the GLB.
void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
}

} // namespace

// Decode one primitive: read every attribute + indices into float/uint streams.
static bool decodePrim(const cgltf_primitive& p, Prim& out) {
    if (p.type != cgltf_primitive_type_triangles) {
        std::fprintf(stderr, "  [skip] primitive is not a triangle list\n");
        return false;
    }
    size_t vcount = 0;
    for (size_t a = 0; a < p.attributes_count; ++a) {
        const cgltf_attribute& ga = p.attributes[a];
        const cgltf_accessor*  ac = ga.data;
        const int comps = int(cgltf_num_components(ac->type));
        Attr attr;
        attr.name  = ga.name ? ga.name : "";
        attr.type  = ga.type;
        attr.comps = comps;
        attr.data.resize(ac->count * comps);
        for (size_t i = 0; i < ac->count; ++i) {
            if (!cgltf_accessor_read_float(ac, i, &attr.data[i * comps], comps)) {
                std::fprintf(stderr, "  [err] cgltf_accessor_read_float failed on %s\n",
                             attr.name.c_str());
                return false;
            }
        }
        if (vcount == 0) vcount = ac->count;
        out.attrs.push_back(std::move(attr));
    }
    out.vertexCount = vcount;

    if (!p.indices) {
        // Non-indexed: synthesize a trivial index buffer.
        out.indices.resize(vcount);
        for (size_t i = 0; i < vcount; ++i) out.indices[i] = unsigned(i);
    } else {
        out.indices.resize(p.indices->count);
        for (size_t i = 0; i < p.indices->count; ++i)
            out.indices[i] = unsigned(cgltf_accessor_read_index(p.indices, i));
    }
    return true;
}

// Simplify `in` to targetRatio; produce a compacted `out` (indices + attrs).
// Returns achieved meshopt error via `resultError`.
static void simplifyPrim(const Prim& in, float targetRatio, float targetError,
                         Prim& out, float& resultError) {
    // Locate POSITION for the geometric error metric.
    const Attr* pos = nullptr;
    for (const Attr& a : in.attrs)
        if (a.type == cgltf_attribute_type_position) { pos = &a; break; }

    const size_t targetIndexCount =
        size_t(targetRatio * float(in.indices.size())) / 3 * 3;

    std::vector<unsigned int> simplified(in.indices.size());
    resultError = 0.0f;
    size_t outIndexCount = in.indices.size();
    if (pos) {
        outIndexCount = meshopt_simplify(
            simplified.data(), in.indices.data(), in.indices.size(),
            pos->data.data(), in.vertexCount, sizeof(float) * pos->comps,
            targetIndexCount, targetError, /*options=*/0, &resultError);
    } else {
        std::memcpy(simplified.data(), in.indices.data(),
                    in.indices.size() * sizeof(unsigned int));
    }
    simplified.resize(outIndexCount);

    // Compact: remap the referenced vertices to a dense 0..N range so the LOD
    // GLB carries only the vertices its (smaller) index buffer actually uses.
    std::vector<unsigned int> remap(in.vertexCount);
    const size_t newVertexCount = meshopt_optimizeVertexFetchRemap(
        remap.data(), simplified.data(), simplified.size(), in.vertexCount);

    out.indices.resize(simplified.size());
    meshopt_remapIndexBuffer(out.indices.data(), simplified.data(),
                             simplified.size(), remap.data());
    out.vertexCount = newVertexCount;

    for (const Attr& a : in.attrs) {
        Attr na;
        na.name = a.name; na.type = a.type; na.comps = a.comps;
        na.data.resize(newVertexCount * a.comps);
        meshopt_remapVertexBuffer(na.data.data(), a.data.data(), in.vertexCount,
                                  sizeof(float) * a.comps, remap.data());
        out.attrs.push_back(std::move(na));
    }
}

// Hand-assemble a valid GLB from a set of primitives (one mesh per prim).
static bool writeGlb(const std::string& path, const std::vector<Prim>& prims) {
    std::string  json;
    std::vector<uint8_t> bin;
    std::string  accessorsJson, bufferViewsJson, meshesJson;
    int accIdx = 0, bvIdx = 0;

    auto appendAccessor = [&](int bv, const char* ctype, size_t count,
                              int comps, const float* minv, const float* maxv) {
        char buf[512];
        std::string mm;
        if (minv && maxv) {
            std::string smin = "[", smax = "[";
            for (int c = 0; c < comps; ++c) {
                char t[64];
                std::snprintf(t, sizeof t, "%s%.9g", c ? "," : "", minv[c]); smin += t;
                std::snprintf(t, sizeof t, "%s%.9g", c ? "," : "", maxv[c]); smax += t;
            }
            smin += "]"; smax += "]";
            mm = ",\"min\":" + smin + ",\"max\":" + smax;
        }
        std::snprintf(buf, sizeof buf,
            "%s{\"bufferView\":%d,\"componentType\":5126,\"count\":%zu,\"type\":\"%s\"%s}",
            accIdx ? "," : "", bv, count, ctype, mm.c_str());
        accessorsJson += buf; return accIdx++;
    };
    auto appendIdxAccessor = [&](int bv, size_t count) {
        char buf[256];
        std::snprintf(buf, sizeof buf,
            "%s{\"bufferView\":%d,\"componentType\":5125,\"count\":%zu,\"type\":\"SCALAR\"}",
            accIdx ? "," : "", bv, count);
        accessorsJson += buf; return accIdx++;
    };
    auto appendBufferView = [&](size_t offset, size_t length, int target) {
        char buf[256];
        std::snprintf(buf, sizeof buf,
            "%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":%d}",
            bvIdx ? "," : "", offset, length, target);
        bufferViewsJson += buf; return bvIdx++;
    };

    for (size_t m = 0; m < prims.size(); ++m) {
        const Prim& pr = prims[m];
        std::string attrsObj = "{";
        bool first = true;
        for (const Attr& a : pr.attrs) {
            const size_t off = bin.size();
            const size_t len = a.data.size() * sizeof(float);
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(a.data.data());
            bin.insert(bin.end(), raw, raw + len);
            bin.resize(pad4(bin.size()), 0);
            const int bv = appendBufferView(off, len, 34962 /*ARRAY_BUFFER*/);

            // min/max only meaningful for POSITION (required by spec for POSITION).
            const float *mn = nullptr, *mx = nullptr;
            std::vector<float> vmin(a.comps, FLT_MAX), vmax(a.comps, -FLT_MAX);
            if (a.type == cgltf_attribute_type_position) {
                for (size_t v = 0; v < pr.vertexCount; ++v)
                    for (int c = 0; c < a.comps; ++c) {
                        float f = a.data[v * a.comps + c];
                        if (f < vmin[c]) vmin[c] = f;
                        if (f > vmax[c]) vmax[c] = f;
                    }
                mn = vmin.data(); mx = vmax.data();
            }
            const int acc = appendAccessor(bv, compTypeForCount(a.comps),
                                           pr.vertexCount, a.comps, mn, mx);
            char kv[128];
            std::snprintf(kv, sizeof kv, "%s\"%s\":%d",
                          first ? "" : ",", a.name.c_str(), acc);
            attrsObj += kv; first = false;
        }
        attrsObj += "}";

        const size_t ioff = bin.size();
        const size_t ilen = pr.indices.size() * sizeof(unsigned int);
        const uint8_t* iraw = reinterpret_cast<const uint8_t*>(pr.indices.data());
        bin.insert(bin.end(), iraw, iraw + ilen);
        bin.resize(pad4(bin.size()), 0);
        const int ibv  = appendBufferView(ioff, ilen, 34963 /*ELEMENT_ARRAY_BUFFER*/);
        const int iacc = appendIdxAccessor(ibv, pr.indices.size());

        char meshBuf[512];
        std::snprintf(meshBuf, sizeof meshBuf,
            "%s{\"primitives\":[{\"attributes\":%s,\"indices\":%d,\"mode\":4}]}",
            m ? "," : "", attrsObj.c_str(), iacc);
        meshesJson += meshBuf;
    }

    // Nodes: one per mesh; a single scene referencing them all.
    std::string nodesJson, sceneNodes;
    for (size_t m = 0; m < prims.size(); ++m) {
        char nb[64]; std::snprintf(nb, sizeof nb, "%s{\"mesh\":%zu}", m ? "," : "", m);
        nodesJson += nb;
        char sn[16]; std::snprintf(sn, sizeof sn, "%s%zu", m ? "," : "", m);
        sceneNodes += sn;
    }

    char header[256];
    std::snprintf(header, sizeof header,
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"meshopt_cook MESHOPT-COOK-1\"},"
        "\"buffers\":[{\"byteLength\":%zu}],", bin.size());
    json  = header;
    json += "\"bufferViews\":[" + bufferViewsJson + "],";
    json += "\"accessors\":["   + accessorsJson   + "],";
    json += "\"meshes\":["      + meshesJson      + "],";
    json += "\"nodes\":["       + nodesJson       + "],";
    json += "\"scenes\":[{\"nodes\":[" + sceneNodes + "]}],\"scene\":0}";

    // Pad JSON chunk with spaces to 4 bytes; BIN chunk already padded above.
    std::vector<uint8_t> jsonChunk(json.begin(), json.end());
    jsonChunk.resize(pad4(jsonChunk.size()), 0x20);
    bin.resize(pad4(bin.size()), 0);

    std::vector<uint8_t> glb;
    putU32(glb, 0x46546C67);                                  // "glTF"
    putU32(glb, 2);                                           // version
    const uint32_t total = 12 + 8 + uint32_t(jsonChunk.size())
                              + 8 + uint32_t(bin.size());
    putU32(glb, total);
    putU32(glb, uint32_t(jsonChunk.size())); putU32(glb, 0x4E4F534A); // JSON
    glb.insert(glb.end(), jsonChunk.begin(), jsonChunk.end());
    putU32(glb, uint32_t(bin.size())); putU32(glb, 0x004E4942);       // BIN\0
    glb.insert(glb.end(), bin.begin(), bin.end());

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "  [err] cannot open output %s\n", path.c_str()); return false; }
    std::fwrite(glb.data(), 1, glb.size(), f);
    std::fclose(f);
    std::printf("  wrote %s  (%zu bytes)\n", path.c_str(), glb.size());
    return true;
}

// ---- Synthetic skinned self-test: proves JOINTS/WEIGHTS survive simplify ----
static int selfTestSkinned() {
    // Build a 16x16 skinned grid (positions + joints + weights), simplify to 0.5.
    const int N = 16;
    Prim in;
    Attr P; P.name = "POSITION"; P.type = cgltf_attribute_type_position; P.comps = 3;
    Attr J; J.name = "JOINTS_0"; J.type = cgltf_attribute_type_joints;   J.comps = 4;
    Attr W; W.name = "WEIGHTS_0";W.type = cgltf_attribute_type_weights;  W.comps = 4;
    for (int j = 0; j < N; ++j) for (int i = 0; i < N; ++i) {
        float x = float(i) / (N - 1), z = float(j) / (N - 1);
        float y = 0.2f * std::sin(x * 6.28f) * std::cos(z * 6.28f);
        P.data.insert(P.data.end(), { x, y, z });
        float jn = float((i < N/2) ? 1 : 2);        // two bones, split down the middle
        J.data.insert(J.data.end(), { jn, 0, 0, 0 });
        W.data.insert(W.data.end(), { 1.0f, 0, 0, 0 });
    }
    in.vertexCount = size_t(N) * N;
    in.attrs = { P, J, W };
    for (int j = 0; j < N - 1; ++j) for (int i = 0; i < N - 1; ++i) {
        unsigned v = unsigned(j * N + i);
        in.indices.insert(in.indices.end(), { v, v + 1, v + unsigned(N) + 1,
                                              v, v + unsigned(N) + 1, v + unsigned(N) });
    }

    Prim out; float err = 0;
    simplifyPrim(in, 0.5f, 0.01f, out, err);

    // Validate: every surviving vertex still carries a joint in {1,2} and a
    // finite, ~1.0 total weight — i.e. bindings were not mangled.
    const Attr *oj = nullptr, *ow = nullptr;
    for (const Attr& a : out.attrs) {
        if (a.type == cgltf_attribute_type_joints)  oj = &a;
        if (a.type == cgltf_attribute_type_weights) ow = &a;
    }
    bool ok = oj && ow && oj->comps == 4 && ow->comps == 4;
    int badJoint = 0, badWeight = 0;
    for (size_t v = 0; ok && v < out.vertexCount; ++v) {
        float jn = oj->data[v * 4 + 0];
        if (jn != 1.0f && jn != 2.0f) ++badJoint;
        float wsum = ow->data[v*4+0]+ow->data[v*4+1]+ow->data[v*4+2]+ow->data[v*4+3];
        if (!(std::fabs(wsum - 1.0f) < 1e-4f)) ++badWeight;
    }
    std::printf("[self-test-skinned] in: %zu verts / %zu idx  ->  out: %zu verts / %zu idx  err=%.6f\n",
                in.vertexCount, in.indices.size(), out.vertexCount, out.indices.size(), err);
    std::printf("[self-test-skinned] JOINTS/WEIGHTS present=%s  bad-joint=%d  bad-weight-sum=%d\n",
                (oj && ow) ? "yes" : "NO", badJoint, badWeight);
    if (ok && badJoint == 0 && badWeight == 0) {
        std::printf("[self-test-skinned] PASS — bone bindings preserved through simplify.\n");
        return 0;
    }
    std::printf("[self-test-skinned] FAIL\n");
    return 1;
}

int main(int argc, char** argv) {
    const char* inPath   = nullptr;
    const char* outDir   = nullptr;
    const char* suffix   = "_lod";
    float targetError    = 0.01f;
    std::vector<float> ratios;

    for (int a = 1; a < argc; ++a) {
        if (!std::strcmp(argv[a], "--self-test-skinned")) return selfTestSkinned();
        else if (!std::strcmp(argv[a], "--in") && a + 1 < argc) inPath = argv[++a];
        else if (!std::strcmp(argv[a], "--out-dir") && a + 1 < argc) outDir = argv[++a];
        else if (!std::strcmp(argv[a], "--lod-suffix") && a + 1 < argc) suffix = argv[++a];
        else if (!std::strcmp(argv[a], "--target-error") && a + 1 < argc)
            targetError = float(std::atof(argv[++a]));
        else if (!std::strcmp(argv[a], "--ratios") && a + 1 < argc) {
            char* s = argv[++a];
            for (char* tok = std::strtok(s, ","); tok; tok = std::strtok(nullptr, ","))
                ratios.push_back(float(std::atof(tok)));
        }
    }
    if (!inPath) {
        std::fprintf(stderr,
            "usage: meshopt_cook --in <src.glb> [--out-dir <dir>] "
            "[--ratios 0.5,0.25] [--target-error 0.01] [--lod-suffix _lod]\n"
            "       meshopt_cook --self-test-skinned\n");
        return 2;
    }
    if (ratios.empty()) { ratios = { 0.5f, 0.25f }; }

    cgltf_options opts{};
    cgltf_data*   data = nullptr;
    if (cgltf_parse_file(&opts, inPath, &data) != cgltf_result_success) {
        std::fprintf(stderr, "meshopt_cook: cgltf parse failed: %s\n", inPath); return 1;
    }
    if (cgltf_load_buffers(&opts, data, inPath) != cgltf_result_success) {
        std::fprintf(stderr, "meshopt_cook: cgltf_load_buffers failed: %s\n", inPath);
        cgltf_free(data); return 1;
    }

    // Decode every primitive of every mesh once.
    std::vector<Prim> base;
    for (size_t m = 0; m < data->meshes_count; ++m)
        for (size_t p = 0; p < data->meshes[m].primitives_count; ++p) {
            Prim pr;
            if (decodePrim(data->meshes[m].primitives[p], pr)) base.push_back(std::move(pr));
        }
    cgltf_free(data);

    if (base.empty()) { std::fprintf(stderr, "meshopt_cook: no triangle primitives\n"); return 1; }

    // Derive stem + out dir.
    std::string in(inPath);
    size_t slash = in.find_last_of("/\\");
    std::string dir  = (outDir ? std::string(outDir)
                               : (slash == std::string::npos ? "." : in.substr(0, slash)));
    std::string file = (slash == std::string::npos) ? in : in.substr(slash + 1);
    size_t dot = file.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? file : file.substr(0, dot);

    std::printf("meshopt_cook: %s  (%zu primitive(s))  target-error=%.4g\n",
                inPath, base.size(), targetError);

    int rc = 0;
    for (size_t r = 0; r < ratios.size(); ++r) {
        const float ratio = ratios[r];
        std::printf("--- LOD%zu  ratio=%.3f ---\n", r + 1, ratio);
        std::vector<Prim> lod;
        for (size_t pi = 0; pi < base.size(); ++pi) {
            Prim out; float err = 0;
            simplifyPrim(base[pi], ratio, targetError, out, err);
            std::printf("  prim %zu: verts %zu -> %zu   idx %zu -> %zu   error=%.6f\n",
                        pi, base[pi].vertexCount, out.vertexCount,
                        base[pi].indices.size(), out.indices.size(), err);
            lod.push_back(std::move(out));
        }
        char outName[1024];
        std::snprintf(outName, sizeof outName, "%s/%s%s%zu.glb",
                      dir.c_str(), stem.c_str(), suffix, r + 1);
        if (!writeGlb(outName, lod)) rc = 1;
    }
    return rc;
}
