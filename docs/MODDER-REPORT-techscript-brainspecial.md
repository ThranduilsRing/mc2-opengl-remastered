# MC2R engine support for your TechScript / mission_specials system

Hey — wanted to give you a status update, because your **Carver V Enhanced** campaign and its `mission_specials.fit` / TechScript design turned out to be the perfect reference for a feature we've been building into the MC2 Remastered engine. Short version: **the engine can now load, parse, and execute your TechScript specials directly, and we adopted your file format as the native format** rather than inventing our own. Thank you — it saved us from guessing what a good shape looks like, because you'd already built one that works.

This is early and everything is behind off-by-default switches, so nothing here changes stock missions or existing campaigns. But the foundation is in and proven.

## What the engine does now

We added a native "BrainSpecial" runtime that hooks into each unit's brain update. Step by step:

1. On mission load it looks for `data/missions/<mission>_ai.fit` (per-unit brain settings) and `data/missions/<mission>_specials.fit` (your specials).
2. It parses the `TechSpecial { ... Body { DO <verb> ... } }` blocks into an internal token list — same structure you authored.
3. Each brain tick it runs that unit's special body, recognizes the DO-verbs, and (when enabled) executes them as real engine actions.

Today it recognizes your core verb families (`Brain.*`, `OPORD.*`, `Unit.*`, etc.) and **the first real effect is live**: `Brain.CorePower false` makes the unit power down through the engine's actual order system. Everything else is currently recognized-and-logged while we wire effects up one verb at a time — deliberately, so each addition is provable and can't destabilize a mission.

We're building it as a ladder: recognize verbs → execute one safe verb → execute the rest. Movement/attack/OPORD orders come next, each one isolated.

## A couple of friendly heads-ups about the content

These aren't criticisms — your files run, and they're the reason we could build this at all. But while parsing them we noticed two things the engine now has to account for, and you might want to know about them for future authoring:

**1. State machines run "flat."** Your unit specials list every FSM state's body one after another (the `Unit.InState "X"` branches), but there's no enforced transition gate — so without help, *every* state's body executes *every* tick, and `trans` / `transBack` get dropped. Your ABL originals had real state transitions; the generated specials flattened them. We're adding a per-unit "active state" tracker in the engine so a unit only runs its current state's body — but if you hand-author specials, keeping one active-state branch in mind will match how the engine will execute them.

**2. `Var.Set scope=Mission` is shared across all units.** Mission-scope variables are global to the whole mission, so if two units write the same variable on the same tick, the order isn't well-defined (and that matters for multiplayer determinism). For per-unit values, the engine is going to namespace them per unit automatically. If you intend a variable to be unit-private (like `numMates` for one lance), it's safest to treat it as unit-scoped rather than a shared mission global.

## Naming note

Internally the engine calls this layer **"BrainSpecial"** — only because the editor already has an unrelated feature called "TechScript" (the mission-trigger system), and we didn't want two things with the same name in the codebase. Your `Runtime = "TechScript"` files work as-is; it's purely an internal label.

## Bottom line

Your TechScript/specials approach is now a first-class engine path, not a mod hack — loaded, parsed, and starting to execute natively, on a hardened and deterministic foundation. We'd love to keep using Carver V Enhanced as the proving ground as more verbs come online. If you have notes on the verb vocabulary, the FSM-state model, or how you'd *want* the engine to treat mission vs unit variables, that input would directly shape what we build next.

## How to enable

This runtime is off by default. To turn it on, set this gate chain (each requires the previous one):

1. `MC2_BRAIN_RUNTIME=1` — master gate for the declarative-brain runtime (`code/warrior.cpp`).
2. `MC2_BRAIN_RUNTIME_APPLY=1` — lets resolved intents actually apply (requires `MC2_BRAIN_RUNTIME=1`).
3. `MC2_BRAIN_DISPATCH=1` — TechScript special-dispatch master gate (`code/mission.cpp`, `code/brain_special_dispatch.cpp`; requires the two gates above).

File naming: per-mission brain/specials data lives at `data/missions/<mission>_ai.fit` (per-unit brain settings) and `data/missions/<mission>_specials.fit` (your TechScript specials).

Example fixtures: see `tests/fixtures/brain_runtime/`.

See [docs/tier1_env_vars.md](tier1_env_vars.md) (Mech import, TechBrain, and misc arc gates section) for the authoritative gate reference.
