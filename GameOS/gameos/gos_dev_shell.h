// gos_dev_shell.h - DEV-SHELL-1: localhost dev command socket.
// Remote-control spine for live engine iteration (write half of the
// MC2_DEBUG_STATE_DUMP read bridge). Gate: MC2_DEV_SHELL=1, default OFF —
// when unset both hooks are a single static bool check, no socket, no
// behavior change. Localhost-only (127.0.0.1), nonblocking; every command
// executes on the main thread at the poll point (which owns the GL context).
// Protocol: newline-delimited JSON request {"type":cmd,"params":{...}},
// reply {"ok":bool,"error":str|null,"version":1,"data":{...}}.
// Client: tools/dev_shell/mc2_cmd.py. Port: 9877 (MC2_DEV_SHELL_PORT).
#pragma once

#include <stdint.h>
#include <string>

namespace gos_dev_shell {

// Poll the socket and execute any complete pending commands. Called once per
// frame from the gameosmain loop (after gos_RendererHandleEvents). Returns
// true when a "quit" command was received — caller sets g_exit.
bool pollCommands(uint32_t frame);

// Execute a pending screenshot request, if any. Called at the post-render
// capture point (same site as [SCREENSHOT v1] / visual capture — sceneFBO is
// intact just before swap). No-op when nothing pending.
void capturePendingScreenshot(uint32_t frame);

// Extension seam: upper layers (game code, editor) register their own shell
// commands without gameos linking back at them — same inversion as the
// GuiRuntime panel registrations. Handler runs on the main thread at the
// poll point; returns true + fills dataJsonOut (a JSON object, "{}" ok) on
// success, false + errorOut on failure. Registering an existing name
// replaces it. Registration is cheap and safe when the shell gate is OFF
// (handlers just never fire).
typedef bool (*CommandHandler)(const char* paramsJson,
                               std::string& dataJsonOut,
                               std::string& errorOut);
void registerCommand(const char* name, CommandHandler fn);

// RESULTS-CAPTURE-1: engine-side screenshot scheduling — game code (e.g.
// MissionResults) can request the same end-of-frame capture the socket
// "screenshot" command uses. No-op when the shell gate is OFF or a shot is
// already pending. name is sanitized like the socket path.
void scheduleScreenshot(const char* name, bool backbuffer);

}  // namespace gos_dev_shell
