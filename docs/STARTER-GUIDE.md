<!-- doc-status: current | reviewed:2026-10-11 -->
# MechCommander 2 (OpenGL remaster) — Starter Guide

A modernized, OpenGL build of MechCommander 2 that plays the original campaign plus a
large library of community campaigns (MC2X / MCO). This guide takes you from download to
playing, and is honest about what works and what isn't fully verified yet.

---

## 1. Get the build

- Obtain the release build folder (e.g. `mc2-win64-v0.4d-rc1/`) from the project's release
  location. It contains `mc2.exe`, `data/`, `mods/`, and `shaders/`.
- Windows 64-bit. A GPU supporting **OpenGL 4.3+** is required (most cards since ~2013).
- Unzip somewhere with a real path (avoid OneDrive/temp). The folder is self-contained.

> You only need the base build to play **stock** MC2. Community campaigns are added with the
> portable **Import CLI** (section 4).

---

## 2. Launch

- Run **`mc2.exe`**. Open **Campaigns & mods** on the main menu, select a campaign,
  then **Start campaign**. Changing content restarts and resumes the game.
- Open **Graphics** for player settings, engine/display options and cheats.

---

## 3. Settings (before you play)

- **Graphics…** — Display resolution, **Enable VSync**, **Limit frame rate** / Frame Rate
  cap, and **Crisp near shadows**. Pick your monitor resolution; cap the frame rate if you
  want quieter fans.
- **Engine Options…** — runtime feature toggles (these set `MC2_*` environment gates).
  Defaults are safe; leave them unless you know what you're changing.
- **Cheats** — optional. `Infinite money` and `Salvage all` for a relaxed/sandbox run.

> Tip: visuals render at 4:3 internally and stretch to fill widescreen — this is a known
> cosmetic limitation (see section 7).

---

## 4. Import a community campaign

1. Have the original campaign installed somewhere (a normal MC2 / MC2X / MCO install — the
   folder that contains `data/` and/or the `.fst` archives).
2. Install Python 3. From the game folder run `py -3 tools/mc2_import/import_tool.py probe "SOURCE"`.
3. Run `py -3 tools/mc2_import/import_tool.py auto "SOURCE" --deploy .`.
   Linux: use `python3` in place of `py -3`. `--dry-run` previews, `--force` reimports,
   Ctrl+C cancels. Read warnings and missing-file counts before playing.
4. Press **Refresh** in the in-game picker and select the new campaign.
   Legacy installer EXEs must first be extracted/installed into a separate source folder.

After importing, you can sanity-check assets without launching:

```
py -3 scripts/verify_campaign_assets.py --all          # uses $MC2_DEPLOY_DIR, or pass --deploy <build folder>
```

This reports any mission missing its colormap (→ black terrain) or any campaign missing
its pilot roster. All-OK means it's ready to play.

---

## 5. Play

1. Pick a campaign in the launcher.
2. (Optional) toggle Cheats.
3. Click **Launch**.
4. In the Mech Bay: buy/assemble your lance, assign pilots, and **Launch Mission**. Support
   vehicles and copters auto-receive a vehicle pilot (they don't consume a roster
   mech-pilot), so they don't block launch.

---

## 6. What to expect

- **19 campaigns, ~398 missions** total (stock + MC2X + MCO). All boot and play.
- Modernized rendering: tessellated PBR terrain, dynamic shadows, post-processing, HDRI
  skies. Original gameplay/missions intact.

---

## 7. What is NOT verified / known limitations

These are cosmetic or edge-case items — campaigns are playable despite them:

- **Pilot-ready (deploy) screen skill/rank icons** can render at the wrong scale on some
  imported campaigns (a fit/atlas-geometry mismatch). The AVAILABLE-PILOTS list icons are
  fixed; the deploy-screen ones are still being addressed. Pilots/skills function correctly
  — only the icon art may look off.
- **Widescreen**: the 3D scene is drawn 4:3 and stretched to fill the screen (slight
  horizontal stretch). HUD is fixed 800×600. A proper widescreen ("hor+") mode is planned.
- **MC2X string tables**: a few imported MC2X campaigns can show `<missing-string>` labels
  for some names/stat text (string-overlay stage in progress). Gameplay unaffected.
- **MechCommanderOmnitech & some MC2X campaigns**: a couple don't auto-advance under the
  test harness but boot and play normally when driven by hand.
- **MC2-Exodus** ships corrupt mission scripts. It needs the opt-in flag
  `MC2_ABL_SKIP_ERRORED_MODULES=1` (skips the broken script) to run cleanly; without it, it
  can crash on certain missions. All other campaigns run with this flag **off** (the
  default) — leaving it off preserves working campaign AI.
- Per-mission cosmetic issues on a few older mod maps (magenta dirt roads,
  transition-seam bleed) noted but not blocking.

---

## 8. Troubleshooting

- **Black terrain on a campaign** → that campaign is missing its colormaps. Re-import it
  (section 4) from its original install; run `verify_campaign_assets.py` to confirm.
- **Generic/wrong mech icons** → re-import the campaign (its icon atlas wasn't carried in).
- **A campaign crashes on mission load** → if it's Exodus (or a campaign with known-corrupt
  scripts), set `MC2_ABL_SKIP_ERRORED_MODULES=1`. Otherwise capture the crash and report.
- **Scripts can't find the build** → set `MC2_DEPLOY_DIR` to your build folder, or pass
  `--deploy <folder>` (the tools are path-agnostic; nothing is hardcoded to one machine).
- **Stale after an update** → close the game fully before re-deploying/importing (a running
  `mc2.exe` can't be overwritten).
