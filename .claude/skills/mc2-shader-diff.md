---
name: mc2-shader-diff
description: Show full diff between worktree shaders and deployed shaders. Flags mismatches, missing files, and stale deployed files.
---

# MC2 Shader Diff

Show the actual content diff (not just filenames) between the worktree and the deployed runtime directory.

## Paths
- **Source**: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/`
- **Deployed**: `A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/`

## Steps

1. **Compare all shader files** individually using `diff`:
```bash
for f in A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/*.{frag,vert,tesc,tese,geom}; do
  name=$(basename "$f")
  deployed="A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/$name"
  if [ ! -f "$deployed" ]; then
    echo "MISSING IN DEPLOY: $name"
  elif ! diff -q "$f" "$deployed" > /dev/null 2>&1; then
    echo "=== DIFFERS: $name ==="
    diff "$f" "$deployed"
  fi
done
```

2. **Compare include/ directory**:
```bash
for f in A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/include/*; do
  name=$(basename "$f")
  deployed="A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/include/$name"
  if [ ! -f "$deployed" ]; then
    echo "MISSING IN DEPLOY (include): $name"
  elif ! diff -q "$f" "$deployed" > /dev/null 2>&1; then
    echo "=== DIFFERS (include): $name ==="
    diff "$f" "$deployed"
  fi
done
```

3. **Check for stale files** in the deploy dir not present in source:
```bash
for f in A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/*.{frag,vert,tesc,tese,geom}; do
  [ -f "$f" ] || continue
  name=$(basename "$f")
  src="A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/$name"
  [ ! -f "$src" ] && echo "STALE IN DEPLOY (not in source): $name"
done
```

4. **Report summary**: Count mismatches, missing, and stale. If all files match, print "All shaders in sync."
