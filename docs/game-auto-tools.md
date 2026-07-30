# Game Auto Tools

`scripts/game_auto.py` is the lightweight MC2 UI automation helper.

> **USER-PRESENT ONLY (NOFOCUS-FINISH-1).** This tool drives the game with
> synthetic input aimed at SCREEN coordinates and screenshots the DESKTOP
> COMPOSITE, so it needs the window on the visible desktop — which every agent
> launch now refuses. `game_auto.py launch` declares `offscreen=False` and
> `nofocus.apply_env` raises `DesktopOccupancyRefused` unless the session sets
> `MC2_HARNESS_ALLOW_FOREGROUND=1`. That is not a bug to work around: driving
> the game by synthetic mouse and keyboard IS taking over the user's desktop,
> and the reason engine launches were hard-blocked in the first place. Run it
> only in a session where a human is present and expecting it.

## Commands

Run from anywhere:

```powershell
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\game_auto.py launch
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\game_auto.py kill
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\game_auto.py script A:\path\to\menu_canary.txt
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\game_auto.py record A:\path\to\menu_canary.txt
```

## Recorder

`record <file>` writes a script in the same line-oriented format that `script <file>` replays.

Behavior:

- Starts after a 3 second countdown.
- Press `F9` to stop and save.
- Left click records `click X Y`.
- Right click records `rclick X Y`.
- Common keys record as `key <name>`.
- Time between actions is recorded as `wait <seconds>`.

That makes the recorded file directly reusable as a menu canary script.

## Recommended workflow

1. Start recording to a new file.
2. Boot MC2 and click through main menu and logistics into the first mission.
3. Press `F9` once the mission is loading or at spawn.
4. Review the script and trim any noisy extra clicks or key presses.
5. Replay it with `script <file>`.

## Notes

- Coordinates are absolute screen coordinates, so replay assumes the same monitor/layout/resolution class used during recording.
- Keep menu canary scripts separate from the direct-start smoke matrix. They are checking different failure modes.
