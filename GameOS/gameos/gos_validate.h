// gos_validate.h - Validation mode telemetry for autonomous dev iteration
#pragma once

#include <vector>
#include <string>

struct ValidateConfig {
    bool enabled;
    int maxFrames;
    char screenshotPath[512];
    char logPath[512];
    // Feature overrides: -1=no override, 0=off, 1=on
    int bloomOverride;
    int shadowsOverride;
    int fxaaOverride;
    int grassOverride;
};

struct ValidateTelemetry {
    int framesRendered;
    float totalFrameMs;
    float maxFrameMs;
    std::vector<std::string> glErrors;
    std::vector<std::string> shaderErrors;
    int exitCode; // 0=success, 1=error
};

ValidateConfig& getValidateConfig();
ValidateTelemetry& getValidateTelemetry();

void validateParseArgs(int argc, char** argv);
void validateRecordShaderError(const char* msg);
void validateRecordFrame(float frameMs);
bool validateShouldExit();
void validateWriteResults(int viewportW, int viewportH);

// Tier-1 instrumentation: drain the GL error queue at a render-pass
// boundary. Consumes pending errors (see spec §4.3). Always safe to call
// — if there are no errors, it's a single glGetError() returning GL_NO_ERROR.
void drainGLErrors(const char* pass);
