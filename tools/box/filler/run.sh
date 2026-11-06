#!/bin/bash
# Start the box idle filler workers for GPUs 0-2 (/workspace/filler/run.sh). Clears STOP; a worker whose pid is alive is left alone.
# box-supervisor restarts dead workers on its own; this is the manual start after stop.sh.
set -euo pipefail
F=/workspace/filler
mkdir -p $F/logs $F/results $F/tmp /workspace/locks
rm -f $F/STOP
for g in 0 1 2; do
  p=$(cat $F/worker$g.pid 2>/dev/null || true)
  if [ -n "$p" ] && [ -d "/proc/$p" ] && grep -q worker.sh "/proc/$p/cmdline" 2>/dev/null; then echo "worker$g alive ($p)"; continue; fi
  setsid nohup bash $F/worker.sh $g >> $F/logs/worker$g.log 2>&1 < /dev/null &
  echo "worker$g started ($!)"
done
