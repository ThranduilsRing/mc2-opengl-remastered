#include "gameos.hpp"
#include "gos_render.h"
#include <stdio.h>
#include <time.h>

#include <SDL2/SDL.h>
#include "gos_input.h"

#include "utils/camera.h"
#include "utils/shader_builder.h"
#include "utils/gl_utils.h"
#include "utils/timing.h"
#include "gos_postprocess.h"

#include <signal.h>
#include "gos_profiler.h"

extern void gos_GetTerrainCameraPos(float* x, float* y, float* z);

extern void gos_CreateRenderer(graphics::RenderContextHandle ctx_h, graphics::RenderWindowHandle win_h, int w, int h);
extern void gos_DestroyRenderer();
extern void gos_RendererBeginFrame();
extern void gos_RendererEndFrame();
extern void gos_RendererHandleEvents();
extern void gos_RenderUpdateDebugInput();
extern void gos_RenderEnableDebugDrawCalls();
extern bool gos_RenderGetEnableDebugDrawCalls();

extern bool gosExitGameOS();

extern bool gos_CreateAudio();
extern void gos_DestroyAudio();

static bool g_exit = false;
static bool g_focus_lost = false;
#if 0
static camera g_camera;
#endif

static void handle_key_down( SDL_Keysym* keysym ) {
    switch( keysym->sym ) {
        case SDLK_ESCAPE:
            if(keysym->mod & KMOD_RALT)
                g_exit = true;
            break;
        case 'd':
            if(keysym->mod & KMOD_RALT)
                gos_RenderEnableDebugDrawCalls();
            break;
        case SDLK_F1:
            if (keysym->mod & KMOD_RALT) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->bloomEnabled_ = !pp->bloomEnabled_;
                    fprintf(stderr, "Bloom: %s\n", pp->bloomEnabled_ ? "ON" : "OFF");
                }
            }
            break;
        case SDLK_F2:
            if (keysym->mod & KMOD_RALT) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    if (!pp->showShadowDebug_) {
                        pp->showShadowDebug_ = true;
                        pp->shadowDebugMode_ = 0;
                        fprintf(stderr, "Shadow Debug: STATIC map\n");
                    } else if (pp->shadowDebugMode_ == 0) {
                        pp->shadowDebugMode_ = 1;
                        fprintf(stderr, "Shadow Debug: DYNAMIC map\n");
                    } else {
                        pp->showShadowDebug_ = false;
                        fprintf(stderr, "Shadow Debug: OFF\n");
                    }
                }
            }
            break;
        case SDLK_F3:
            if (keysym->mod & KMOD_RALT) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->shadowsEnabled_ = !pp->shadowsEnabled_;
                    fprintf(stderr, "Shadows: %s\n", pp->shadowsEnabled_ ? "ON" : "OFF");
                }
            }
            break;
        case SDLK_F5:
            if (keysym->mod & KMOD_RALT) {
                bool cur = gos_GetTerrainDrawEnabled();
                gos_SetTerrainDrawEnabled(!cur);
                fprintf(stderr, "Terrain Draw: %s\n", !cur ? "ON" : "OFF");
            }
            break;
        case SDLK_4:
            if (keysym->mod & KMOD_RALT) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    if (!pp->screenShadowEnabled_) {
                        pp->screenShadowEnabled_ = true;
                        pp->screenShadowDebug_ = 0;
                        fprintf(stderr, "Screen Shadows: ON\n");
                    } else if (pp->screenShadowDebug_ == 0) {
                        pp->screenShadowDebug_ = 1;
                        fprintf(stderr, "Screen Shadows: DEBUG (red=terrain, green=lit, blue=shadowed, black=sky)\n");
                    } else {
                        pp->screenShadowEnabled_ = false;
                        pp->screenShadowDebug_ = 0;
                        fprintf(stderr, "Screen Shadows: OFF\n");
                    }
                }
            }
            break;
        case SDLK_9:
            if (keysym->mod & KMOD_RALT) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->ssaoEnabled_ = !pp->ssaoEnabled_;
                    fprintf(stderr, "SSAO: %s\n", pp->ssaoEnabled_ ? "ON" : "OFF");
                }
            }
            break;
        case SDLK_5:
            if (keysym->mod & KMOD_RALT) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->grassEnabled_ = !pp->grassEnabled_;
                    fprintf(stderr, "Grass: %s\n", pp->grassEnabled_ ? "ON" : "OFF");
                }
            }
            break;
        case SDLK_6:
            if (keysym->mod & KMOD_RALT) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->godrayEnabled_ = !pp->godrayEnabled_;
                    fprintf(stderr, "God Rays: %s\n", pp->godrayEnabled_ ? "ON" : "OFF");
                }
            }
            break;
        case SDLK_7:
            if (keysym->mod & KMOD_RALT) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->shorelineEnabled_ = !pp->shorelineEnabled_;
                    fprintf(stderr, "Shorelines: %s\n", pp->shorelineEnabled_ ? "ON" : "OFF");
                }
            }
            break;
    }
}

