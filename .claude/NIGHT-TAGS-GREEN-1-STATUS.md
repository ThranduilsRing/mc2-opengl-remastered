# NIGHT-TAGS-GREEN-1 — STATUS

**Verdict: brief's central premise REFUTED. Scoped "Hot-Green-only" mode NOT implemented,
because Hot Green is the *widest* of the three night tags, not the narrowest.
`MC2_STATIC_PROP_NIGHT_TAGS` stays default-OFF. RECOMMEND AGAINST flipping.**

Delivered instead: the in-scope hygiene item — `MC2_LIGHT_CONE_NODES` registered + documented.

---

## 1. The premise I was sent to implement

> ★ THE KEY FINDING: the branches are separable. Runway/beacon fixtures use ONLY
> Hot Green `0xff00ff00`; building windows use Hot Pink `0xffff00ff`.
>
> MISSION: implement a scoped Hot-Green-only mode — relight runway/taxiway/beacon
> fixtures without touching building windows. Near-zero blast radius.

The premise is half true, and the half that is false is the load-bearing half.

- TRUE: runway/beacon fixtures do carry Hot Green, and building **windows** do carry Hot Pink.
- FALSE: the converse. Hot Green is **not** exclusive to runway/beacon fixtures.

The inference "green ⇒ fixture" does not follow from "fixture ⇒ green". Hot Green is
shared, and it is shared with buildings specifically.

## 2. Evidence (source data, `A:/Games/mc2-srcdata/tgl/`, 2947 `.ase` meshes)

Tag encoding in ASE is `*MESH_VERTCOL <n> R G B`; Hot Green = `0.0000 1.0000 0.0000`.

| Tag | Meshes carrying it |
|---|---|
| Hot Green `0xff00ff00` | **738** |
| Hot Pink `0xffff00ff` | 214 |
| Hot Yellow `0xffffff00` | 155 |

Of the **738** Hot-Green meshes, only **8** are beacon/runway fixtures. The other **730**
are buildings. Sampled names: `s_barracksdamx`, `l_palacewingdam`, `s_govresidence`,
`meethall`, `city6storyx`, `hpg_corner`, `guntowerenc`, `solarcollector`,
`wasteprocessorenc`, `dropshipliaox`, `d_turretcontrol`, `civstorageadaml1`.

This is exactly what the shader itself says. `shaders/include/lighting.hglsl:118`:

    else if (start_v_light == 0xff00ff00)   //Hot Green -- Building Base Lights -- ONLY at NIGHT

The tag's stock name is **"Building Base Lights"**. The runway/beacon fixtures are
*borrowing* the building-base-light tag for their posts; they are not its primary users.

### The per-shape uniform does not rescue it

Last possible escape: if buildings never declared a `HotGreenRGB`, green would be a
no-op for them and the brief would survive. They do declare it, with substantive
non-black values. Across `data/tgl/*.ini`, **274** declare `HotGreenRGB`:

| Value | .ini count | What it is |
|---|---|---|
| `0x00707070` | 149 | grey building base glow |
| `0x00557238` | 70 | olive building base glow |
| `0x00787832` | 30 | olive building base glow |
| `0x006AA6A6` | 16 | teal building base glow |
| `0x00325064` | 4 | blue building base glow |
| `0x00CCCC66` | 1 | building |
| `0x00ff0000` | 2 | **runwaylight + redbeaconlight** |
| `0x0000ff00` | 1 | **greenbeaconlight** |
| `0x000000ff` | 1 | **bluebeaconlight** |

**270 of 274** `HotGreenRGB` declarations are buildings. Only **4** are the fixtures the
user is complaining about.

## 3. Why this makes green-only the *worst* available scoping, not the best

A Hot-Green-only mode would relight ~270 building types with a grey/olive base glow and
~4 fixture types with their beacon colour. Measured against the two things it was
supposed to achieve:

- **"Near-zero blast radius"** — false. Green-only has a *larger* building blast radius
  than green+pink+yellow has fixtures. It is 730 buildings to buy 8 fixtures.
- **"Without touching building windows"** — technically true and irrelevant. It spares
  the windows (pink) and lights the building *bases* instead. The user's separate
  complaint is that buildings look over-lit relative to props; green-only pushes
  directly into that complaint rather than away from it.

