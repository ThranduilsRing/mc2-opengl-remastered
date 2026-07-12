# VFX First Visual Improvement — Plan (VFX-VISUAL-PLAN-0)

Decides the **first real VFX visual improvement** for the GPU particle lane.
Plan only — nothing implemented here. Builds on `docs/vfx-rv-arc-recon.md`
(authority/shaders/risks), `docs/vfx-overdraw-audit.md` (cost/coverage), and
the shipped Batch-2 substrate (`MC2_VFX_DEBUG_MODE`, the "VFX Tuning" controls).

Worktree `track-rv-VFX` @ `32698e43`. Lane deploy: `mc2-win64-water`.

---

## 1. The candidates

| # | Option | Verdict |
|---|---|---|
| **A** | **GPU-side age advance / curve eval** | **RECOMMENDED — first visual** |
| B | Soft particles / depth fade | Strong second; needs depth sample (more risk) |
| C | Alpha / additive tuning | Already shipped as *controls* (Batch 2); a *default* change is forbidden |
| D | Fog coherence | Deferred — no fog term in particle shader; low payoff |
| E | Trail cleanup (atlas UV) | Deferred — narrow (PpcBolt/MissileSmoke), not the headline defect |
| F | Texture / cook rewrite | Explicitly out of scope |

---

## 2. Answering the required questions

**Is age=0.5 the biggest blocker?** **Yes.** Every GPU particle samples its
spec curves at a fixed `age = 0.5` (`mclib/particles/spawn_card.cpp:71`, and
the sibling `spawn_*.cpp`). Consequence: particles **do not animate** — no
fade-in, no fade-out, no grow/shrink. An explosion is a static mid-life
snapshot popped on and off. This is the single most visible quality deficit
(recon §2). Fixing it is the highest-value *look* lever (additive intensity is
the highest-value *cost/tuning* lever — different axis).

**Is dual-draw / overdraw a blocker?** **No.** The overdraw audit closed both:
no dual-draw (routed leaves suppress legacy MLR), and overdraw is bounded +
combat-scoped (worst mc2_24 ~193 sprites/frame, 8 additive groups). Animating
particles does not increase particle *count* (same emit, time-varying
color/size), so it does not worsen overdraw. Cost is orthogonal to this slice.

**What per-particle age/lifetime data exists?** The `GpuParticle` struct
(`spec.h:42`) already carries `lifetime` (offset 48) and `age` (offset 52)
fields. `lifetime` is sampled at spawn; `age` is currently written `0` and the
**shader never reads either** (`particle_billboard.frag` does `tex*v_color`
only). More importantly, the **gosFX effect's own normalized age `m_age`
(0..1) is CPU-advanced by gameplay every frame** (`effect.cpp:625,741`) and is
in scope at each producer's `Draw` call site (e.g. `card.cpp:506` calls
`Spawn(GetSpecification(), &m_localToWorld, (float)m_seed)` — `this->m_age` is
available there).

**Can the GPU bridge evaluate curves without gameplay timing changes?**
**Yes — and it doesn't even need GPU-side curve eval.** The cleanest approach
keeps curve evaluation on the CPU (where `FCurve::ComputeValue` already lives)
but **samples at the effect's real `m_age` instead of the fixed `0.5`**:

- GPU particles are **re-emitted every frame** from live gosFX state (the
  batcher has zero persistence — recon §1). So sampling at the effect's
  *current* `m_age` each frame produces a correctly time-varying color/alpha/
  size **for free**, with no GPU persistence, no spawn-timestamp bookkeeping,
  and no GPU curve upload.
- `m_age` is **already advanced by gameplay** — we only *read* it for the
  visual sample. **No emission/lifetime/timing is touched** (hard constraint
  satisfied). This is a pure render-side read of an existing value.
- The original "age=0 bakes invisible particles" trap (the reason 0.5 was
  hardcoded) **becomes correct behaviour**: an effect genuinely early in its
  life *should* be faint/small (fade-in); one near end *should* fade out. The
  0.5 hack only existed because a single static sample can't represent a
  lifecycle — per-frame real-age sampling removes that constraint.

A later, larger option (true GPU-side curve eval from an uploaded LUT + a
spawn-timestamp + a `u_time` uniform) exists but is **not** needed for the
first visual and carries the persistence/ABI cost. Defer it.

---

