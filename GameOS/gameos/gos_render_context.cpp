// gos_render_context.cpp — GAMEOS-RENDER-CONTEXT-PARITY-1
//
// Shared implementation of the render-context conventions that BOTH the game
// and the editor must establish identically. Compiled into ${SOURCES} so it
// lands in both the `gameos` lib (linked by mc2.exe) and the `gameos_editor`
// lib (linked by EditRel.exe). See gos_render_context.h for the contract.

#include "gos_render_context.h"

#include "gameos.hpp"   // gosASSERT

#include <GL/glew.h>    // glClipControl, GLEW_ARB_clip_control, GLEW_VERSION_4_5
#include <stdio.h>
#include <stdlib.h>     // abort
#include "mc2fmt.h"

void InitializeRenderContextConventions(RenderHostKind host)
{
    const char* hostName = (host == RenderHostKind::Editor) ? "editor" : "game";

    // --- Reverse-Z depth baseline ------------------------------------------
    // The shared scene render path is reverse-Z (U2): far plane = depth 0,
    // comparison GL_GEQUAL. We set the depth func here as the documented
    // baseline default so neither host can drift to GL_LEQUAL (the editor's
    // pre-Slice-0 bug). The per-frame scene clear to glClearDepth(0.0) stays
    // at each host's per-frame site — that is a per-frame op, not context init.
    glDepthFunc(GL_GEQUAL);

    // --- Clip control (clip-space + NDC depth convention) ------------------
    // GL_ARB_clip_control aligns the hardware depth convention with the
    // engine's existing D3D-style projection matrices. cameraToClip
    // (mclib/camera.cpp) produces clip-space [0, w] by deliberate design;
    // without clip control, hardware default expects [-w, w] and compresses
    // our output into window depth [0.5, 1.0] — half precision wasted. With
    // ZERO_TO_ONE, hardware natively expects [0, w], NDC z is [0, 1], window
    // depth uses the full [0, 1] range.
    //
    // **Fail-closed contract:** the shader depth path removes its depth-range
    // workaround remaps unconditionally and assumes [0,1] NDC z. Running
    // without glClipControl(GL_ZERO_TO_ONE) would feed [0,1] NDC z to hardware
    // expecting [-1,1] and produce garbage depth. So if the extension is
    // somehow unavailable at runtime we MUST refuse to start rather than ship
    // broken rendering — for BOTH hosts.
    //
    // Plan: docs/superpowers/plans/2026-05-06-clip-control-adoption.md
    if (GLEW_ARB_clip_control || GLEW_VERSION_4_5) {
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        mc2::fmt::print_out("[RENDER-CTX] host={} clip_control=enabled origin=lower_left depth=zero_to_one depth_func=gequal\n", mc2::fmt::safe(hostName));
        fflush(stdout);
    } else {
        mc2::fmt::print_out("[RENDER-CTX] host={} clip_control=unsupported fatal=1\n", mc2::fmt::safe(hostName));
        fflush(stdout);
        gosASSERT(false);
        abort();
    }
}
