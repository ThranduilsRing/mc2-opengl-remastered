#!/bin/bash
# Box lane hygiene. Tracked copy: tools/box/lane_cleanup.sh (deployed to /workspace/mc2-box/lane_cleanup.sh by `box_run.py --deploy-box`).
#   lane_cleanup.sh one <key> [--dry-run]     remove a lane: dir, .box_lane script, mirror ref, worktree entry
#   lane_cleanup.sh outs <minutes> [--dry-run] delete out/<sha8> dirs older than <minutes> in lanes with no live process
#   lane_cleanup.sh daily [--dry-run]          outs older than 24 h + idle lane dirs whose branch is merged into nifty
#   lane_cleanup.sh mirror [--dry-run]         worktree prune, repack --geometric=2, delete merged refs/lanes/*
#   lane_cleanup.sh evict <gb> [keep] [--dry-run]  below <gb> free: remove the oldest idle lane dirs (by .lane.lock mtime) one by one, each logged by name
# <key> is the lane dir name (the branch with / replaced by _). A lane is LIVE (never touched) when any of: a process command line
# names its dir or its .box_lane script, a process has its cwd inside it, or its .lane.lock is held (box_run holds it for a whole run).
# W/LANES/MIRROR/FILLER_SRC/IDLE_MIN can be overridden for tests; NO_PROC_SCAN=1 skips the /proc cwd scan (slow under MSYS).
set -u
W=${W:-/workspace}; LANES=${LANES:-$W/lanes}; MIRROR=${MIRROR:-$W/toolchains/mirror.git}
FILLER_SRC=${FILLER_SRC:-$W/filler/src}; NIFTY_KEY=claude_nifty-mendeleev; IDLE_MIN=${IDLE_MIN:-120}
DRY=0; ARGS=(); for a in "$@"; do if [ "$a" = --dry-run ]; then DRY=1; else ARGS+=("$a"); fi; done
act() { if [ $DRY = 1 ]; then echo "[lane_cleanup] would: $*"; else echo "[lane_cleanup] $*"; "$@"; fi; }

live() {   # live <key>: 0 when a process uses the lane
  local k=$1 L=$LANES/$1 p c
  ps -eo args | grep -v grep | grep -qF -e "$L/" -e ".box_lane_$k.sh" && return 0
  [ "${NO_PROC_SCAN:-0}" = 1 ] || for p in /proc/[0-9]*; do c=$(readlink "$p/cwd" 2>/dev/null) || continue; case "$c" in "$L"|"$L"/*) return 0;; esac; done
  [ -e "$L/.lane.lock" ] && { ( flock -n 8 ) 8<"$L/.lane.lock" || return 0; }
  return 1
}

nifty_ref() {   # a ref of the nifty tip in the mirror, or nothing
  git -C "$MIRROR" fetch -q "$FILLER_SRC" +refs/filler/nifty:refs/nifty-tip 2>/dev/null
  for r in refs/nifty-tip refs/lanes/$NIFTY_KEY; do git -C "$MIRROR" rev-parse -q --verify "$r" >/dev/null && { echo "$r"; return; }; done
}
merged() { git -C "$MIRROR" merge-base --is-ancestor "refs/lanes/$1" "$2" 2>/dev/null; }

lane_dirs() { for d in "$LANES"/*/; do [ -d "$d" ] && basename "$d"; done; }