This independently reconfirms `.claude/PROP-NIGHT-LIGHT-3-STATUS.md` §5
("RECOMMEND AGAINST flipping ... it would **widen** the very building-vs-prop contrast
the user is complaining about"). That lane reached the right conclusion by reading the
shader comment; this lane confirms it by counting the actual authored assets.

## 4. Recommendation on default-ON

**Do not flip `MC2_STATIC_PROP_NIGHT_TAGS` to any nonzero default — not mode 1, not
mode 2, and not a green-only mode 3.** Reasons, in order of weight:

1. **Wrong scoping axis.** Colour does not separate fixtures from buildings; the data
   above shows colour is shared. The only axis that separates them is **fixture
   identity** (§5).
2. **No headroom, unchanged.** The grading lane's mode-2 (4×) result of +17.27 vs
   mode-1's +17.14 still stands: the emission saturates against a night ambient of
   ~196/255. Whatever is shipped, it cannot be tuned brighter, so it has no margin
   for the "lights are not ON at all" complaint it is meant to answer.
3. **It would worsen a live user complaint** (over-lit buildings) to fix a different one.
4. **Overview-altitude regression risk is unretired.** The `RUNWAY-MARKER-LIGHTS-1`
   "atrociously hideous streaks" regression escaped because verification was all at
   airbase range. No overview-altitude evidence exists for this change either. I did
   not manufacture any, because measuring an effect I had already shown to be
   mis-scoped would have been wasted cycles (see §6 on the capture noise floor).

## 5. The correct design for a successor lane

Scope by **fixture identity, not vertex colour**. The fixture set is small, closed, and
enumerable — `data/tgl/{runway,red,green,blue}beaconlight*.ase` plus `runwaypoint`,
~24 files including `dam`/`x`/`l1` variants, backed by exactly 4 `.ini` files whose
`HotGreenRGB` is the beacon emission colour rather than a building glow.

Sketch: mark those shapes at load time (the 4 `.ini` are already distinguishable — they
are the only ones whose three `Hot*RGB` values are saturated primaries rather than
desaturated greys/olives), carry a per-instance flag into the static-prop instance SSBO,
and let the night-tag branch fire only for flagged instances. That is a genuine
near-zero-blast-radius fix and it is what the user's defect actually needs.

It is a real slice (instance-SSBO plumbing + shader branch + capture campaign), not an
edit, which is why this lane stopped and reported rather than improvising it under a
brief written for a different design.

## 6. Note for the next lane: the capture noise floor is still unsolved

I did not add to the noise-floor work, because I refuted the change before it was worth
measuring. Flagging so it is not mistaken for a solved problem: mc2_10 captures remain
nondeterministic across launches (a same-environment control differed by up to **24 %**
of pixels). Any successor measuring the fixture-scoped fix should prefer in-process A/B
or numeric per-fixture diag receipts over whole-frame pixel deltas between launches.

## 7. What this lane actually changed

`MC2_LIGHT_CONE_NODES` — default-ON, single reader `mclib/cone_glow.cpp:118`
(`return !(v && v[0]=='0')`), consumed at `mclib/bdactor.cpp:3398`. It was
smoke-allowlisted but **unregistered and undocumented**: a live default-behaviour gate
sitting outside the registry. That gap is not cosmetic — it is the mechanism by which
`CONE-SPILL-COUPLING-1` (`c1f98b45`) rode a default-ON gate into the shipped render
ungated and unreviewed.

- `RenderCore/RendererFeatureRegistry.h` — registry entry added, `default=true`.
- `docs/tier1_env_vars.md` — `MC2_LIGHT_CONE_NODES=0` documented.

No renderer behaviour changed. No shader, no batcher, no `.ase`/`.ini` data touched.

## 8. Adversarial self-review

- *"Would the user still see the defect on a default launch?"* **Yes.** Runway and
  beacon lights remain unlit at night. This lane did not fix the user's defect and does
  not claim to. It establishes that the prescribed fix would not have fixed it either,
  and would have made a second complaint worse.
- *"Did I refute my own refutation?"* Attempted twice. (a) Maybe green-tagged buildings
  are rare/unused — no: 730 meshes spanning barracks, palaces, city blocks, HPG, turrets.
  (b) Maybe buildings declare no `HotGreenRGB`, making green a no-op for them — no:
  270 building `.ini` declare it with non-black grey/olive/teal values.
- *"Is the gate work attributable?"* The registry entry is compiled into the exe; the
  doc is text. Both verified present at commit. No behaviour change, so tier1 is a
  regression check, not evidence of a feature.
- *"Am I hiding an inconvenient result?"* No. The brief asked for an implementation and
  a default-ON recommendation. It gets a refusal to implement, with counts, plus an
  explicit RECOMMEND AGAINST — the opposite of what the brief anticipated.
