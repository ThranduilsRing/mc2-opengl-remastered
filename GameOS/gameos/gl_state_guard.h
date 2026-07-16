#ifndef GL_STATE_GUARD_H
#define GL_STATE_GUARD_H
//
// gl_state_guard.h — GlStateGuard slice 1.
//
// A minimal RAII primitive for scoped OpenGL state ownership. The engine's
// passes historically inherit GL state implicitly and hand-roll save/restore
// (glGetIntegeri_v -> bind -> ... -> rebind-previous). That pattern is correct
// but easy to forget, and forgetting it has shipped real bugs (e.g. the GPU
// cull SSBO slots leaking into later passes -> "water disappears on camera
// move"; see gpu_cull_compute.cpp C1B-SSBO-CLEANUP).
//
// Slice 1 introduces ONE guard — GlScopedSsboBinding — and applies it to ONE
// contained, modern, timer+oracle-covered pass (GPU cull patch dispatch),
// replacing the hand-rolled prevSsbo7 / prevSsbo15 save/restore with no
// behavior change. This is the pattern-proving slice, not a renderer rewrite.
// It deliberately covers only SHADER_STORAGE_BUFFER binding-base slots — the
// states the target pass actually mutates. Depth/blend/cull/VAO/program guards
// are out of scope until a pass that mutates them is wrapped.
//
#include <GL/glew.h>
#include <cstdio>
#include <cstdlib>

namespace mc2gl {

// Captures the buffer bound at SHADER_STORAGE_BUFFER binding-base `slot` on
// construction and restores it on destruction. Restore-previous semantics:
// whatever was bound before the guarded region is rebound after it. A rebind
// to the same value (slot never changed) is a harmless no-op.
//
// Leak visibility (opt-in, default-off): set MC2_GLSTATEGUARD_LOG=1 to print a
// one-line note whenever the guarded region actually changed the slot (i.e. the
// restore is load-bearing). Sampled once at process start; zero cost when off.
class GlScopedSsboBinding {
public:
    explicit GlScopedSsboBinding(GLuint slot) : slot_(slot) {
        GLint prev = 0;
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, slot_, &prev);
        prev_ = static_cast<GLuint>(prev);
    }

    ~GlScopedSsboBinding() {
        if (logEnabled()) {
            GLint cur = 0;
            glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, slot_, &cur);
            if (static_cast<GLuint>(cur) != prev_) {
                std::printf("[GLSTATEGUARD v1] ssbo slot=%u restored %u->%u\n",
                            slot_, static_cast<GLuint>(cur), prev_);
                std::fflush(stdout);
            }
        }
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot_, prev_);
    }

    GlScopedSsboBinding(const GlScopedSsboBinding&) = delete;
    GlScopedSsboBinding& operator=(const GlScopedSsboBinding&) = delete;
    GlScopedSsboBinding(GlScopedSsboBinding&&) = delete;
    GlScopedSsboBinding& operator=(GlScopedSsboBinding&&) = delete;

private:
    static bool logEnabled() {
        static const bool on = (std::getenv("MC2_GLSTATEGUARD_LOG") != nullptr);
        return on;
    }

    GLuint slot_;
    GLuint prev_ = 0;
};

// --- Slice 2: composable render-state guards -------------------------------
//
// Slice 1 proved the RAII pattern on SSBO binding (a compute pass). Slice 2
// introduces the first RENDER-state vocabulary so a draw pass can OWN the
// depth/blend/cull state it depends on instead of inheriting it (the class of
// bug behind the terrain transparency / depth-mask saga: a prior pass left
// glDepthMask FALSE -> opaque terrain wrote color but no depth -> see-through).
// Each guard captures the previous value in its ctor and restores it in its
// dtor (restore-previous, exactly matching the hand-rolled save/restore it
// replaces). Guards are non-copyable/movable; declare them at the point the
// state is set and let scope exit restore. NOTE: if the guarded function ends
// with gos_InvalidateRenderStateCache(), the guard MUST be block-scoped to
// close BEFORE that call (GameOS re-reads raw GL there) — terrain has no such
// call, so function scope is correct for it.

