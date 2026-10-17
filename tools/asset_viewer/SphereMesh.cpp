// tools/asset_viewer/SphereMesh.cpp
#include "SphereMesh.h"
#include <cmath>

void SphereMesh::generate(float radius, int stacks, int slices) {
    verts_.clear();
    idx_.clear();
    const float PI = 3.14159265358979323846f;
    for (int i = 0; i <= stacks; ++i) {
        float phi = PI * (float)i / (float)stacks;          // 0..PI (lat)
        float sp = std::sin(phi), cp = std::cos(phi);
        for (int j = 0; j <= slices; ++j) {
            float theta = 2.0f * PI * (float)j / (float)slices;  // 0..2PI (lon)
            float st = std::sin(theta), ct = std::cos(theta);

            SphereVertex sv{};
            // unit normal/position
            float nx = sp * ct, ny = cp, nz = sp * st;
            sv.px = radius * nx; sv.py = radius * ny; sv.pz = radius * nz;
            sv.nx = nx; sv.ny = ny; sv.nz = nz;
            // uv: u from longitude, v from latitude
            sv.u = (float)j / (float)slices;
            sv.v = (float)i / (float)stacks;
            // tangent = d(position)/d(theta), normalized. dP/dtheta = (-sp*st, 0, sp*ct).
            float tx = -sp * st, ty = 0.0f, tz = sp * ct;
            float tl = std::sqrt(tx*tx + ty*ty + tz*tz);
            if (tl < 1e-6f) { tx = 1.0f; ty = 0.0f; tz = 0.0f; tl = 1.0f; }  // poles
            sv.tx = tx/tl; sv.ty = ty/tl; sv.tz = tz/tl;
            // handedness: sign of dot(cross(N,T), B) where B = dP/dphi.
            // For this parameterization right-handed TBN gives +1.
            sv.tw = 1.0f;
            verts_.push_back(sv);
        }
    }
    int cols = slices + 1;
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            uint32_t a = i * cols + j;
            uint32_t b = (i + 1) * cols + j;
            idx_.push_back(a);   idx_.push_back(b);   idx_.push_back(a + 1);
            idx_.push_back(a + 1); idx_.push_back(b); idx_.push_back(b + 1);
        }
    }
}

void SphereMesh::uploadGL() {
    indexCount_ = (int)idx_.size();
    toolvk::createBuffer(vb_, verts_.data(), verts_.size() * sizeof(SphereVertex), false);
    toolvk::createBuffer(ib_, idx_.data(), idx_.size() * sizeof(uint32_t), true);
}

void SphereMesh::destroyGL() {
    toolvk::destroyBuffer(ib_);
    toolvk::destroyBuffer(vb_);
}
