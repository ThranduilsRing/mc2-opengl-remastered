# Local Qwen Workflow

Shared local-Qwen entrypoint for this worktree:

```powershell
py -3 scripts/qwen_local.py
```

It targets LM Studio's OpenAI-compatible API at `http://127.0.0.1:1234/v1` by default.

Environment overrides:

```powershell
$env:MC2_QWEN_BASE_URL = "http://127.0.0.1:1234/v1"
$env:MC2_QWEN_MODEL = "qwen3.6-35b-a3b-mtp"
```

## Commands

List models:

```powershell
py -3 scripts/qwen_local.py models
```

Ping the active model:

```powershell
py -3 scripts/qwen_local.py ping
```

Recon pass:

```powershell
py -3 scripts/qwen_local.py recon `
  --task "Trace terrain shadow draw flow and identify likely edit sites" `
  --file GameOS/gameos/gameos_graphics.cpp `
  --file mclib/txmmgr.cpp `
  --file code/gamecam.cpp
```

Edit pass:

```powershell
py -3 scripts/qwen_local.py edit `
  --task "Draft a minimal unified diff to rename a local helper without changing behavior" `
  --file path/to/file.cpp
```

## Intended use

- `recon`: read-only mapping, symbol tracing, flow summaries, likely edit sites, risks.
- `edit`: draft patch generation for a narrowly-scoped slice after recon is reviewed.

## Constraints

- The helper pins context to the canonical worktree path and rejects file paths outside it.
- `edit` is draft-patch generation only. Build, smoke, deploy, and final acceptance stay outside Qwen.
- Keep file sets small. The default cap is 8 files, 48 KB per file.
- Completion length is capped by default (`--max-tokens 500`) to keep local runs from wandering into long reasoning output.
- For load-bearing renderer work, use Qwen for recon and draft diffs, then verify locally with the normal build and smoke gates.

If a recon task genuinely needs more room, raise the cap explicitly:

```powershell
py -3 scripts/qwen_local.py recon `
  --task "Trace this subsystem and list risks" `
  --file some/file.cpp `
  --max-tokens 900
```

## Notes for Codex and Claude Code

- Both agents can invoke the same command from the worktree root with `py -3 scripts/qwen_local.py ...`.
- If you want raw output saved for a later step, add `--output docs/recon/<name>.json` or another repo path.