// Enable or disable a GL capability for the scope; restore its prior
// enabled/disabled state on destruction.
class GlScopedCapability {
public:
    GlScopedCapability(GLenum cap, bool enable) : cap_(cap) {
        prev_ = glIsEnabled(cap_);
        if (enable) glEnable(cap_); else glDisable(cap_);
    }
    // Slice 4 (VOCAB-1): capture-only overload — snapshot the current
    // enable state and restore it at scope exit WITHOUT setting anything in
    // the ctor. Used by the epilogue conversions where the pass body keeps
    // its own glEnable/glDisable calls exactly where they were (minimal-diff
    // rule; also the fix for the Site-4 billboard cull double-assert trap:
    // the body's re-assert stays, only the save/restore lines collapse).
    explicit GlScopedCapability(GLenum cap) : cap_(cap) {
        prev_ = glIsEnabled(cap_);
    }
    ~GlScopedCapability() {
        if (prev_) glEnable(cap_); else glDisable(cap_);
    }
    GlScopedCapability(const GlScopedCapability&) = delete;
    GlScopedCapability& operator=(const GlScopedCapability&) = delete;
    GlScopedCapability(GlScopedCapability&&) = delete;
    GlScopedCapability& operator=(GlScopedCapability&&) = delete;
private:
    GLenum    cap_;
    GLboolean prev_ = GL_FALSE;
};

// Set the depth write-mask and depth-func for the scope; restore both on
// destruction. (Depth-test enable is a capability -> use GlScopedCapability.)
class GlScopedDepthState {
public:
    GlScopedDepthState(GLboolean mask, GLenum func) {
        glGetBooleanv(GL_DEPTH_WRITEMASK, &prevMask_);
        GLint f = GL_LESS;
        glGetIntegerv(GL_DEPTH_FUNC, &f);
        prevFunc_ = static_cast<GLenum>(f);
        glDepthMask(mask);
        glDepthFunc(func);
    }
    // Slice 4 (VOCAB-1): capture-only — snapshot mask+func, restore at scope
    // exit; the pass body keeps its own glDepthMask/glDepthFunc calls.
    GlScopedDepthState() {
        glGetBooleanv(GL_DEPTH_WRITEMASK, &prevMask_);
        GLint f = GL_LESS;
        glGetIntegerv(GL_DEPTH_FUNC, &f);
        prevFunc_ = static_cast<GLenum>(f);
    }
    ~GlScopedDepthState() {
        glDepthMask(prevMask_);
        glDepthFunc(prevFunc_);
    }
    GlScopedDepthState(const GlScopedDepthState&) = delete;
    GlScopedDepthState& operator=(const GlScopedDepthState&) = delete;
    GlScopedDepthState(GlScopedDepthState&&) = delete;
    GlScopedDepthState& operator=(GlScopedDepthState&&) = delete;
private:
    GLboolean prevMask_ = GL_TRUE;
    GLenum    prevFunc_ = GL_LESS;
};

// Save/restore glClipControl (clip origin + depth-range mode). ARB_clip_control
// is required at engine startup (gameosmain.cpp); this guard is safe to
// instantiate whenever GL is live. Use at the boundary of any pass that could
// change clip semantics, or defensively in endXxxPass() to re-assert the scene
// expectation (GL_LOWER_LEFT + GL_ZERO_TO_ONE) if an NVIDIA driver reset occurs
// on FBO switch. Not usable across function call pairs — use manual member-var
// save/restore instead (see gosPostProcess::shadowSaved* pattern).
class GlScopedClipControl {
public:
    GlScopedClipControl() {
        GLint origin = GL_LOWER_LEFT;
        GLint depth  = GL_NEGATIVE_ONE_TO_ONE;
        glGetIntegerv(GL_CLIP_ORIGIN,     &origin);
        glGetIntegerv(GL_CLIP_DEPTH_MODE, &depth);
        prevOrigin_ = static_cast<GLenum>(origin);
        prevDepth_  = static_cast<GLenum>(depth);
    }
    ~GlScopedClipControl() {
        glClipControl(prevOrigin_, prevDepth_);
    }
    GlScopedClipControl(const GlScopedClipControl&) = delete;
    GlScopedClipControl& operator=(const GlScopedClipControl&) = delete;
    GlScopedClipControl(GlScopedClipControl&&) = delete;
    GlScopedClipControl& operator=(GlScopedClipControl&&) = delete;
private:
    GLenum prevOrigin_ = GL_LOWER_LEFT;
    GLenum prevDepth_  = GL_ZERO_TO_ONE;
};

