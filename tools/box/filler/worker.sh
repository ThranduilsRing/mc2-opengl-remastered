#!/bin/bash
# Box idle filler worker for one GPU: worker.sh <gpu 0-2> (/workspace/filler/worker.sh; started by run.sh, restarted by box-supervisor).
# Lanes always win: a job starts only when EVERY game slot of the GPU (gpuN.s0..s<K-1>) is free (it then holds all of them), no lane waits for the GPU (gpuN.want), the GPU is idle, no quiet-slot
# perf row runs (/workspace/locks/quiet), and at most two GPUs fill while lanes are active. A deploy flips the script under it: the loop
# re-execs itself between jobs when the resolved script path changes, so new code lands without killing a job.
# set -u only: this daemon loop branches on return codes (no -e), and its `tr | grep -q` probes must not turn a match into a SIGPIPE
# failure (no pipefail).
set -u
G=$1
case "$G" in 0|1|2) ;; *) echo "worker.sh: GPU $G is not a filler GPU (0-2; GPU 3 is the gate's)"; exit 2 ;; esac
F=/workspace/filler
[ -f /workspace/locks/cores.env ] && . /workspace/locks/cores.env
KS=${MC2_BOX_GPU_SLOTS:-${GPU_SLOTS:-3}}; case $KS in ''|*[!0-9]*|0) KS=1;; esac   # same K as box_run
SELF=$(readlink -f "$0")
mkdir -p /workspace/locks
echo $$ > $F/worker$G.pid
echo "$SELF $(date -u +%FT%TZ)" > $F/worker$G.ver
rm -f $F/active.$G
bus=$(nvidia-smi --query-gpu=pci.bus_id --format=csv,noheader -i "$G"); bus=${bus: -12}
TAG="pci-$(echo "$bus" | tr ':.' '__')"
lane_mc2() { local p; for p in $(pgrep -x mc2); do tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -q '^FILLER_GPU=' || return 0; done; return 1; }
busy() { local p u; for p in $(pgrep -x mc2); do tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -qx "DRI_PRIME=$TAG" && return 0; done
  u=$(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits -i "$G" 2>/dev/null | tr -d ' '); [ "${u:-0}" -gt 25 ] && return 0; return 1; }
quiet_on() { local qp _r; [ -f /workspace/locks/quiet ] && read -r qp _r < /workspace/locks/quiet && [ -n "$qp" ] && [ -d "/proc/$qp" ]; }
one_job() {   # exit 1 = lock held by a lane, 2 = GPU busy / lane waiting / quiet slot / cap, 3 = no install yet, else run_job.py's rc
  local k   # take every slot (fds 10..): a lane slot held means a lane runs here; the job inherits the fds and so keeps lanes out until it ends
  for k in $(seq 0 $((KS-1))); do eval "exec $((10+k))>>/workspace/locks/gpu$G.s$k"; flock -n $((10+k)) || return 1; done
  [ -n "$(find /workspace/locks/gpu$G.want -mmin -30 2>/dev/null)" ] && return 2
  quiet_on && return 2
  busy && return 2
  [ -f $F/current_sha ] || return 3
  flock -s -n 8 || return 2
  ( flock 5
    n=$(ls $F/active.* 2>/dev/null | grep -vc "active.$G\$")
    if [ "$n" -ge 2 ] && [ -n "$(find $F/lane_seen -mmin -15 2>/dev/null)" ]; then exit 1; fi
    : > $F/active.$G ) 5>$F/cap.lock || return 2
  local rc=0
  python3 $F/run_job.py "$G" >> $F/logs/gpu$G.log 2>&1 || rc=$?
  rm -f $F/active.$G
  return $rc
}
trap 'rm -f $F/active.$G' EXIT
while [ ! -e $F/STOP ]; do
  [ "$(readlink -f "$F/worker.sh")" = "$SELF" ] || { echo "$(date -u +%FT%TZ) worker$G: worker.sh changed, re-exec"; exec bash "$F/worker.sh" "$G"; }
  { lane_mc2 || ls /workspace/locks/gpu?.want >/dev/null 2>&1; } && : > $F/lane_seen
  rc=0
  ( one_job ) 8>$F/install.lock || rc=$?
  case $rc in 0) sleep 5 ;; 1|2) sleep 20 ;; 3) sleep 180 ;; *) sleep 120 ;; esac
done