static void process_events( void ) {

    input::beginUpdateMouseState();

    SDL_Event event;
    while( SDL_PollEvent( &event ) ) {

        if(g_focus_lost) {
            if(event.type != SDL_WINDOWEVENT_FOCUS_GAINED) {
                continue;
            } else {
                g_focus_lost = false;
            }
        }

        switch( event.type ) {
        case SDL_KEYDOWN:
            handle_key_down( &event.key.keysym );
            // fallthrough
        case SDL_KEYUP:
            input::handleKeyEvent(&event);
            break;
        case SDL_QUIT:
            g_exit = true;
            break;
		case SDL_WINDOWEVENT_RESIZED:
			{
				float w = (float)event.window.data1;
				float h = (float)event.window.data2;
				glViewport(0, 0, (GLsizei)w, (GLsizei)h);
                SPEW(("INPUT", "resize event: w: %f h:%f\n", w, h));
			}
			break;
        case SDL_WINDOWEVENT_FOCUS_LOST:
            g_focus_lost = true;
            break;
        case SDL_MOUSEMOTION:
            input::handleMouseMotion(&event); 
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            //input::handleMouseButton(&event);
            break;
        case SDL_MOUSEWHEEL:
            input::handleMouseWheel(&event);
            break;
        }
    }

    input::updateMouseState();
    input::updateKeyboardState();
}

extern bool g_disable_quads;

static void draw_screen( void )
{
    g_disable_quads = false;

    gosPostProcess* pp = getGosPostProcess();

    glCullFace(GL_FRONT);

	const int viewport_w = Environment.drawableWidth;
	const int viewport_h = Environment.drawableHeight;

    if (pp) {
        pp->resize(viewport_w, viewport_h);
        pp->beginScene();
    }

    glViewport(0, 0, viewport_w, viewport_h);
    glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
#if 0
    mat4 proj;
    g_camera.get_projection(&proj);
    mat4 viewM;
    g_camera.get_view(&viewM);
#endif

#if 0
    gos_VERTEX q[4];
    q[0].x = 10; q[0].y = 10;
    q[0].z = 0.0;
    q[0].rhw = 1;
    q[0].argb = 0xffff0000;
    q[1] = q[2] = q[3] = q[0];

    q[1].x = 210; q[1].y = 10;
    q[2].x = 110; q[2].y = 210;
    q[3].x = 10; q[3].y = 210;

    g_disable_quads = false;
    gos_DrawQuads(&q[0], 4);
    g_disable_quads = true;
#endif
	/*
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadTransposeMatrixf((const float*)proj);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadTransposeMatrixf((const float*)viewM);
	*/

    CHECK_GL_ERROR;

    // TODO: reset all states to sane defaults!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    glDepthMask(GL_TRUE);
    // Blue-grey sky during gameplay, black for menus/loading/mech bay
    // Uses previous frame's terrain flag (set during rendering, cleared in beginScene)
    {
        gosPostProcess* ppClear = getGosPostProcess();
        if (ppClear && ppClear->prevFrameHadTerrain_)
            glClearColor(0.55f, 0.62f, 0.72f, 1.0f);
        else
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    }
    glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

    // Skybox disabled — terrain fog provides atmosphere, bright sky looked jarring
    // if (pp) pp->renderSkybox(0.3f, 0.7f, 0.2f);

    {
        ZoneScopedN("Camera.UpdateRenderers");
        gos_RendererBeginFrame();
        Environment.UpdateRenderers();
        gos_RendererEndFrame();
    }

    glUseProgram(0);

    // Composite post-processed scene to default framebuffer
    if (pp) {
        pp->endScene();
    }
    //CHECK_GL_ERROR;
}

