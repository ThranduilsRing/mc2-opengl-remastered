#!/bin/bash
# Box lane hygiene. Tracked copy: tools/box/lane_cleanup.sh (deployed to /workspace/mc2-box/lane_cleanup.sh by `box_run.py --deploy-box`).
#   lane_cleanup.sh one <key> [--dry-run]     remove a lane: dir, .box_lane script, mirror ref, worktree entry
#   lane_cleanup.sh outs <minutes> [--dry-run] delete out/<sha8> dirs older than <minutes> in lanes with no live process
#   lane_cleanup.sh daily [--dry-run]          outs older than 24 h + idle lane dirs whose branch is merged into nifty
#   lane_cleanup.sh mirror [--dry-run]         worktree prune, repack --geometric=2, delete merged refs/lanes/*
#   lane_cleanup.sh evict <gb> [keep] [--dry-run]  below <gb> free: remove the oldest evictable lane dirs one by one, each logged by name to lanes/EVICTIONS.log.
#                                                   NEVER evicted: a lane with a process, a held lock, a queued/running pueue task (command or path names the lane),
#                                                   a locks/<key>.want file, or activity in the last IDLE_MIN minutes. If the pueue queue cannot be read, nothing is evicted.
#   lane_cleanup.sh holders                         table of lane dirs: unique MB (hardlinks once), age, last activity, why each is not evictable
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

LOCKS=${LOCKS:-$W/locks}; PUEUE=${PUEUE:-$W/toolchains/pueue/pueue}; EVLOG=$LANES/EVICTIONS.log
ACTIVE=""; ACTIVE_FAIL=0; ACTIVE_LOADED=0
load_active() {   # text of every queued/running/stashed/paused pueue task (command, path, label); ACTIVE_FAIL=1 when the queue cannot be read
  [ $ACTIVE_LOADED = 1 ] && return 0; ACTIVE_LOADED=1
  if [ -n "${PUEUE_ACTIVE_FILE:-}" ]; then ACTIVE=$(cat "$PUEUE_ACTIVE_FILE" 2>/dev/null); return 0; fi   # tests
  ACTIVE=$("$PUEUE" status --json 2>/dev/null | jq -r '.tasks[]|select(.status|tostring|test("Queued|Running|Stashed|Paused|Locked"))|[.command,.path,(.label//"")]|join(" ")' 2>/dev/null) || ACTIVE_FAIL=1
  [ -n "$ACTIVE" ] || { "$PUEUE" status --json >/dev/null 2>&1 || ACTIVE_FAIL=1; }
}
hard_busy() {   # hard_busy <key>: prints why the lane must not be removed (RUNNING, QUEUED, WANT, QUEUE-UNKNOWN), or nothing
  local k=$1
  if live "$k"; then echo RUNNING; return; fi
  load_active
  if [ $ACTIVE_FAIL = 1 ]; then echo QUEUE-UNKNOWN; return; fi   # cannot read the queue: assume busy
  if printf '%s\n' "$ACTIVE" | sed 's/$/ /' | grep -qF -e "/lanes/$k/" -e ".box_lane_$k.sh" -e "/lanes/$k "; then echo QUEUED; return; fi
  if [ -e "$LOCKS/$k.want" ] || grep -lxF "$k" "$LOCKS"/*.want >/dev/null 2>&1; then echo WANT; return; fi
}
activity_epoch() {   # last use (evict is LRU on this): newest mtime in the lane dir (2 levels), its .lane.lock and its .box_lane script (box_run rewrites it every run)
  { find "$LANES/$1" -maxdepth 2 -printf '%T@\n' 2>/dev/null; stat -c %Y "$LANES/.box_lane_$1.sh" "$LANES/$1/.lane.lock" 2>/dev/null; } | cut -d. -f1 | sort -n | tail -1
}
log_removal() { [ $DRY = 1 ] || printf '%s removed %s reason=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$1" "${REASON:-cleanup}" >> "$EVLOG"; }

one() {
  local k=$1 L=$LANES/$1 why
  case "$k" in ''|*/*|.*|*..*) echo "[lane_cleanup] bad lane key '$k'"; return 2;; esac
  why=$(hard_busy "$k"); if [ -n "$why" ]; then echo "[lane_cleanup] $k: $why (the lane is in use or has queued work); refusing"; return 3; fi
  [ -d "$L" ] || echo "[lane_cleanup] $k: no lane dir (ref and script only)"
  log_removal "$k"
  act rm -rf "$L"
  act rm -f "$LANES/.box_lane_$k.sh"
  act git -C "$MIRROR" update-ref -d "refs/lanes/$k"
  act git -C "$MIRROR" worktree prune
}

outs() {
  local m=$1 k d
  for k in $(lane_dirs); do
    [ -n "$(hard_busy "$k")" ] && continue
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
    [ -n "$(hard_busy "$k")" ] && continue
    [ -n "$(find "$LANES/$k" -maxdepth 1 -mmin -"$IDLE_MIN" 2>/dev/null)" ] && continue   # touched recently: not idle
    merged "$k" "$n" && REASON="daily-merged" one "$k"
  done
}

