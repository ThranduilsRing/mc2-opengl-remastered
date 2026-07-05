#!/bin/sh
# Only these TUs may include nlohmann/json. Fail otherwise.
# (Per-file case match — a space-joined grep -v -F pattern is WRONG: it treats
#  the whole allowlist as one literal and never matches either path.)
set -e
hits=$(grep -rl 'nlohmann/json' --include='*.cpp' --include='*.h' \
  mclib GameOS RenderCore code tests 2>/dev/null || true)
status=0
for f in $hits; do
  case "$f" in
    mclib/model_override_registry.cpp) ;;
    mclib/anim_override_registry.cpp) ;;
    mclib/fx_def_registry.cpp) ;;
    tests/model_override/test_model_override_registry.cpp) ;;
    */model_override_registry.cpp) ;;
    */anim_override_registry.cpp) ;;
    */fx_def_registry.cpp) ;;
    */test_model_override_registry.cpp) ;;
    *) echo "json isolation violated: $f"; status=1 ;;
  esac
done
[ "$status" -eq 0 ] && echo "json isolation OK"
exit "$status"
