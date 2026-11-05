#!/bin/bash
# Keeps asset/Blender CPU work off the game-slot cores 0-31, and writes /workspace/STATUS.json every 60 s (tools/box/filler/status.py).
[ -f /workspace/locks/cores.env ] && . /workspace/locks/cores.env
BC=${BUILD_CPUS:-32-63}
i=0
while true; do
  for p in $(pgrep -f blender-5) $(pgrep -f scene2.py) $(pgrep -f micro.py); do taskset -apc "$BC" $p >/dev/null 2>&1; done
  i=$((i+1)); [ $((i % 2)) = 0 ] && python3 /workspace/filler/status.py >/dev/null 2>&1
  timeout 30 tail -f /dev/null
done