extern float frameRate;


const char* getStringForType(GLenum type)
{
	switch (type)
	{
	case GL_DEBUG_TYPE_ERROR: return "DEBUG_TYPE_ERROR";
	case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "DEBUG_TYPE_DEPRECATED_BEHAVIOR";
	case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: return "DEBUG_TYPE_UNDEFINED_BEHAVIOR";
	case GL_DEBUG_TYPE_PERFORMANCE: return "DEBUG_TYPE_PERFORMANCE";
	case GL_DEBUG_TYPE_PORTABILITY: return "DEBUG_TYPE_PORTABILITY";
	case GL_DEBUG_TYPE_MARKER: return "DEBUG_TYPE_MARKER";
	case GL_DEBUG_TYPE_PUSH_GROUP: return "DEBUG_TYPE_PUSH_GROUP";
	case GL_DEBUG_TYPE_POP_GROUP: return "DEBUG_TYPE_POP_GROUP";
	case GL_DEBUG_TYPE_OTHER: return "DEBUG_TYPE_OTHER";
	default: return "(undefined)";
	}
}

const char* getStringForSource(GLenum type)
{
	switch (type)
	{
	case GL_DEBUG_SOURCE_API: return "DEBUG_SOURCE_API";
	case GL_DEBUG_SOURCE_SHADER_COMPILER: return "DEBUG_SOURCE_SHADER_COMPILER";
	case GL_DEBUG_SOURCE_WINDOW_SYSTEM: return "DEBUG_SOURCE_WINDOW_SYSTEM";
	case GL_DEBUG_SOURCE_THIRD_PARTY: return "DEBUG_SOURCE_THIRD_PARTY";
	case GL_DEBUG_SOURCE_APPLICATION: return "DEBUG_SOURCE_APPLICATION";
	case GL_DEBUG_SOURCE_OTHER: return "DEBUG_SOURCE_OTHER";
	default: return "(undefined)";
	}
}

