#pragma once
// FRAMEGRAPH-APPLY-STATE-EXTEND-1 — declaration of the shared GENERIC-AXES apply helper.
//
// applyTopLevelGenericAxes() applies the OPTIONAL non-pipeline render-state axes that a
// TopLevelStateDesc declares (FBO bind, viewport, depth-clear), each SKIPPED on its
// sentinel value (fboTarget==Unknown / viewport==Inherit / clear==None). The pipeline
// itself is applied by the per-pass apply fn AFTER calling this helper — kept in the
// per-pass fn for now to minimize churn.
//
// ★This header carries only the DECLARATION (no GL). The definition lives in the GL-touching
// gameos_graphics.cpp TU. The `fbo` param is a plain `unsigned` (GLuint is unsigned int) so
// this header pulls in NO GL header and stays cheaply includable from any TU (e.g. the
// static-prop batcher) — the GL-free top_level_pass_executor.h (offline-testable) is NOT
// touched, per the EXTEND design.
//
// Cross-TU shared (not inlined per-pass) so the upcoming Shadow slice reuses the SAME helper

#include "RenderCore/top_level_pass_executor.h"  // TopLevelStateDesc, ClearSpec

namespace RenderCore { namespace framegraph {

// Apply the declared optional axes IN ORDER, each skipped on its sentinel:
//   1. fboTarget != Unknown  -> glBindFramebuffer(GL_FRAMEBUFFER, fbo)
//   2. viewport  != Inherit  -> glViewport(0, 0, vpW, vpH)
//   3. clear == DepthForwardZ -> glClearDepth(1); glClear(GL_DEPTH_BUFFER_BIT); glClearDepth(0)
// (pipeline is applied by the caller AFTER this returns.)
void applyTopLevelGenericAxes(const TopLevelStateDesc& d, unsigned fbo, int vpW, int vpH);

}} // namespace RenderCore::framegraph
