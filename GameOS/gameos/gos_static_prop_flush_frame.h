// Locals the GpuStaticPropBatcher::flush phases hand to each other.
#ifndef GAMEOS_GAMEOS_GOS_STATIC_PROP_FLUSH_FRAME_H
#define GAMEOS_GAMEOS_GOS_STATIC_PROP_FLUSH_FRAME_H

#include "gos_static_prop_batcher_internal.h"
#include "mdi_submit.h"                          // MDI-SUBMISSION-SCAFFOLD-1: flush issues the indirect draws

// Locals the static-prop flush phases share.
struct StaticPropFlushFrame {
    GLuint parityBuffer;
    std::optional<mc2gl::GlScopedProgram> guardProgram;
    std::optional<mc2gl::GlScopedVertexArray> guardVao;
    std::optional<mc2gl::GlScopedBufferBinding> guardArrayBuf;
    std::optional<mc2gl::GlScopedBufferBinding> guardElemBuf;
    std::optional<mc2gl::GlScopedTextureUnit> guardTexUnit0;
    std::optional<mc2gl::GlScopedSsboBinding> guardSsbo0;
    std::optional<mc2gl::GlScopedSsboBinding> guardSsbo1;
    std::optional<mc2gl::GlScopedSsboBinding> guardSsbo2;
    std::optional<mc2gl::GlScopedSsboBinding> guardSsbo3;
    std::optional<mc2gl::GlScopedSsboBinding> guardSsbo5;
    GLint locParityWrite;
    GLint locParityVerts;
    GLint locParityBaseVertex;
    bool useC1bIndirect;
    size_t fr_off_bytes_d;
    size_t off_total_bytes;
    size_t on_total_bytes;
    std::optional<mc2gl::GlScopedSsboBinding> guardSsbo4;
    std::optional<mc2gl::GlScopedTextureUnit> guardTex2DArray;
    bool runV6;
    uint32_t v6LockstepViolations;
    bool runV5;
};

#endif  // GAMEOS_GAMEOS_GOS_STATIC_PROP_FLUSH_FRAME_H
