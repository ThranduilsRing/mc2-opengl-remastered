# Contributing to MC2 OpenGL Remastered

Thanks for helping modernize MechCommander 2. This page covers the mechanics of
getting a change accepted: branch flow, the mandatory build configuration, the
smoke gate every change must pass, the coding-rule documents, and licensing.

## Branch / PR flow

- Fork the repo (or branch, if you have push access) and work on a topic branch
  named for the change, e.g. `fix/shadow-banding` or `feature/hud-scale`.
- Keep commits small and logical; one concern per commit. Reference GitHub
  issues in the commit body (`Fixes #42`) where applicable.
- Open a pull request against the default branch. Describe *what changed and
  how you verified it* — a PR that states its smoke/parity evidence reviews far
  faster than one that doesn't.
- Default-OFF gating: new renderer/gameplay features should ship behind an
  `MC2_*` env gate, default OFF, so stock behavior stays byte-identical. See
  the "Adding a new MC2_* gate" checklist at the top of
  [docs/tier1_env_vars.md](docs/tier1_env_vars.md).

## Build (mandatory configuration)

Requires Visual Studio 2022 Build Tools (MSVC v143). Extract the vendored
`3rdparty.zip` at the repo root first, then:

```bash
cmake -G "Visual Studio 17 2022" -DCMAKE_PREFIX_PATH=C:/absolute/path/to/repo/3rdparty -DCMAKE_LIBRARY_ARCHITECTURE=x64 -DMC2_IMGUI=ON -B build64
cmake --build build64 --config RelWithDebInfo --target mc2
```

**Always build `RelWithDebInfo`.** Release builds crash due to GL debug
callback registration; Debug is too slow to smoke. Output lands at
`build64/RelWithDebInfo/mc2.exe`. Full details: [BUILD-WIN.md](BUILD-WIN.md).

## Smoke gate (must pass before a PR)

Every change must pass the tier-1 smoke:

```bash
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs
```

This boots a mission headlessly, watches for crashes / GL errors / shader
failures, and keeps logs for triage. If your change adds an env gate, prove
both gate-ON and gate-OFF runs (see the gate checklist in
[docs/tier1_env_vars.md](docs/tier1_env_vars.md)).

## Coding rules (read before touching engine code)

- [docs/cxx17-coding-rules.md](docs/cxx17-coding-rules.md) — the C++17 subset
  and style rules this codebase holds to.
- [docs/critical_inline_rules.md](docs/critical_inline_rules.md) — hard rules
  whose violation causes real, known crashes/corruption (read this one first).
- [docs/disciplines.md](docs/disciplines.md) — process disciplines (evidence
  requirements, slice preflight, verification-before-completion).

## Env-gate checklist

Any new `MC2_*` environment variable must be registered, allowlisted for the
smoke harness, and documented — the full checklist lives at the top of
[docs/tier1_env_vars.md](docs/tier1_env_vars.md). Unregistered gate reads are
CI-blocking (check-env-registry.sh (retired in G8; the gate table is the registry)).

## Licensing

Per [LICENSES.md](LICENSES.md): the tree mixes Microsoft Shared Source
Limited Permissive code (original `code/`, `mclib/`, `gui/`, `GameOS/`),
GPLv3 port scaffolding from alariq/mc2, and MIT remaster code.

**New contributions are MIT** unless they modify a file from the Microsoft
Shared Source or GPL buckets, in which case the contribution inherits that
file's license. By submitting a PR you agree to license your contribution
accordingly.
