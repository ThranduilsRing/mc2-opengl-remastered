# ⚠ WORKTREE GUARD — read FIRST if you are a subagent working the Vulkan arc

**You are (or should be) working in:** `A:/Games/mc2-vk-interop-bridge`, branch
`claude/vk-interop-bridge-1`. This is the CANONICAL worktree for the Vulkan backend migration arc.
All arc commits (P1..P13b, 14+) live here.

## The mc2-repo-intel MCP is bound to a DIFFERENT worktree — DO NOT trust its preflight here
`mcp__mc2-repo-intel__preflight` / `gate_status` / `deploy_status` are pinned to the
`nifty-mendeleev` worktree (a different tree with ~245 dirty files). So in THIS worktree they
return `branch_ok=false` / `root_ok=false` / wrong dirty-state — a **KNOWN BENIGN ARTIFACT**, NOT a
real guard failure. Do NOT hard-stop on it, and do NOT let it make you think you're on the wrong tree.

## Verify tree state DIRECTLY with git (authoritative), not the MCP:
```
git -C A:/Games/mc2-vk-interop-bridge rev-parse --abbrev-ref HEAD   # → claude/vk-interop-bridge-1
git -C A:/Games/mc2-vk-interop-bridge log --oneline -1               # arc HEAD
git -C A:/Games/mc2-vk-interop-bridge status --short                 # your + prior dirty files
```
Use absolute paths / `git -C <this worktree>` for ALL git ops. NEVER touch `nifty-mendeleev`.
NEVER `git add -A` (shared-index / stray-file risk) — stage explicit paths only.

## Build / deploy for this arc
- Build variant: `build64_island` (`-DMC2_VULKAN=ON -DMC2_VULKAN_ISLAND=ON`), RelWithDebInfo, target mc2.
- Deploy: `deploy_payload.py --build-dir build64_island/RelWithDebInfo --build64-root build64_island` → `mc2-win64-v0.4c`.
- Launcher bypass for direct runs: `MC2_NO_LAUNCHER=1` (+ `MC2_RENDER_BACKEND=vulkan` to exercise the VK backend).
- Long game/parity runs: FOREGROUND (blocking) — background runs have died mid-run and lost their verdict.
