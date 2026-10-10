# Phase split spec for main (GameOS/gameos/gameosmain.cpp). Line numbers are 1-based lines of the file at BASE.
BASE = '5f3a9f992'
SRC = 'GameOS/gameos/gameosmain.cpp'
FUNC_SIG = r'^int main\(int argc, char\*\* argv\)'
INDENT = '    '
DEDENT = ''
STRUCTS = {}
FRAME_DOC = ''
ORCH_DOC = ('// Engine entry: gate / log / config bring-up, the window and render context, the frame loop and the shutdown.\n'
            '// The early-exit checks, the gate invalidation, the [CONFIG v1] dump and every other ordering-critical statement stay\n'
            '// here in their original order; the large self-contained blocks are the file-static helpers above, called at the\n'
            '// same points.')

# What main owns (the window, the context, the snapshot) is passed to the helpers that name it.
PARAMS = [
    ('win', 'graphics::RenderWindowHandle'),
    ('ctx', 'graphics::RenderContextHandle'),
    ('w', 'int'),
    ('h', 'int'),
    ('snap', 'RenderSnapshot&'),
    ('s_vsync_active', 'bool'),
    ('s_fps_cap_mission', 'int'),
    ('s_fps_cap_menu', 'int'),
    ('start_tick', 'uint64_t'),
]
MEMBERS = []
EDITS = {}
ORCH_BEFORE = {}
INIT = 'GameOS/gameos/gameosmain_init.cpp'
FRAME = 'GameOS/gameos/gameosmain_frame.cpp'
INTERNAL_INCLUDES = ['#include "gameosmain_internal.h"']
FILE_HEADS = {
    INIT: ['// main: the start-up helpers -- DPI awareness, the launcher hand-off and launcher_env.json, the instrumentation banner,',
           '// the pre-window resolution and the Vulkan backend bring-up. The orchestrator is in gameosmain.cpp.'],
    FRAME: ['// main: the per-frame helpers -- draw-packet emit, the screenshot oracle, the load-race probes, the camera park and the swap.',
            '// The orchestrator is in gameosmain.cpp.'],
}

PHASES = [('initDpiAwareness',
  942,
  961,
  'Opt the process into per-monitor DPI awareness unless MC2_DPI_UNAWARE is set.',
  {'file': 'GameOS/gameos/gameosmain_init.cpp'}),
 ('initLauncherBootstrap',
  962,
  999,
  'Hand a direct start off to mc2-launcher.exe; true when the launcher was started and this process must exit.',
  {'exits': '0', 'file': 'GameOS/gameos/gameosmain_init.cpp'}),
 ('initLauncherEnv',
  1001,
  1076,
  'Apply launcher_env.json to the process environment on the MC2_NO_LAUNCHER path.',
  {'file': 'GameOS/gameos/gameosmain_init.cpp'}),
 ('initInstrBanner',
  1147,
  1284,
  'Read the instrumentation gates, initialise the object recon and emit the [INSTR v1] banner.',
  {'file': 'GameOS/gameos/gameosmain_init.cpp'}),
 ('initResolutionFromConfig',
  1326,
  1444,
  'Pick the pre-window resolution and fullscreen state from the environment or options.cfg.',
  {'file': 'GameOS/gameos/gameosmain_init.cpp'}),
 ('initVulkanBackend',
  1521,
  1595,
  'Birth the engine Vulkan context and present path when the Vulkan backend is selected.',
  {'file': 'GameOS/gameos/gameosmain_init.cpp'}),
 ('frameThrottleLogicEvents',
  1702,
  1741,
  'Throttle while the window is invisible, then run the game logic and the event pumps.',
  {'file': 'GameOS/gameos/gameosmain_frame.cpp'}),
 ('frameEmitDrawPackets',
  1757,
  2091,
  'Emit the draw packets and the debug / parity records for this frame snapshot.',
  {'file': 'GameOS/gameos/gameosmain_frame.cpp'}),
 ('frameScreenshotOracle',
  2115,
  2178,
  'Capture the deterministic backbuffer screenshot at the requested frame.',
  {'file': 'GameOS/gameos/gameosmain_frame.cpp'}),
 ('frameLoadRaceVerdict', 2180, 2234, 'Emit the one-shot load-race verdict at its frame.', {'file': 'GameOS/gameos/gameosmain_frame.cpp'}),
 ('frameLoadRaceProbes',
  2236,
  2352,
  'Dump the load-race transient probes at their frames.',
  {'file': 'GameOS/gameos/gameosmain_frame.cpp'}),
 ('frameCameraPark',
  2354,
  2402,
  'Pin the camera to the MC2_CAMERA_PARK pose once its frame arrives.',
  {'file': 'GameOS/gameos/gameosmain_frame.cpp'}),
 ('frameFrameCap',
  2603,
  2627,
  'Sleep to the mission / menu frame cap unless vsync paces the loop.',
  {'file': 'GameOS/gameos/gameosmain_frame.cpp'}),
 ('frameSwapWindow',
  2414,
  2601,
  'Present the frame: pre-swap probes, the swap and the heartbeat.',
  {'file': 'GameOS/gameos/gameosmain_frame.cpp'})]