// --- Slice 3: texture-unit save/restore ------------------------------------
//
// Slice 2 covered depth/blend/cull states. Slice 3 adds the texture-unit
// leak class: legacy decal/overlay paths call glActiveTexture(GL_TEXTURE0)
// and glBindTexture(GL_TEXTURE_2D, ...) inside their draw loops without
// restoring the previous active unit or the previous unit-0 binding.
// gos_InvalidateRenderStateCache() does NOT track texture unit state, so
// after one of these paths the engine cache believes unit 0 is bound to
// whatever applyRenderStates() last set, while GL actually holds the
// decal/overlay texture — exactly the vendor-visibility gap (AMD tolerates
// it; NVIDIA exposes wrong textures or intermittent corruption).
//
// Usage: declare at the point where the raw binds begin; the guard captures
// the current active-texture unit and the GL_TEXTURE_2D binding on `unit`
// (default 0), then restores both on destruction.  If the enclosing function
// ends with gos_InvalidateRenderStateCache(), use a block scope so the guard
// closes (restores) BEFORE that call — the cache-invalidation path re-reads
// raw GL state and must see the restored binding, not the mutated one.
class GlScopedTextureUnit {
public:
    // Slice 4 (VOCAB-1): optional `target` selects the texture target whose
    // binding is captured/restored on `unit` (GL_TEXTURE_2D default; the
    // mine-static bridge needs GL_TEXTURE_2D_ARRAY on unit 5).
    explicit GlScopedTextureUnit(GLuint unit = 0, GLenum target = GL_TEXTURE_2D)
        : unit_(unit), target_(target) {
        GLint active = static_cast<GLint>(GL_TEXTURE0);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        prevActiveTex_ = static_cast<GLenum>(active);
        glActiveTexture(GL_TEXTURE0 + unit_);
        GLint binding = 0;
        glGetIntegerv(bindingQueryFor(target_), &binding);
        prevBinding_ = static_cast<GLuint>(binding);
    }
    ~GlScopedTextureUnit() {
        glActiveTexture(GL_TEXTURE0 + unit_);
        glBindTexture(target_, prevBinding_);
        glActiveTexture(prevActiveTex_);
    }
    GlScopedTextureUnit(const GlScopedTextureUnit&) = delete;
    GlScopedTextureUnit& operator=(const GlScopedTextureUnit&) = delete;
    GlScopedTextureUnit(GlScopedTextureUnit&&) = delete;
    GlScopedTextureUnit& operator=(GlScopedTextureUnit&&) = delete;
private:
    static GLenum bindingQueryFor(GLenum target) {
        switch (target) {
            case GL_TEXTURE_2D_ARRAY:   return GL_TEXTURE_BINDING_2D_ARRAY;
            case GL_TEXTURE_3D:         return GL_TEXTURE_BINDING_3D;
            case GL_TEXTURE_CUBE_MAP:   return GL_TEXTURE_BINDING_CUBE_MAP;
            case GL_TEXTURE_2D:
            default:                    return GL_TEXTURE_BINDING_2D;
        }
    }
    GLuint unit_;
    GLenum target_        = GL_TEXTURE_2D;
    GLenum prevActiveTex_ = GL_TEXTURE0;
    GLuint prevBinding_   = 0;
};

// --- Slice 4: GLSTATE-GUARD-VOCAB-1 -----------------------------------------
//
// The epilogue recon (docs/testing/glstate-guard-epilogue-recon-1.md) found the
// GPU-direct pass epilogues are DOMINATED by state no slice-1..3 guard covers:
// program, VAO, array/element buffer, blend-func, sampler, cull mode, MRT
// draw-buffer list. This slice completes the vocabulary so those epilogues can
// convert. ALL slice-4 guards are CAPTURE-ONLY (snapshot in ctor, restore in
// dtor, never set) — the conversion rule is: guard declarations replace the
// hand-rolled SAVE lines in place, the trailing RESTORE lines are deleted, and
// every SET call in the pass body stays exactly where it was. Declaration
// order = reverse of the original hand-rolled restore order (C++ destroys in
// reverse), so the restore sequence is preserved instruction-for-instruction.

// GL_CURRENT_PROGRAM — restore via glUseProgram.
class GlScopedProgram {
public:
    GlScopedProgram() { glGetIntegerv(GL_CURRENT_PROGRAM, &prev_); }
    ~GlScopedProgram() { glUseProgram(static_cast<GLuint>(prev_)); }
    GlScopedProgram(const GlScopedProgram&) = delete;
    GlScopedProgram& operator=(const GlScopedProgram&) = delete;
    GlScopedProgram(GlScopedProgram&&) = delete;
    GlScopedProgram& operator=(GlScopedProgram&&) = delete;
private:
    GLint prev_ = 0;
};

