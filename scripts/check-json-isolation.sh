#!/bin/sh
# Only these TUs may include nlohmann/json. Fail otherwise.
# (Per-file case match — a space-joined grep -v -F pattern is WRONG: it treats
#  the whole allowlist as one literal and never matches either path.)
#
# CORPUS FLOOR (UNWIRED-CHECK-CLOSE-1, 2026-07-30). This gate passes by
# iterating a hit list and finding nothing un-allowlisted. An EMPTY hit list
# reaches the same "json isolation OK" -- so a renamed vendor header, a moved
# source tree, or simply running from the wrong cwd turns this into a
# permanent silent green that guards nothing. Private registry/profile parser
# TUs and their focused tests are the legitimate population, so a
# floor of 1 is enough to distinguish "clean" from "looked at nothing" while
# staying immune to a legitimate consolidation.
set -e
hits=$(grep -rl 'nlohmann/json' --include='*.cpp' --include='*.h' \
  mclib GameOS RenderCore code tests 2>/dev/null || true)

FLOOR=1
n_hits=$(printf '%s' "$hits" | grep -c . || true)
if [ "$n_hits" -lt "$FLOOR" ]; then
  echo "check-json-isolation WENT BLIND: found $n_hits file(s) including nlohmann/json, floor is $FLOOR." >&2
  echo "  A clean verdict from an empty corpus is not a pass -- the include may have been" >&2
  echo "  renamed/vendored elsewhere, or this ran outside the worktree root." >&2
  exit 1
fi

status=0
for f in $hits; do
  case "$f" in
    mclib/model_override_registry.cpp) ;;
    mclib/anim_override_registry.cpp) ;;
    mclib/fx_def_registry.cpp) ;;
    tests/model_override/test_model_override_registry.cpp) ;;
    GameOS/gameos/visual_tuning_profile.cpp) ;;
    tests/unit/test_mission_environment.cpp) ;;
    */model_override_registry.cpp) ;;
    */anim_override_registry.cpp) ;;
    */fx_def_registry.cpp) ;;
    */test_model_override_registry.cpp) ;;
    *) echo "json isolation violated: $f"; status=1 ;;
  esac
done
[ "$status" -eq 0 ] && echo "json isolation OK ($n_hits file(s) examined)"
exit "$status"
