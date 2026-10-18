// gos_render_context.h — GAMEOS-RENDER-CONTEXT-PARITY-1
//
// Single source of truth for the render-context conventions that MUST match
// between the game (mc2.exe) and the mission editor (EditRel.exe). Both hosts
// share the same GLSL shaders, so any divergence in clip-space / depth
// convention silently corrupts depth (Slice 0 fixed exactly such a bug where
// the editor never set glClipControl and rendered against [0,1]-expecting
// shaders with the default [-1,1] convention).
//
// The invariants owned here:
//   * glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE) under the SAME fail-closed
//     guard (extension/version present, else abort) — see the .cpp for the
//     full rationale carried over from gameosmain.cpp.
//   * The reverse-Z baseline depth convention (far plane = depth 0, GL_GEQUAL).
//     This sets the baseline default depth func; the per-frame scene clear
//     (glClearDepth(0.0)) remains at each host's per-frame site because it is
//     a per-frame operation, not context init.
//
// There is no GL debug-output setup (the MC2_GL_DEBUG callback was removed with the GL backend).
#ifndef GOS_RENDER_CONTEXT_H
#define GOS_RENDER_CONTEXT_H

// Which host is establishing the render context. Lets the shared function keep
// the few legitimately host-specific divergences explicit instead of forking
// the whole convention block.
enum class RenderHostKind {
    Game,   // mc2.exe — gameosmain.cpp InitGameOS path
    Editor  // EditRel.exe — editor/EditorGameOS.cpp InitGameOS path
};

// Establish the shared render-context conventions. Call ONCE per host after
// glewInit() succeeds and a current GL context exists, and BEFORE any renderer
// creation / first scene draw.
//
// Fail-closed: if glClipControl is unavailable the conventions cannot be
// honored, so this aborts rather than render garbage depth.
void InitializeRenderContextConventions(RenderHostKind host);

#endif // GOS_RENDER_CONTEXT_H