mirror() {
  local n k r; n=$(nifty_ref)
  act git -C "$MIRROR" worktree prune
  if [ -n "$n" ]; then
    for r in $(git -C "$MIRROR" for-each-ref --format='%(refname)' refs/lanes/); do
      k=${r#refs/lanes/}; [ "$k" = "$NIFTY_KEY" ] && continue
      [ -n "$(hard_busy "$k")" ] && continue
      merged "$k" "$n" && act git -C "$MIRROR" update-ref -d "$r"
    done
  fi
  act git -C "$MIRROR" repack -d -q --geometric=2
}

free_gb() { if [ -n "${FAKE_FREE_GB:-}" ]; then echo "$FAKE_FREE_GB"; else df -BG --output=avail "$W" | tail -1 | tr -dc 0-9; fi; }
declare -A SZ=() WHY=() ACT=()
scan_lanes() {   # per lane: unique MB (golden-install listed FIRST in one du call, so hardlinks shared with it count there, once), last-activity epoch, busy reason
  local mb p k
  load_active   # in this shell, so the queue is read once, not once per lane
  while IFS=$'\t' read -r mb p; do k=$(basename "$p"); SZ[$k]=$mb; done < <(du -sm "$W/golden-install" "$LANES"/*/ 2>/dev/null)
  for k in $(lane_dirs); do
    ACT[$k]=$(activity_epoch "$k"); ACT[$k]=${ACT[$k]:-0}
    WHY[$k]=$(hard_busy "$k")
    [ -z "${WHY[$k]}" ] && [ $(( $(date +%s) - ${ACT[$k]} )) -lt $(( IDLE_MIN * 60 )) ] && WHY[$k]=RECENT
  done
}
holders() {   # table of the lane dirs holding space: unique size, age, last activity, why each is (not) evictable
  local k now born; now=$(date +%s)
  [ ${#WHY[@]} -gt 0 ] || scan_lanes
  echo "[lane_cleanup] lane dirs holding space (unique MB = hardlinks counted once, shared data attributed to golden-install: ${SZ[golden-install]:-?} MB):"
  for k in $(lane_dirs); do
    born=$(stat -c %W "$LANES/$k" 2>/dev/null); { [ -z "$born" ] || [ "$born" = 0 ]; } && born=$(stat -c %Z "$LANES/$k" 2>/dev/null || echo "$now")
    printf '%s\t  %-34s unique=%6s MB  age=%4s h  last_activity=%5s min ago  %s\n' "${ACT[$k]}" "$k" "${SZ[$k]:-?}" "$(( (now - born) / 3600 ))" "$(( (now - ${ACT[$k]}) / 60 ))" "${WHY[$k]:-idle}"
  done | sort -n | cut -f2-
}

evict() {   # evict <floor_gb> [keep_key]: free space up to the floor by removing the OLDEST idle lane dirs (never one that is running, queued in pueue, wanted, or active in the last IDLE_MIN), each logged by name
  local floor=$1 keep=${2:-} free k n=0 est gone=" " d
  free=$(free_gb); [ "${free:-0}" -lt "$floor" ] || { echo "[lane_cleanup] evict: $free GB free (floor $floor GB), 0 lane dir(s) evicted"; return 0; }
  scan_lanes
  while [ "${free:-0}" -lt "$floor" ]; do
    k=$(for d in $(lane_dirs); do printf '%s %s\n' "${ACT[$d]}" "$d"; done | sort -n | while read -r _ d; do
          [ "$d" = "$keep" ] && continue; case "$gone" in *" $d "*) continue;; esac; [ -n "${WHY[$d]}" ] && continue; echo "$d"; break; done)
    if [ -z "$k" ]; then echo "[lane_cleanup] evict: $free GB free, floor $floor GB, and no evictable lane dir left (all are running, queued, wanted or recently active):"; holders; return 1; fi
    est=$(( ${SZ[$k]:-0} / 1024 ))
    echo "[lane_cleanup] evict: removing lane dir $k (free $free GB < floor $floor GB, unique ${SZ[$k]:-?} MB, last activity $(( ($(date +%s) - ${ACT[$k]}) / 60 )) min ago)"
    if [ $DRY = 1 ]; then gone="$gone$k "; free=$(( free + est ))
    else REASON="evict floor=${floor}GB free=${free}GB unique_mb=${SZ[$k]:-?} last_activity_min=$(( ($(date +%s) - ${ACT[$k]}) / 60 ))" one "$k" >/dev/null || return 1; free=$(free_gb); fi
    n=$((n+1))
  done
  echo "[lane_cleanup] evict: $free GB free (floor $floor GB), $n lane dir(s) evicted"
}

case "${ARGS[0]:-}" in
  holders) scan_lanes; holders ;;
  evict) evict "${ARGS[1]:-30}" "${ARGS[2]:-}" ;;
  one) one "${ARGS[1]:-}" ;;
  outs) outs "${ARGS[1]:-1440}" ;;
  daily) daily ;;
  mirror) mirror ;;
  *) echo "usage: lane_cleanup.sh one <key>|outs <minutes>|daily|mirror|holders|evict <floor_gb> [keep_key] [--dry-run]"; exit 2 ;;
esac
