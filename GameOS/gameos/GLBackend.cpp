// GameOS/gameos/GLBackend.cpp
//
// GL implementation of RenderCore::IRenderBackend. The ONLY place IRenderBackend
// ops become real gl* calls (RenderCore stays GL-free — same split as
// pipeline_binder). First boundary slice (RENDER-BACKEND-IFACE-POSTPROCESS-1):
// bindBackbuffer() forwards the EXACT GL the PostProcess composite edge issued
// today (glBindFramebuffer(GL_FRAMEBUFFER, 0)), unchanged.

#include "GLBackend.h"

#include <GL/glew.h>

namespace RenderCore {

namespace {

class GLBackend final : public IRenderBackend {
public:
    void bindBackbuffer() override {
        // Verbatim the composite edge's prior direct GL. No behavior change.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void bindFramebuffer(unsigned int fbo) override {
        // Verbatim the routed FBO-bind's prior direct GL. No behavior change.
        // RENDER-BACKEND-IFACE-FBO-1.
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    }

    void setViewport(int x, int y, int w, int h) override {
        // Verbatim the routed composite viewport's prior direct GL. No behavior
        // change. RENDER-BACKEND-IFACE-VIEWPORT-1.
        glViewport(x, y, w, h);
    }

    void clear(unsigned int mask, float r, float g, float b, float a) override {
        // Verbatim the routed letterbox clear's prior direct GL. No behavior
        // change. RENDER-BACKEND-IFACE-CLEAR-1.
        glClearColor(r, g, b, a);
        glClear((GLenum)mask);
    }
};

}  // namespace

IRenderBackend& getGLBackend() {
    static GLBackend s_glBackend;
    return s_glBackend;
}

}  // namespace RenderCore