// GL_VERTEX_ARRAY_BINDING — restore via glBindVertexArray. NOTE: restoring a
// VAO also swaps in that VAO's element-array binding; where a pass ALSO
// save/restores GL_ELEMENT_ARRAY_BUFFER_BINDING, declare the VAO guard FIRST
// (destroyed LAST is wrong — the element rebind must land INSIDE the restored
// VAO, i.e. the element-buffer guard must be declared BEFORE the VAO guard so
// it destructs AFTER it — matching every existing hand-rolled epilogue).
class GlScopedVertexArray {
public:
    GlScopedVertexArray() { glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_); }
    ~GlScopedVertexArray() { glBindVertexArray(static_cast<GLuint>(prev_)); }
    GlScopedVertexArray(const GlScopedVertexArray&) = delete;
    GlScopedVertexArray& operator=(const GlScopedVertexArray&) = delete;
    GlScopedVertexArray(GlScopedVertexArray&&) = delete;
    GlScopedVertexArray& operator=(GlScopedVertexArray&&) = delete;
private:
    GLint prev_ = 0;
};

// GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER binding — restore via glBindBuffer
// on the same target. (GL_ELEMENT_ARRAY_BUFFER binding is VAO-resident state:
// see the ordering note on GlScopedVertexArray.)
class GlScopedBufferBinding {
public:
    explicit GlScopedBufferBinding(GLenum target) : target_(target) {
        glGetIntegerv(bindingQueryFor(target), &prev_);
    }
    ~GlScopedBufferBinding() { glBindBuffer(target_, static_cast<GLuint>(prev_)); }
    GlScopedBufferBinding(const GlScopedBufferBinding&) = delete;
    GlScopedBufferBinding& operator=(const GlScopedBufferBinding&) = delete;
    GlScopedBufferBinding(GlScopedBufferBinding&&) = delete;
    GlScopedBufferBinding& operator=(GlScopedBufferBinding&&) = delete;
private:
    static GLenum bindingQueryFor(GLenum target) {
        switch (target) {
            case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
            case GL_SHADER_STORAGE_BUFFER: return GL_SHADER_STORAGE_BUFFER_BINDING;
            case GL_UNIFORM_BUFFER:       return GL_UNIFORM_BUFFER_BINDING;
            case GL_ARRAY_BUFFER:
            default:                      return GL_ARRAY_BUFFER_BINDING;
        }
    }
    GLenum target_;
    GLint  prev_ = 0;
};

// Blend function (separate RGB/alpha) — restore via glBlendFuncSeparate.
// STRICTLY-MORE-CORRECT NOTE: the hand-rolled epilogues saved only
// SRC_RGB/DST_RGB and restored with glBlendFunc(), which CLOBBERS a prior
// separate-alpha factor with the RGB one. This guard restores the true
// previous separate state. Both variants converge after the pass's
// gos_InvalidateRenderStateCache() (full blend re-apply on next legacy draw).
class GlScopedBlendFunc {
public:
    GlScopedBlendFunc() {
        glGetIntegerv(GL_BLEND_SRC_RGB,   &srcRgb_);
        glGetIntegerv(GL_BLEND_DST_RGB,   &dstRgb_);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA_);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA_);
    }
    ~GlScopedBlendFunc() {
        glBlendFuncSeparate(static_cast<GLenum>(srcRgb_), static_cast<GLenum>(dstRgb_),
                            static_cast<GLenum>(srcA_),   static_cast<GLenum>(dstA_));
    }
    GlScopedBlendFunc(const GlScopedBlendFunc&) = delete;
    GlScopedBlendFunc& operator=(const GlScopedBlendFunc&) = delete;
    GlScopedBlendFunc(GlScopedBlendFunc&&) = delete;
    GlScopedBlendFunc& operator=(GlScopedBlendFunc&&) = delete;
private:
    GLint srcRgb_ = GL_ONE, dstRgb_ = GL_ZERO, srcA_ = GL_ONE, dstA_ = GL_ZERO;
};

// glBindSampler(unit) binding — indexed query (production-proven pattern:
// gos_mech_batcher.cpp saves sampler slots 0-7 with glGetIntegeri_v).
class GlScopedSampler {
public:
    explicit GlScopedSampler(GLuint unit) : unit_(unit) {
        glGetIntegeri_v(GL_SAMPLER_BINDING, unit_, &prev_);
    }
    ~GlScopedSampler() { glBindSampler(unit_, static_cast<GLuint>(prev_)); }
    GlScopedSampler(const GlScopedSampler&) = delete;
    GlScopedSampler& operator=(const GlScopedSampler&) = delete;
    GlScopedSampler(GlScopedSampler&&) = delete;
    GlScopedSampler& operator=(GlScopedSampler&&) = delete;
private:
    GLuint unit_;
    GLint  prev_ = 0;
};

