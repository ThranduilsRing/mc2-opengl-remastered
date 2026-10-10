<!-- doc-status: current | reviewed:2026-10-11 -->
# Glossary

Short definitions of terms used across MC2 docs and commit messages.

- **nifty** — `claude/nifty-mendeleev`, the canonical dev branch.
- **lane** — a leased deploy folder under `A:/Games/mc2-opengl/` used by one session at a time.
- **slice** — a small gated unit of work shipped with a proof (parity/smoke/screenshot evidence).
- **tier1** — the 5-mission smoke set: `mc2_01`, `mc2_03`, `mc2_10`, `mc2_17`, `mc2_24`.
- **gate** — an `MC2_*` env var feature switch controlling a slice's behavior.
- **kill-switch** — a default-**ON** gate; setting it to `0` disables the feature.
- **compbas** — `objects/compbas.csv`, the equipment definition table.
- **FST** — packed archive format used to bundle loose data files (e.g. `mission.fst`, `tgl.fst`).
- **FIT** — text key-value data format used for mission/unit/config data.
- **.pak** — a mission package (terrain + object data for one mission).
- **TGL** — the legacy shape/geometry system (`.tgl`/`.ase` meshes, `tgl.cpp`/`tgl.h`).
- **bank** — to merge verified work into `nifty`.
- **chip** — a small queued follow-up task spun off from the current session.
- **greybeard** — an adversarial senior-review pass on a proposed change.
- **ENV-DROP** — an `MC2_*` env var not present in the smoke-harness allowlist, silently dropped at launch.

See also [AGENTS.md](../AGENTS.md) for agent-facing hard rules.
