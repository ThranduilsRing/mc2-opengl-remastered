// gos_validate.cpp - Validation mode: auto-run, telemetry, screenshot, JSON log
#include "gos_validate.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <GL/glew.h>

static ValidateConfig s_config = {};
static ValidateTelemetry s_telemetry = {};

ValidateConfig& getValidateConfig() { return s_config; }
ValidateTelemetry& getValidateTelemetry() { return s_telemetry; }

void validateParseArgs(int argc, char** argv) {
    memset(&s_config, 0, sizeof(s_config));
    s_config.maxFrames = 60;
    s_config.bloomOverride = -1;
    s_config.shadowsOverride = -1;
    s_config.fxaaOverride = -1;
    s_config.grassOverride = -1;
    strncpy(s_config.logPath, "validate.json", sizeof(s_config.logPath) - 1);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--validate") == 0) {
            s_config.enabled = true;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            s_config.maxFrames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            strncpy(s_config.screenshotPath, argv[++i], sizeof(s_config.screenshotPath) - 1);
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            strncpy(s_config.logPath, argv[++i], sizeof(s_config.logPath) - 1);
        } else if (strcmp(argv[i], "--enable") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "bloom") == 0) s_config.bloomOverride = 1;
            else if (strcmp(argv[i], "shadows") == 0) s_config.shadowsOverride = 1;
            else if (strcmp(argv[i], "fxaa") == 0) s_config.fxaaOverride = 1;
            else if (strcmp(argv[i], "grass") == 0) s_config.grassOverride = 1;
        } else if (strcmp(argv[i], "--disable") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "bloom") == 0) s_config.bloomOverride = 0;
            else if (strcmp(argv[i], "shadows") == 0) s_config.shadowsOverride = 0;
            else if (strcmp(argv[i], "fxaa") == 0) s_config.fxaaOverride = 0;
            else if (strcmp(argv[i], "grass") == 0) s_config.grassOverride = 0;
        }
    }

    if (s_config.enabled) {
        fprintf(stderr, "VALIDATE: mode enabled, %d frames, log=%s\n",
                s_config.maxFrames, s_config.logPath);
    }
}

void validateRecordShaderError(const char* msg) {
    if (s_config.enabled) {
        s_telemetry.shaderErrors.push_back(msg);
        s_telemetry.exitCode = 1;
    }
}

void validateRecordFrame(float frameMs) {
    s_telemetry.framesRendered++;
    s_telemetry.totalFrameMs += frameMs;
    if (frameMs > s_telemetry.maxFrameMs)
        s_telemetry.maxFrameMs = frameMs;
}

bool validateShouldExit() {
    return s_config.enabled && s_telemetry.framesRendered >= s_config.maxFrames;
}

static void writeScreenshotTGA(const char* path, int w, int h) {
    unsigned char* pixels = new unsigned char[w * h * 3];
    glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, pixels);

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "VALIDATE: Failed to write screenshot to %s\n", path);
        delete[] pixels;
        return;
    }

    // TGA header: uncompressed true-color
    unsigned char header[18] = {};
    header[2] = 2;
    header[12] = w & 0xFF;
    header[13] = (w >> 8) & 0xFF;
    header[14] = h & 0xFF;
    header[15] = (h >> 8) & 0xFF;
    header[16] = 24;

    fwrite(header, 1, 18, f);
    fwrite(pixels, 1, w * h * 3, f);
    fclose(f);
    delete[] pixels;

    fprintf(stderr, "VALIDATE: Screenshot saved to %s (%dx%d)\n", path, w, h);
}

// Escape a string for JSON output (handles backslashes and quotes)
static void writeJsonString(FILE* f, const char* s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"') fprintf(f, "\\\"");
        else if (*s == '\\') fprintf(f, "\\\\");
        else if (*s == '\n') fprintf(f, "\\n");
        else if (*s == '\r') fprintf(f, "\\r");
        else fputc(*s, f);
    }
    fputc('"', f);
}

void validateWriteResults(int viewportW, int viewportH) {
    static bool s_resultsWritten = false;
    if (!s_config.enabled || s_resultsWritten) return;
    s_resultsWritten = true;

    if (s_config.screenshotPath[0]) {
        writeScreenshotTGA(s_config.screenshotPath, viewportW, viewportH);
    }

    // Drain any remaining GL errors
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        char buf[64];
        snprintf(buf, sizeof(buf), "GL_ERROR: 0x%04X", err);
        s_telemetry.glErrors.push_back(buf);
        s_telemetry.exitCode = 1;
    }

    float avgMs = s_telemetry.framesRendered > 0
        ? s_telemetry.totalFrameMs / s_telemetry.framesRendered : 0.0f;

    FILE* f = fopen(s_config.logPath, "w");
    if (!f) {
        fprintf(stderr, "VALIDATE: Failed to write log to %s\n", s_config.logPath);
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"frames\": %d,\n", s_telemetry.framesRendered);
    fprintf(f, "  \"avg_frame_ms\": %.1f,\n", avgMs);
    fprintf(f, "  \"max_frame_ms\": %.1f,\n", s_telemetry.maxFrameMs);

    fprintf(f, "  \"gl_errors\": [");
    for (size_t i = 0; i < s_telemetry.glErrors.size(); i++) {
        if (i > 0) fprintf(f, ", ");
        writeJsonString(f, s_telemetry.glErrors[i].c_str());
    }
    fprintf(f, "],\n");

    fprintf(f, "  \"shader_errors\": [");
    for (size_t i = 0; i < s_telemetry.shaderErrors.size(); i++) {
        if (i > 0) fprintf(f, ", ");
        writeJsonString(f, s_telemetry.shaderErrors[i].c_str());
    }
    fprintf(f, "],\n");

    if (s_config.screenshotPath[0]) {
        fprintf(f, "  \"screenshot\": ");
        writeJsonString(f, s_config.screenshotPath);
        fprintf(f, ",\n");
    }

    fprintf(f, "  \"exit_code\": %d\n", s_telemetry.exitCode);
    fprintf(f, "}\n");
    fclose(f);

    fprintf(stderr, "VALIDATE: Results written to %s (exit_code=%d, %d frames, %.1fms avg)\n",
            s_config.logPath, s_telemetry.exitCode, s_telemetry.framesRendered, avgMs);
}