## 3. Recommendation: VFX-AGE-SAMPLE-1 (option A, minimal form)

**Sample gosFX spec curves at the effect's real `m_age`, gated, default-OFF
(default keeps the byte-identical `0.5` snapshot).**

Shape of the future slice (NOT implemented here):

1. Add an `age` parameter to `mc2::particles::Spawn(...)` and the five
   `Spawn*` producers (`spawn_card/cardcloud/point/shard/tube.cpp`); thread
   `this->m_age` from each producer's `Draw` (card.cpp:506 etc.).
2. In each `Spawn*`, replace `const Stuff::Scalar age = 0.5f;` with:
   `age = gateOn ? clamp(callerAge, 0, 1) : 0.5f;`. Gate read once from a new
   env (default OFF → `0.5` → **byte-identical**).
3. No shader change required (curves stay CPU-sampled; only the sample point
   moves). No `GpuParticle` ABI change. No reflect-golden drift.
4. Optional follow-up (separate slice): also write the real `age`/`lifetime`
   into `GpuParticle` and let the shader do a cheap fade — only if CPU-sample
   animation proves insufficient.

**Why this and not B (soft particles):** soft particles need the FS to sample
the scene **depth texture** (a new bound resource + a depth-linearization +
reverse-Z care) — more surface, more risk, and it solves *intersection
hardness*, not the *no-animation* defect users see first. Do A first; B is the
natural second visual slice once depth sampling is wired.

**Why not C as a visual:** the Batch-2 tuning controls already expose
brightness/additive/alpha, but their **defaults must stay 1.0** (no-default-flip
constraint). Shipping a non-1.0 default is a separate, explicitly-gated
decision, not a "first visual."

---

## 4. Gate / env

- New gate: **`MC2_VFX_AGE_SAMPLE`** (default **OFF** = `0.5` snapshot,
  byte-identical). `=1` → sample at real `m_age`.
- Register in `RendererFeatureRegistry.h` + `docs/tier1_env_vars.md`.
- Optional runtime toggle in the "VFX Tuning" Graphics Options section
  (mirrors the debug-mode combo), default following the env.

---

## 5. Captures that prove it

- Preset `vfx_combat_10` (mc2_10). **Use the intro ~15–40 s particle window**
  (laser/missile/MG/SRM are active there; `warmup_s=28` lands in-window — see
  memory `vfx_mc2_10_intro_particle_window`).
- Before/after pairs at the same warmup: gate OFF (0.5 snapshot) vs gate ON
  (real-age). Expect visible fade-in/out + grow on explosions/flares; default
  (OFF) byte-identical to current.
- **Transient-VFX rule:** confirm particle presence via the capture `.log`
  (`GOSFX_GPU enabled=1 sprites=N` / `TRAIL_PROBE`), not the PNG alone.
- Capture a short sequence (multiple warmups across 15–40 s) to show the
  *animation*, since a single frame can't demonstrate a lifecycle.
- Gates: tier1 5/5 default (byte-identical, Δdestroys +0); mc2_10 + gate ON
  runs clean with particles active; `check-vfx-no-objectid` PASS (no shader
  change, but assert the invariant); no `--kill-existing` (concurrent OK).

---

## 6. Out of scope (for the first visual)

- GPU persistence / spawn-timestamp / `u_time` curve eval (larger option A).
- Soft-particle depth fade (option B — separate slice once depth sampling
  exists).
- Any **default** intensity/alpha/additive change (no-default-flip constraint).
- Fog coherence (D), trail atlas cleanup (E), texture/cook rewrite (F).
- Routing the 5 unrouted CPU-only classes (PertCloud/ShapeCloud/Shape/
  DebrisCloud/PointLight) to the GPU bridge — a coverage slice, not a visual.
- Postprocess/bloom/tonemap changes.
- Any gameplay/emission/lifetime/weapon/timing change.

---

## 7. Readiness verdict

VFX is **ready for its first visual implementation.** The substrate is
complete (debug views, baseline presets, overdraw/coverage audit, tuning
controls), the no-animation defect is precisely located (fixed `0.5` sample),
and a **safe, gated, default-byte-identical** fix path exists that reads an
already-gameplay-advanced value without touching timing, persistence, ABI, or
shaders. Recommended first slice: **VFX-AGE-SAMPLE-1** (needs its own approval;
not authorized by this plan).