one() {
  local k=$1 L=$LANES/$1
  case "$k" in ''|*/*|.*|*..*) echo "[lane_cleanup] bad lane key '$k'"; return 2;; esac
  if live "$k"; then echo "[lane_cleanup] $k: LIVE (a process uses the lane); refusing"; return 3; fi
  [ -d "$L" ] || echo "[lane_cleanup] $k: no lane dir (ref and script only)"
  act rm -rf "$L"
  act rm -f "$LANES/.box_lane_$k.sh"
  act git -C "$MIRROR" update-ref -d "refs/lanes/$k"
  act git -C "$MIRROR" worktree prune
}

outs() {
  local m=$1 k d
  for k in $(lane_dirs); do
    live "$k" && continue
    [ -d "$LANES/$k/out" ] || continue
    while IFS= read -r d; do [ -n "$d" ] && act rm -rf "$d"; done < <(find "$LANES/$k/out" -mindepth 1 -maxdepth 1 -type d -mmin +"$m")
  done
}

daily() {
  outs 1440
  local n k; n=$(nifty_ref)
  [ -z "$n" ] && { echo "[lane_cleanup] no nifty ref in the mirror: merged-lane cleanup skipped"; return 0; }
  for k in $(lane_dirs); do
    [ "$k" = "$NIFTY_KEY" ] && continue
    live "$k" && continue
    [ -n "$(find "$LANES/$k" -maxdepth 1 -mmin -"$IDLE_MIN" 2>/dev/null)" ] && continue   # touched recently: not idle
    merged "$k" "$n" && one "$k"
  done
}

mirror() {
  local n k r; n=$(nifty_ref)
  act git -C "$MIRROR" worktree prune
  if [ -n "$n" ]; then
    for r in $(git -C "$MIRROR" for-each-ref --format='%(refname)' refs/lanes/); do
      k=${r#refs/lanes/}; [ "$k" = "$NIFTY_KEY" ] && continue
      live "$k" && continue
      merged "$k" "$n" && act git -C "$MIRROR" update-ref -d "$r"
    done
  fi
  act git -C "$MIRROR" repack -d -q --geometric=2
}

free_gb() { if [ -n "${FAKE_FREE_GB:-}" ]; then echo "$FAKE_FREE_GB"; else df -BG --output=avail "$W" | tail -1 | tr -dc 0-9; fi; }
lane_age_key() { stat -c %Y "$LANES/$1/.lane.lock" 2>/dev/null || stat -c %Y "$LANES/$1" 2>/dev/null || echo 0; }

evict() {   # evict <floor_gb> [keep_key]: free space up to the floor by removing the OLDEST idle lane dirs, one named dir at a time
  local floor=$1 keep=${2:-} free k n=0 est gone=" "
  free=$(free_gb)
  while [ "${free:-0}" -lt "$floor" ]; do
    k=$(for d in $(lane_dirs); do printf '%s %s\n' "$(lane_age_key "$d")" "$d"; done | sort -n | while read -r _ d; do
          [ "$d" = "$keep" ] && continue; case "$gone" in *" $d "*) continue;; esac; live "$d" && continue; echo "$d"; break; done)
    [ -n "$k" ] || { echo "[lane_cleanup] evict: $free GB free, floor $floor GB, and no idle lane dir left to evict"; return 1; }
    est=$(du -sm --exclude=data "$LANES/$k" 2>/dev/null | cut -f1); est=$(( ${est:-0} / 1024 ))   # install/data is hardlinks to the golden install: frees nothing
    echo "[lane_cleanup] evict: removing lane dir $k (free $free GB < floor $floor GB)"
    if [ $DRY = 1 ]; then gone="$gone$k "; free=$(( free + est )); else one "$k" >/dev/null || return 1; free=$(free_gb); fi
    n=$((n+1))
  done
  echo "[lane_cleanup] evict: $free GB free (floor $floor GB), $n lane dir(s) evicted"
}

case "${ARGS[0]:-}" in
  evict) evict "${ARGS[1]:-30}" "${ARGS[2]:-}" ;;
  one) one "${ARGS[1]:-}" ;;
  outs) outs "${ARGS[1]:-1440}" ;;
  daily) daily ;;
  mirror) mirror ;;
  *) echo "usage: lane_cleanup.sh one <key>|outs <minutes>|daily|mirror|evict <floor_gb> [keep_key] [--dry-run]"; exit 2 ;;
esac
