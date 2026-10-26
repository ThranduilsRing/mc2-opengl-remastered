<!-- doc-status: current | reviewed:2026-10-11 -->
# Example: Live-Tuning With the In-Game ImGui Panels

> **AVAILABILITY**
> - **Classification: this is a LIVE-TOOLS tutorial, not a distributable mod.** Parts 1 and 3 (tuning + persisting) produce one small `data/`-tree file you *can* ship inside a mod overlay. Part 2 ("add your own panel") is C++ engine-source work — it needs a rebuild and **cannot** ship as a `.mc2mod` package at all. See [`getting-started.md`](../getting-started.md) §1 for the data-mod-vs-engine-source distinction.
> - **Works on:** this checkout, **provided `mc2.exe` was built with `-DMC2_IMGUI=ON`** (the CMake option `MC2_IMGUI` defaults **OFF** — `CMakeLists.txt:104` — but the canonical build command in this repo's `CLAUDE.md` passes `-DMC2_IMGUI=ON`, and the shipped `A:/Games/mc2-opengl/mc2-win64-v0.4` deploy has it on, verified by the presence of `imgui.ini` and a populated `data/visual_tuning.json` at that path). If your own build was configured without that flag, every hotkey in this doc is a no-op and there's nothing to fix on the data side — you need a different build.
> - **Runtime gates (all env vars, all default ON — you only ever need to set them to `0` to turn something off):**
>   - `MC2_IMGUI` — master runtime switch for the whole ImGui layer (`GuiRuntime/GuiRuntime.cpp:25-32`). Unset or non-`"0"` = on.
>   - `MC2_IMGUI_INSPECTOR` — gates **both** the Object Inspector *and* the Renderer Features window (they share one `isEnabled()` check, `GuiRuntime/EditorInspector.cpp:100-107, 220-221`). Unset or non-`"0"` = on. (If you've seen other docs say this one is opt-in/off-by-default, that's wrong for this checkout — verified by reading the source, not assumed.)
> - **Hotkeys** (work in the live game, `mc2.exe` — not editor-only): `Ctrl+Shift+G` Graphics Options, `Ctrl+Shift+I` Object Inspector, `Ctrl+Shift+F` Renderer Features, `Ctrl+Shift+LMB` pick an object for the inspector.
> - **Two unrelated env vars sometimes confused with this feature do not exist on this checkout:** `MC2_EQUIPMENT_ADD` and `MC2_UNIT_PROFILE_DATA` (branch `claude/unit-profile-seam` only). They have nothing to do with ImGui; mentioned here only because this doc's payload also touches `data/`, and stray copy-paste from other example docs is an easy mistake.

Prerequisite reading: [`getting-started.md`](../getting-started.md) (mod folder anatomy, `MC2_ACTIVE_MOD`, the launcher, `mc2mod.py`) and [`mechanism-map.md`](../mechanism-map.md) (this doc's row is "In-game debug UI" near the bottom).

---

## Part 1 — Using the shipped panels to preview/tune your mod live

### 1.1 Open the panels

1. Launch `mc2.exe` (or the launcher, with or without a mod active — the panels work in both base game and any mod, since they're pure engine UI, not `data/`-tree content) and get into a mission.
2. Press **`Ctrl+Shift+G`** — opens **Graphics Options**. This is the big one: 16 collapsing sections covering post-process, terrain, draw packets, render passes, static props, mechs, water, VFX, HUD, a GBuffer preview, a live env-gate readout, debug overlays, and the tuning-profile controls you'll use in Part 3.
3. Press **`Ctrl+Shift+F`** — opens **Renderer Features**, a standalone read-only table of every `RenderCore::RendererFeature` entry (`RenderCore/RendererFeatureRegistry.h`): feature id, its env var, default state, and live on/off status. Hover a feature name for its one-line doc string. This window is *just* a readout — nothing here is editable, it's for figuring out what a given `MC2_*` var currently controls before you go set it.
4. Aim your cursor at a mech or static prop and press **`Ctrl+Shift+LMB`** — opens the **Object Inspector**, pre-populated with the object under the cursor (render-object handle, world position, mesh/material handles, LOD, and for props a full `MaterialGpu` dump: albedo/normal/metallic-roughness/emissive texture handles + flags + PBR factors). Terrain picks show world position only (no `RenderWorld` lookup for terrain). There's also a **Copy All** button that formats the whole panel to your clipboard — handy for pasting into a bug report or an asset-cook debugging session.

All three windows dock/float independently and stay open across `Ctrl+Shift+G`/`F`/`I` toggles until you close them again with the same chord or the window's own close button.

### 1.2 Worked example — tuning terrain lighting and shadow softness for a mission

This walks through the exact controls the payload in this folder was produced from.

1. Load mission `mc2_01` (or any mission — the technique is identical).
2. `Ctrl+Shift+G` → expand **Post-Process** → find **Shadow Tuning** → drag the **Softness** slider. You'll see shadow edges on terrain/props go from crisp to blurry in real time (default `0.9`).
3. Still in Graphics Options, expand **Terrain Tuning** (marked "Heavy section (15 sliders) — collapsed by default"). Try:
   - **Phong Smoothing → Alpha** — 0 = flat linear terrain shading, 1 = fully Phong-smoothed. Most visible on hilly ground.
   - **Material Tint Colors** — `ColorEdit3` swatches per surface material (rock/grass/dirt), mixed with the colormap.
   - **Per-Material Tiling** — UV repeat multiplier per material; useful if a replacement texture tiles at the wrong frequency.
4. Scroll further down in Graphics Options to **Post-Process → Track V (HDR / Bloom / Tonemap / SSAO)** and toggle **HDR Post (master)**. This one is gated by env var `MC2_HDR_POST` (default off) — the checkbox is a live in-session override; the tooltip literally says the env var only *seeds* the startup state.
5. **What you should see:** shadows on the terrain immediately get softer/harder as you drag the softness slider (no restart, no reload); the terrain tint swatches recolor the ground live; toggling HDR Post changes the whole scene's tonemapping the instant you click the checkbox.

### 1.3 Persist your tuning

This is the part that turns a live session into something you can actually ship. **Not every control in the panel persists the same way** — there are three tiers, and mixing them up is the #1 way to lose your tuning on next launch. Verified by reading `GameOS/gameos/visual_tuning_profile.cpp` and `GameOS/gameos/gos_postprocess.cpp` directly, not assumed from the UI.

#### Tier A — the ~24 keys covered by `data/visual_tuning.json` (recommended path)

> **BLOCKER, READ BEFORE YOU PICK A PERSISTENCE OPTION:** `visual_tuning_profile.cpp` loads its file with a raw `fopen(path, "r")` (`GameOS/gameos/visual_tuning_profile.cpp:317`), where `path` defaults to the literal string `"data/visual_tuning.json"` resolved against the game process's current working directory (`:152,314`). **This never goes through the mod-aware VFS resolver** (`File::open`, `mclib/file.cpp:907`) that every other `data/`-tree asset uses — and `mc2.exe` never `chdir`s into a mod folder either. So installing this as a normal mod and activating it via the launcher or `MC2_ACTIVE_MOD` does **nothing**: the engine keeps opening the base deploy root's `data/visual_tuning.json` and silently ignores your mod's copy (missing-file is a silent no-op, and here the base file exists, so it just silently applies base values — no error, no log line). [`docs/visual-tuning-profiles.md`](../../../visual-tuning-profiles.md) doesn't mention this either — it's silent on mod/overlay behavior entirely, so don't expect it to warn you.
>
> The **only** shippable path for this specific file is **Tier A "Direct-to-mod"** below, using `$env:MC2_VISUAL_TUNING_FILE`, which *is* honored (`:313-314`). Skip straight past "Simple" — it's documented here only so you understand why it doesn't work if you've seen it suggested elsewhere. This tutorial's own final install-and-verify walkthrough at the bottom of this page uses the Direct-to-mod / launch-script pattern for exactly this reason.

The **Visual Tuning Profile** section at the bottom of Graphics Options is a small file-backed system (`MISSION-VISUAL-TUNING-1`, fully documented in [`docs/visual-tuning-profiles.md`](../../../visual-tuning-profiles.md) — read that for the full key table and precedence rules). Short version:

- It reads `data/visual_tuning.json` (or `$env:MC2_VISUAL_TUNING_FILE` if set) once per mission load and applies a `"defaults"` block, then a per-mission `"missions".<name>` override block, to a fixed vocabulary of ~22 float tunables: `exposure`, `shadowSoftness`, `staticPropIblStrength`, `waterSkyTintStrength`, `vfxBrightness`/`vfxAdditiveBrightness`/`vfxLitStrength`, `bloomThreshold`/`bloomIntensity`, `aoRadius`/`aoStrength`/`aoBias`, `mechAmbientStrength`/`mechSpecularStrength`, and 8 `terrainClass*` RGB-classifier thresholds (14 named keys + 8 terrainClass thresholds, counted directly off every `strcmp(key, ...)` branch in `applyKey()`). **Every slider you dragged in step 1.2 that maps to one of these keys is covered; everything else in the panel is not (see Tiers B/C below).**
- Precedence, highest wins: **ImGui live slider > env var > profile mission block > profile defaults block > engine hardcoded default.**
- Click **Set as Mission Defaults** in that section. It snapshots the *current* value of all ~24 keys (not just the ones you touched) and writes them into `"missions"."<active mission>"` in the JSON file at whatever path Graphics Options shows under **File:** — by default `data/visual_tuning.json` in your game's working directory (i.e. your `mc2-win64-v0.4`-equivalent install root, **not** your mod folder) unless you've already set `MC2_VISUAL_TUNING_FILE` per below.

To turn that into something shippable, pick one:

- ~~**Simple:** copy the resulting `data/visual_tuning.json` into `mods/<your-id>/data/visual_tuning.json`.~~ **Does not work for this file — see the blocker box above.** This would be the normal whole-file-shadow pattern (per `getting-started.md` §3) and it's how most `data/` content ships, but `visual_tuning_profile.cpp` bypasses the mod VFS, so a copy sitting in your mod's `data/` folder is never read by the running game. Don't use this path.
- **Direct-to-mod (the only path that works):** *before launching*, set `$env:MC2_VISUAL_TUNING_FILE = "mods/<your-id>/data/visual_tuning.json"` so the button writes straight into your mod's overlay path **and**, critically, so the engine also *reads* from that exact path on every subsequent launch — sidestepping the VFS bypass entirely because you're pointing the raw `fopen()` directly at your mod folder. Like `MC2_ACTIVE_MOD`, this is read once at process start (cached into a static in `visual_tuning_profile.cpp`) — set it before launch, not while the game is running. This env var must be set on **every** launch that should use your tuning (including the player's), so ship it via a launch script — see the Tier B launch-script pattern below; the same script pattern applies here.

This example's payload (`mod/data/visual_tuning.json` in this folder) is a **hand-trimmed** example — it only lists the 2 keys I actually changed for `mc2_01` (`exposure`, `shadowSoftness`), to keep the example readable. That's legal: per the schema doc, any key the file omits just falls back to the engine default, and `applyKey()` processes each key independently. A real **Set as Mission Defaults** click writes *all* ~22 keys every time (it round-trips the full vocabulary) — don't be surprised when your own file is longer than this example's. Because of the Tier-A blocker above, this payload also ships `mod/launch-with-tuning.ps1`, which is the only thing that actually makes the engine read this file — see "This folder's payload" at the bottom of this page.

#### Tier B — env-var-seeded checkboxes (session-only unless you also set the env var)

Several Graphics Options checkboxes (Track V's **HDR Post**, **Bloom**, **Tonemap ACES**, **SSAO**, and others) read an `MC2_*` env var *once at startup* to seed a runtime bool, then let the checkbox override it live for the rest of the session — but the checkbox never writes back to anything. If you want the same choice on your next launch, you have to set the env var yourself: `MC2_HDR_POST=1`, `MC2_BLOOM=1`, `MC2_SSAO=1`, etc. (Check the **Renderer Features** window, `Ctrl+Shift+F`, for the exact var name and default for any given feature — that's exactly what it's for.)

The in-game picker persists settings in `launcher_env.json`; see `GuiRuntime/PlayerFrontEnd.cpp`. Shell overrides take precedence. A companion launch script can also seed a session:

```powershell
# mods/<your-id>/launch-with-tuning.ps1 -- ships NEXT TO the mod folder, not inside
# mods/<your-id>/data/ (env vars aren't moddable "data/" content -- this is just
# a convenience script the player double-clicks or runs instead of mc2.exe directly).
$env:MC2_ACTIVE_MOD = "your-id"
$env:MC2_HDR_POST   = "1"
$env:MC2_BLOOM      = "1"
& "$PSScriptRoot\..\..\mc2.exe"
```

#### Tier C — no persistence path at all (verified, not assumed)

Some panel controls have **no env var backing whatsoever** — they're plain C++ constructor defaults (`gos_postprocess.cpp:162,166,170`) that the checkbox flips for the current process only, with nothing to set for next launch: **Screen Shadows**, **God Rays**, and **Shorelines** in the Post-Process section, and every raw `gos_Set*Terrain*`/`gos_Set*Mech*` slider that doesn't appear in the Tier-A key list above (e.g. **Terrain Draw**, **Terrain Wireframe**, **Terrain Tuning → Tessellation/Displacement/Normal Maps/Per-Material Normal Boost**, **Static Prop Tuning**'s roughness override, **Mech**'s ambient/specular *enable* checkboxes as opposed to their *strength* — strength is Tier A, enable is Tier C). If you need one of these to default a certain way every launch, that's an engine-source change (add a `getenv()` check next to the existing hardcoded default) — out of scope for a data mod. Don't tell players "just set `MC2_SCREEN_SHADOW=0`" for these; that var doesn't exist.

**Rule of thumb:** if a slider's value shows up in the **Visual Tuning Profile** panel's key count after you touch it, it's Tier A — ship it via `data/visual_tuning.json`. If it's a checkbox whose tooltip or the Renderer Features table names an `MC2_*` var, it's Tier B — ship a launch script. Otherwise assume Tier C and don't promise persistence you can't deliver.

---

## Part 2 — Adding your own panel (documentation only — requires a C++ rebuild)

**This section is reference material for code-comfortable modders. Nothing in this tutorial's payload edits engine code — do not apply this snippet as part of following this doc.** Per [`mechanism-map.md`](../mechanism-map.md) ("Panel dispatch" row): *"Hardcoded C++ call list ... no data-driven panel registry exists."* There is no plugin system, no `data/`-tree panel description format, nothing you can drop into a mod. A new panel is a genuine engine-source change: it needs `GuiRuntime/CMakeLists.txt` to know about a new `.cpp`, and a full `mc2` rebuild per this repo's `CLAUDE.md` build section.

### Where it hangs off `GuiRuntime::Render()`

`GuiRuntime::Render()` (`GuiRuntime/GuiRuntime.cpp:321-338`) currently calls exactly two draw functions, in order, before `ImGui::Render()`:

```cpp
// GuiRuntime/GuiRuntime.cpp:331-332
EditorInspector::drawImGui();
GraphicsOptionsWindow::draw();
```

A new panel is a third call in that same spot. `GraphicsOptionsWindow.h` even documents the contract for you: `void draw(); // call from GuiRuntime::Render(), before ImGui::Render()`.

### Minimal panel skeleton

Following the exact pattern `GraphicsOptionsWindow.cpp` and `EditorInspector.cpp` both use — own hotkey via `ImGui::IsKeyChordPressed`, own open/closed static, own `Begin`/`End` pair:

```cpp
// GuiRuntime/MyModPanel.h  (documentation only -- not part of this tutorial's payload)
#pragma once
namespace MyModPanel {
    void draw();   // call from GuiRuntime::Render(), before ImGui::Render()
}
```

```cpp
// GuiRuntime/MyModPanel.cpp  (documentation only -- not part of this tutorial's payload)
#include "MyModPanel.h"
#include "imgui.h"
#include <cstdlib>   // getenv
#include <cstring>   // strcmp

namespace MyModPanel {

static bool isEnabled() {
    // Same convention as GuiRuntime.cpp/EditorInspector.cpp: absent or
    // non-"0" = on. Give your panel its own env var so players can turn it
    // off without touching MC2_IMGUI_INSPECTOR/MC2_IMGUI.
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("MC2_MYMOD_PANEL");
        cached = (!v || v[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

static bool s_open = false;

void draw() {
    if (!isEnabled()) return;

    // Pick a chord nothing else in GuiRuntime.cpp/EditorInspector.cpp/
    // GraphicsOptionsWindow.cpp already uses -- Ctrl+Shift+G/I/F are taken.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_M))
        s_open = !s_open;
    if (!s_open) return;

    ImGui::SetNextWindowSize(ImVec2(360, 240), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("My Mod Panel", &s_open)) { ImGui::End(); return; }

    ImGui::TextUnformatted("Hello from a custom panel.");
    // ... your controls here ...

    ImGui::End();
}

}  // namespace MyModPanel
```

Then, purely as documentation of what the wiring would look like (again: do not actually make these edits as part of this tutorial):

```cpp
// GuiRuntime/GuiRuntime.cpp -- inside GuiRuntime::Render(), alongside the two
// existing calls:
#include "MyModPanel.h"
...
EditorInspector::drawImGui();
GraphicsOptionsWindow::draw();
MyModPanel::draw();             // <-- new
```

```cmake
# GuiRuntime/CMakeLists.txt -- add the new source file to the existing target:
add_library(gui_runtime STATIC
    GuiRuntime.cpp
    EditorInspector.cpp
    GraphicsOptionsWindow.cpp
    MyModPanel.cpp              # <-- new
)
```

A real change here would need the full `RelWithDebInfo` build + deploy dance from this repo's `CLAUDE.md` (delete stale `.obj`s / `--clean-first` if you touch a header the rest of the target depends on, redeploy shaders in lockstep even though this particular change wouldn't touch any) — none of which this tutorial performs.

---

## Troubleshooting

| Symptom | Likely cause | Check |
|---|---|---|
| No panels open at all, any hotkey | Build has `MC2_IMGUI` compiled out (CMake option defaults OFF) | Confirm your build was configured `-DMC2_IMGUI=ON`; look for `imgui.ini` next to your `mc2.exe` as a quick signal it was |
| `Ctrl+Shift+G` works but `Ctrl+Shift+I`/`Ctrl+Shift+F` don't | `MC2_IMGUI_INSPECTOR=0` is set somewhere (env, or a wrapper script) | `echo $env:MC2_IMGUI_INSPECTOR` in the shell you're launching from; unset it or set it to `1` |
| Panels open in the wrong dock position / overlap the HUD oddly | `imgui.ini` has stale layout state from a previous session/panel set | Delete `imgui.ini` next to `mc2.exe` and relaunch (auto-dock rebuilds the default layout; `MC2_EDITOR_AUTODOCK=0` disables auto-dock if you'd rather keep a saved layout) |
| "Set as Mission Defaults" button is greyed out / does nothing | No mission is loaded yet (button only appears once `mission` is non-empty in the panel) or the working directory isn't writable | Load a mission first; check Graphics Options → Visual Tuning Profile → **File:** path is where you expect, and that path isn't read-only |
| Your mod's `data/visual_tuning.json` doesn't seem to apply | Wrong relative path, or another mod/dependency higher in the priority chain also ships that path and wins (whole-file shadow, first-wins) | `$env:MC2_LOG_FILE_RESOLVE = "1"`; relaunch; look for `[mod]`/`[mod-dup]` lines naming `data/visual_tuning.json` to see which layer actually won |
| You added/removed files under `mods/<id>/data/tgl/<x>/textures/...` (3+ levels deep) and the game doesn't see the change | Mod index cache trap — the 2-level mtime sweep doesn't see level-3+ changes (`getting-started.md` §6) | `$env:MC2_REBUILD_MOD_CACHE = "1"`; relaunch once, then unset |

---

## This folder's payload

```
mod/
  mod.json                    <- id "imgui-tuning-demo", type "campaign"
  launch-with-tuning.ps1      <- REQUIRED to actually see the tuning apply -- see Part 1.3
  data/
    visual_tuning.json        <- Tier-A example: 3 hand-picked keys under "missions"."mc2_01"
```

`launch-with-tuning.ps1` sits at the top of the mod folder (sibling to `mod.json`/`data/`), not inside `data/` — it's a convenience script, not moddable `data/` content, and `mc2mod.py pack` walks the whole mod directory (excluding only dot-prefixed files/dirs and `src/`/`out/`), so it packs and installs like any other file here.

Verified against the tooling in this repo:

```powershell
py -3 tools/mc2mod/mc2mod.py pack docs/modding/examples/imgui-editing/mod --out <scratch-dir>
# -> pack: <scratch-dir>\imgui-tuning-demo-0.0.0.mc2mod
#      id=imgui-tuning-demo  version=0.0.0  files=3  sha256=83936b6397dea2f394d869369d03f8fd7863ff9d4dd5fbaefcf5dc67fb4a81c2

py -3 tools/mc2mod/mc2mod.py verify-lite <scratch-dir>\imgui-tuning-demo-0.0.0.mc2mod
# -> verify-lite PASS: id=imgui-tuning-demo  version=0.0.0  files=3  pkg_sha256=83936b6397dea2f394d869369d03f8fd7863ff9d4dd5fbaefcf5dc67fb4a81c2
```

(`tools/mod_install/resolver.py` is a library consumed by other tooling, not a standalone CLI — there's no `--help` output and nothing to run it against for a single-mod, no-dependency case like this one; `verify-lite` above already proves the package's structural integrity.)

To actually install and try it: `py -3 tools/mc2mod/mc2mod.py install <scratch-dir>\imgui-tuning-demo-0.0.0.mc2mod --deploy <your-own-copy-of-the-game-folder>` (not one of this project's canonical dev deploy roots — see `getting-started.md` §5). **Then, instead of the launcher or a raw `mc2.exe` launch, run the script the install just placed at `<your-deploy>\mods\imgui-tuning-demo\launch-with-tuning.ps1`** — see "Running the launch script" below. It sets both `MC2_ACTIVE_MOD` and `MC2_VISUAL_TUNING_FILE` and then launches `mc2.exe` for you.

#### Running the launch script

Windows' default action for `.ps1` files is **Edit**, not **Run** — double-clicking `launch-with-tuning.ps1` in Explorer will most likely just open it in a text editor instead of launching the game. Use one of these instead:

- **Right-click the file → "Run with PowerShell"**, or
- Run it explicitly from a PowerShell prompt: `powershell -ExecutionPolicy Bypass -File .\launch-with-tuning.ps1` (the `-ExecutionPolicy Bypass` fallback is only needed if your machine's default execution policy blocks unsigned local scripts; drop it if the script already runs fine without it).
Picking "ImGui Tuning Demo (mc2_01)" in the launcher, or setting `$env:MC2_ACTIVE_MOD` yourself and launching `mc2.exe` directly, is **not sufficient** for this particular mod — both activate the mod overlay correctly, but neither sets `MC2_VISUAL_TUNING_FILE`, so the tuning file is never read (see the Tier-A blocker box in Part 1.3). Load `mc2_01` and compare shadow softness / exposure / terrain-lighting floor against Base Game — should read as slightly brighter and softer-shadowed. If you don't see a difference, re-check that you launched via `launch-with-tuning.ps1` and not the launcher/raw exe.
