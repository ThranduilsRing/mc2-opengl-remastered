# ABL native-vs-library shadow rule

## TL;DR

Native ABL functions registered via `ABLi_addFunction` go into the
**same global symbol table** as functions defined inside compiled ABL
libraries (`*.abx`) loaded by `ABLi_loadLibrary`. There is no separate
namespace. **A native registration with the same name as a library
function silently shadows the library implementation** — the
preprocessor binds calls to the native callback, the library body
becomes unreachable, and there is no warning at registration time or
at call time.

For a stub registered as a no-op, this means the script appears to
parse and run normally, but every call into that name does nothing.
The brain looks alive but is paralyzed.

## Why this matters

This was the root cause of the v0.2 "passive enemies / inert turrets /
broken HQ convoy" bug. Stock MC2 ships
`data/missions/corebrain.abx`, which defines `magicAttack`,
`coreGuard`, `corePatrol`, and `coreWait` as real ABL routines that
call lower-level baseline natives (`coreattack`, `orderattackobject`,
movement primitives, etc.). All 8 stock warrior brain FSMs
(`Attack.abl`, `Patrol.abl`, `Guard.abl`, `Guard_PowerDown.abl`,
`Guard_Repair.abl`, `Escort.abl`, `Patrol_non_Combat_vehicle.abl`,
`sentry.abl`) call into those four names — `magicAttack` shows up 24
times across the set, as the universal "engage this contact" call.

Commit `db8c00a` (2026-04-22, during the Carver5O / Omnitech buffing
work) added native no-op stubs for those four names so MCO content
that calls them would not crash. The native stubs shadowed the
`corebrain.abx` implementations the moment `initABL()` ran, and stock
AI lost its proactive engagement path. Reactive damage handling lives
deeper in C++ and does not go through ABL, which is why "shoot them
and they fight back" continued to work — and why the regression was
easy to miss.

## Verification mechanism (cheap, repeatable)

The `.abx` library format stores function names as plain ASCII inside
the bytecode. You can audit a library against your native registration
table without running the game:

```python
import re
shadow_candidates = ['magicattack', 'coreguard', 'corepatrol',
                     'corewait', 'magicpatrol', 'magicguard',
                     'magicescort', 'setwillrequesthelp',
                     'tdebugstring', 'isdeadorfled', 'printteamstatus']
for fname in ['data/missions/corebrain.abx',
              'data/missions/orders.abx',
              'data/missions/miscfunc.abx']:
    data = open(fname, 'rb').read()
    toks = set(t.decode().lower()
               for t in re.findall(rb'[A-Za-z_][A-Za-z0-9_]{3,}', data))
    hits = sorted(t for t in shadow_candidates if t in toks)
    print(f'{fname}: {hits}')
```

Run against the **deployed** library (e.g. under
`A:/Games/mc2-opengl/mc2-win64-v0.2/data/missions/`), not the source
tree copy at `mc2srcdata/missions/` — they are different files
(different md5) and only the deployed copy reflects what the engine
actually loads.

For the stock 2003-era retail `corebrain.abx` (md5 `75f9bbdf...`,
42786 bytes), the four shadow-positive names are:

- `magicAttack`
- `coreGuard`
- `corePatrol`
- `coreWait`

`orders.abx` and `miscfunc.abx` (stock retail) define none of the
candidate names.

## The rule

> A native ABL function may only be registered via `ABLi_addFunction`
> if the deployed `.abx` libraries for the active content profile do
> NOT define a function with the same name.

For the stock profile, four names are off-limits. They are now
documented at the registration site in
[`code/ablmc2.cpp`](../../code/ablmc2.cpp) just above the tier-2
extension block and the four corresponding stub bodies are gated out
under `#if 0` so they survive as reference for the mod profile
launcher.

## Consequence for the mod profile launcher

When the launcher selects a content pack:

1. Inspect the deployed `data/missions/corebrain.abx` (and any other
   `.abx` libraries the pack ships) using the audit snippet above.
2. Build the set of names the pack defines.
3. Decide native registrations conditionally:
   - If a name is in the pack's library set → do **not** register the
     native stub. Library wins.
   - If a name is absent → safe to register a native (real
     implementation, or no-op stub if all we need is "don't crash").

For Carver5O / Omnitech specifically: their `.abx` (when integrated)
likely does define `magicAttack`/`coreGuard`/`corePatrol`/`coreWait`
itself, in which case the native stubs are still unnecessary; the
reason they appeared to be needed was that the integration was
running with the *stock* `corebrain.abx` that does include them — but
the Carver5O warrior brains were calling them with extra args the
stock library did not handle, producing parse errors elsewhere in the
chain. The right Carver5O fix is to ship the matching `corebrain.abx`
from the MCO/Omnitech distribution, not to paper over with C++
stubs.

## Symbols intentionally left registered

The other 7 names from `db8c00a` and all 40 from `7c852e2` were
audited against stock `corebrain.abx` / `orders.abx` /
`miscfunc.abx` — zero collisions. They are Omnitech-only extensions
that stock MC2 never references, so they remain safely registered as
either stubs (Omnitech-only behavior) or real implementations
(mission/messaging/economy primitives).

## Related code

- [`mclib/ablrtn.cpp:1071`](../../mclib/ablrtn.cpp#L1071) —
  `ABLi_addFunction` definition (one-line wrapper around
  `enterStandardRoutine`)
- [`mclib/ablsymt.cpp:525`](../../mclib/ablsymt.cpp#L525) —
  `enterStandardRoutine`: writes the name into the global symtable
  via `enterNameLocalSymTable` with `DFN_FUNCTION` and a routine key
  into `FunctionCallbackTable[tableIndex]`
- [`code/mission.cpp:2266-2278`](../../code/mission.cpp#L2266) —
  `ABLi_loadLibrary` calls for `orders.abx`, `miscfunc.abx`,
  `corebrain.abx` at every mission start
- [`code/ablmc2.cpp`](../../code/ablmc2.cpp) — Omnitech tier-2
  registration block with the 4 shadowing entries removed and the
  stub bodies gated under `#if 0`