const char* getStringForSeverity(GLenum type)
{
	switch (type)
	{
	case GL_DEBUG_SEVERITY_HIGH: return "DEBUG_SEVERITY_HIGH";
	case GL_DEBUG_SEVERITY_MEDIUM: return "DEBUG_SEVERITY_MEDIUM";
	case GL_DEBUG_SEVERITY_LOW: return "DEBUG_SEVERITY_LOW";
	case GL_DEBUG_SEVERITY_NOTIFICATION: return "DEBUG_SEVERITY_NOTIFICATION";
	default: return "(undefined)";
	}
}
//typedef void (GLAPIENTRY *GLDEBUGPROCARB)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam);
#ifdef PLATFORM_WINDOWS
void GLAPIENTRY OpenGLDebugLog(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
#else
void GLAPIENTRY OpenGLDebugLog(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, GLvoid* userParam)
#endif
{
	if (severity != GL_DEBUG_SEVERITY_NOTIFICATION && severity != GL_DEBUG_SEVERITY_LOW)
	{
		printf("Type: %s; Source: %s; ID: %d; Severity : %s\n",
			getStringForType(type),
			getStringForSource(source),
			id,
			getStringForSeverity(severity)
		);
		printf("Message : %s\n", message);
	}
}

#ifndef DISABLE_GAMEOS_MAIN
int main(int argc, char** argv)
{
    //signal(SIGTRAP, SIG_IGN);

    // gather command line
	size_t cmdline_len = 0;
    for(int i=0;i<argc;++i) {
        cmdline_len += strlen(argv[i]);
        cmdline_len += 1; // ' '
    }
    char* cmdline = new char[cmdline_len + 1];
    size_t offset = 0;
    for(int i=0;i<argc;++i) {
        size_t arglen = strlen(argv[i]);
        memcpy(cmdline + offset, argv[i], arglen);
        cmdline[offset + arglen] = ' ';
        offset += arglen + 1;
    }
    cmdline[cmdline_len] = '\0';

    // fills in Environment structure
    GetGameOSEnvironment(cmdline);

    delete[] cmdline;
    cmdline = NULL;

    int w = Environment.screenWidth;
    int h = Environment.screenHeight;

    graphics::RenderWindowHandle win = graphics::create_window("mc2", w, h);
    if(!win)
        return 1;

    graphics::RenderContextHandle ctx = graphics::init_render_context(win);
    if(!ctx)
        return 1;

    graphics::make_current_context(ctx);

    GLenum err = glewInit();
    if (GLEW_OK != err)
    {
        SPEW(("GLEW", "Error: %s\n", glewGetErrorString(err)));
        return 1;
    }

	glEnable(GL_DEBUG_OUTPUT);
	glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
	//glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 76, 1, "My debug group");
	glDebugMessageControlARB(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
	glDebugMessageCallbackARB((GLDEBUGPROC)&OpenGLDebugLog, NULL);


    SPEW(("GRAPHICS", "Status: Using GLEW %s\n", glewGetString(GLEW_VERSION)));
    //if ((!GLEW_ARB_vertex_program || !GLEW_ARB_fragment_program))
    //{
     //   SPEW(("GRAPHICS", "No shader program support\n"));
      //  return 1;
    //}

    if(!glewIsSupported("GL_VERSION_3_0")) {
        SPEW(("GRAPHICS", "Minimum required OpenGL version is 3.0\n"));
        return 1;
    }

    const char* glsl_version = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    SPEW(("GRAPHICS", "GLSL version supported: %s\n", glsl_version));

    int glsl_maj = 0, glsl_min = 0;
    sscanf(glsl_version, "%d.%d", &glsl_maj, &glsl_min);

    if(glsl_maj < 3 || (glsl_maj==3 && glsl_min < 30) ) {
        SPEW(("GRAPHICS", "Minimum required OpenGL version is 330 ES, current: %d.%d\n", glsl_maj, glsl_min));
        return 1;
    }

    char version[16] = {0};
    snprintf(version, sizeof(version), "%d%d", glsl_maj, glsl_min);
    SPEW(("GRAPHICS", "Using %s shader version\n", version));

    gos_CreateRenderer(ctx, win, w, h);
    if(!gos_CreateAudio())
    {   // not an error
        SPEW(("AUDIO", "Failed to create audio\n"));
    }

    Environment.InitializeGameEngine();

#if 0
	float aspect = (float)w/(float)h;
	mat4 proj_mat = frustumProjMatrix(-aspect*0.5f, aspect*0.5f, -.5f, .5f, 1.0f, 100.0f);
	g_camera.set_projection(proj_mat);
	g_camera.set_view(mat4::translation(vec3(0, 0, -16)));
#endif

	timing::init();
    TracyGpuContext;

    while( !g_exit ) {

		uint64_t start_tick = timing::gettickcount();
		timing::sleep(10*1000000);

        {
            ZoneScopedN("GameLogic");
            if(gos_RenderGetEnableDebugDrawCalls()) {
                gos_RenderUpdateDebugInput();
            } else {
                Environment.DoGameLogic();
            }
        }

        process_events();

		gos_RendererHandleEvents();

        {
            ZoneScopedN("DrawScreen");
            graphics::make_current_context(ctx);
            draw_screen();
        }

        {
            ZoneScopedN("SwapWindow");
            graphics::swap_window(win);
        }

        TracyGpuCollect;
        FrameMark;

        g_exit |= gosExitGameOS();

		uint64_t end_tick = timing::gettickcount();
		uint64_t dt = timing::ticks2ms(end_tick - start_tick);
		frameRate = 1000.0f / (float)dt;
    }
    
    Environment.TerminateGameEngine();

    gos_DestroyRenderer();

    graphics::destroy_render_context(ctx);
    graphics::destroy_window(win);

    gos_DestroyAudio();

    return 0;
}
#endif // DISABLE_GAMEOS_MAIN
