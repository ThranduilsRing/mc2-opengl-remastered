// gos_validate.cpp - Validation mode: auto-run, telemetry, screenshot, JSON log
#include "mc2_czstring.h"
#include "gos_validate.h"
#include "gos_crashbundle.h"
#include "gos_screenshot.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include "mc2fmt.h"

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
        mc2::fmt::print_err("VALIDATE: mode enabled, {:d} frames, log={}\n", mc2::fmt::as_i32(s_config.maxFrames), mc2::fmt::safe(s_config.logPath));
    }
}

void validateRecordShaderError(mc2::czstring msg) {
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
    // The screenshot is read back by the present path of the next frame, which is the last one the run draws.
    if (s_config.enabled && s_config.screenshotPath[0] && s_telemetry.framesRendered == s_config.maxFrames - 1)
        gos::screenshot::request(s_config.screenshotPath);
}

bool validateShouldExit() {
    return s_config.enabled && s_telemetry.framesRendered >= s_config.maxFrames;
}

// Escape a string for JSON output (handles backslashes and quotes)
static void writeJsonString(FILE* f, mc2::czstring s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"') mc2::fmt::print(f, "\\\"");
        else if (*s == '\\') mc2::fmt::print(f, "\\\\");
        else if (*s == '\n') mc2::fmt::print(f, "\\n");
        else if (*s == '\r') mc2::fmt::print(f, "\\r");
        else fputc(*s, f);
    }
    fputc('"', f);
}

void validateWriteResults() {
    static bool s_resultsWritten = false;
    if (!s_config.enabled || s_resultsWritten) return;
    s_resultsWritten = true;

    float avgMs = s_telemetry.framesRendered > 0
        ? s_telemetry.totalFrameMs / s_telemetry.framesRendered : 0.0f;

    FILE* f = fopen(s_config.logPath, "w");
    if (!f) {
        mc2::fmt::print_err("VALIDATE: Failed to write log to {}\n", mc2::fmt::safe(s_config.logPath));
        return;
    }

    mc2::fmt::print(f, "{{\n");
    mc2::fmt::print(f, "  \"frames\": {:d},\n", mc2::fmt::as_i32(s_telemetry.framesRendered));
    mc2::fmt::print(f, "  \"avg_frame_ms\": {:.1f},\n", avgMs);
    mc2::fmt::print(f, "  \"max_frame_ms\": {:.1f},\n", s_telemetry.maxFrameMs);

    mc2::fmt::print(f, "  \"shader_errors\": [");
    for (size_t i = 0; i < s_telemetry.shaderErrors.size(); i++) {
        if (i > 0) mc2::fmt::print(f, ", ");
        writeJsonString(f, s_telemetry.shaderErrors[i].c_str());
    }
    mc2::fmt::print(f, "],\n");

    if (s_config.screenshotPath[0]) {
        mc2::fmt::print(f, "  \"screenshot\": ");
        writeJsonString(f, s_config.screenshotPath);
        mc2::fmt::print(f, ",\n");
    }

    mc2::fmt::print(f, "  \"exit_code\": {:d}\n", mc2::fmt::as_i32(s_telemetry.exitCode));
    mc2::fmt::print(f, "}}\n");
    fclose(f);

    mc2::fmt::print_err("VALIDATE: Results written to {} (exit_code={:d}, {:d} frames, {:.1f}ms avg)\n", mc2::fmt::safe(s_config.logPath), mc2::fmt::as_i32(s_telemetry.exitCode), mc2::fmt::as_i32(s_telemetry.framesRendered), avgMs);
}
