// tools/asset_viewer/SphereMesh.h
#pragma once
#include <vector>
#include <cstdint>
#include "tool_vk_gpu.h"

// Interleaved vertex: position(3), normal(3), tangent(4 = xyz + handedness w), uv(2) = 12 floats.
struct SphereVertex {
    float px, py, pz;
    float nx, ny, nz;
    float tx, ty, tz, tw;   // tangent.xyz + handedness (+1 / -1)
    float u, v;
};

class SphereMesh {
public:
    // Generates a UV sphere of `radius` with `stacks` latitude bands and
    // `slices` longitude segments. CPU buffers only; call uploadGL() once toolvk is up
    // to create the vertex + index buffers.
    void generate(float radius, int stacks, int slices);
    void uploadGL();          // creates the vertex + index buffers from the CPU buffers
    void destroyGL();         // free them
    const toolvk::Buffer& vertexBuffer() const { return vb_; }
    const toolvk::Buffer& indexBuffer() const { return ib_; }
    uint32_t indexCount() const { return (uint32_t)indexCount_; }

    const std::vector<SphereVertex>& vertices() const { return verts_; }
    const std::vector<uint32_t>&     indices()  const { return idx_;   }

private:
    std::vector<SphereVertex> verts_;
    std::vector<uint32_t>     idx_;
    toolvk::Buffer vb_, ib_;
    int indexCount_ = 0;
};
