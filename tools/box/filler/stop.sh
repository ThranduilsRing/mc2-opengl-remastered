#!/bin/bash
# Stop the box idle filler (/workspace/filler/stop.sh): STOP first (box-supervisor then leaves the workers down), then TERM each worker's
# process group (a running pose job ends with it; its .run marker is reaped by box-supervisor). Start again with run.sh.
set -euo pipefail
F=/workspace/filler
touch $F/STOP
GPUS=$(sed -n 's/^GPU\([0-9][0-9]*\)_CPUS=.*/\1/p' /workspace/locks/cores.env)
for g in $GPUS; do
  p=$(cat $F/worker$g.pid 2>/dev/null || true)
  if [ -n "$p" ] && [ -d "/proc/$p" ] && grep -q worker.sh "/proc/$p/cmdline" 2>/dev/null; then
    kill -TERM -- "-$p" 2>/dev/null || kill -TERM "$p" 2>/dev/null || true
    echo "worker$g stopped ($p)"
  fi
  rm -f $F/worker$g.pid
done