// glCullFace mode (GL_CULL_FACE_MODE latched state — independent of the
// GL_CULL_FACE enable, which is GlScopedCapability's job).
class GlScopedCullFaceMode {
public:
    GlScopedCullFaceMode() { glGetIntegerv(GL_CULL_FACE_MODE, &prev_); }
    ~GlScopedCullFaceMode() { glCullFace(static_cast<GLenum>(prev_)); }
    GlScopedCullFaceMode(const GlScopedCullFaceMode&) = delete;
    GlScopedCullFaceMode& operator=(const GlScopedCullFaceMode&) = delete;
    GlScopedCullFaceMode(GlScopedCullFaceMode&&) = delete;
    GlScopedCullFaceMode& operator=(GlScopedCullFaceMode&&) = delete;
private:
    GLint prev_ = GL_BACK;
};

// Non-indexed glColorMask — restore all four channels.
class GlScopedColorMask {
public:
    GlScopedColorMask() { glGetBooleanv(GL_COLOR_WRITEMASK, prev_); }
    ~GlScopedColorMask() { glColorMask(prev_[0], prev_[1], prev_[2], prev_[3]); }
    GlScopedColorMask(const GlScopedColorMask&) = delete;
    GlScopedColorMask& operator=(const GlScopedColorMask&) = delete;
    GlScopedColorMask(GlScopedColorMask&&) = delete;
    GlScopedColorMask& operator=(GlScopedColorMask&&) = delete;
private:
    GLboolean prev_[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
};

// MRT draw-buffer list — captures GL_DRAW_BUFFER0..7 of the CURRENTLY BOUND
// draw framebuffer and restores via glDrawBuffers. The guarded region must
// not switch draw FBOs (the restore would then target the wrong FBO) — true
// for the one consumer (particle deferred tube flush forces single
// COLOR_ATTACHMENT0 for its draws on the scene FBO).
class GlScopedDrawBuffers {
public:
    GlScopedDrawBuffers() {
        for (int i = 0; i < 8; ++i) {
            GLint d = GL_NONE;
            glGetIntegerv(GL_DRAW_BUFFER0 + i, &d);
            prev_[i] = static_cast<GLenum>(d);
            if (d != GL_NONE) count_ = i + 1;
        }
    }
    ~GlScopedDrawBuffers() {
        if (count_ > 0) glDrawBuffers(count_, prev_);
    }
    GlScopedDrawBuffers(const GlScopedDrawBuffers&) = delete;
    GlScopedDrawBuffers& operator=(const GlScopedDrawBuffers&) = delete;
    GlScopedDrawBuffers(GlScopedDrawBuffers&&) = delete;
    GlScopedDrawBuffers& operator=(GlScopedDrawBuffers&&) = delete;
    int savedCount() const { return count_; }
private:
    GLenum prev_[8] = { GL_NONE, GL_NONE, GL_NONE, GL_NONE,
                        GL_NONE, GL_NONE, GL_NONE, GL_NONE };
    int    count_ = 0;
};

// The RENDER_STATES v1 invalidation contract as a guard: dtor calls
// gos_InvalidateRenderStateCache() so the "call the hook AT THE END of the
// path" rule (gameos_graphics.cpp invalidateRenderStateCache() comment) holds
// BY CONSTRUCTION. Declare FIRST in a converted pass — it then destructs LAST,
// i.e. the invalidate fires after every state guard has restored, exactly
// matching the hand-rolled epilogue order (restore block, then invalidate).
// The hook only flips stateCacheValid_ (no GL reads), so guard-after-invalidate
// ordering bugs are impossible either way — this just keeps the sequence
// identical to the code it replaces. Collapses the known_issues.md "~14-entry
// manual invalidation list" bug class: a converted pass cannot forget the call.
class GlScopedRenderStateInvalidate {
public:
    GlScopedRenderStateInvalidate() = default;
    ~GlScopedRenderStateInvalidate() {
        extern void __stdcall gos_InvalidateRenderStateCache();
        gos_InvalidateRenderStateCache();
    }
    GlScopedRenderStateInvalidate(const GlScopedRenderStateInvalidate&) = delete;
    GlScopedRenderStateInvalidate& operator=(const GlScopedRenderStateInvalidate&) = delete;
    GlScopedRenderStateInvalidate(GlScopedRenderStateInvalidate&&) = delete;
    GlScopedRenderStateInvalidate& operator=(GlScopedRenderStateInvalidate&&) = delete;
};

}  // namespace mc2gl

#endif  // GL_STATE_GUARD_H
