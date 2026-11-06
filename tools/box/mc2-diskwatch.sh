#!/usr/bin/env bash
# MC2 box disk watchdog. Runs under supervisor; never exits. Tracked copy: tools/box/mc2-diskwatch.sh (deployed to /usr/local/bin by `box_run.py --deploy-box`).
# Levels (percent of / used):
#   >=70  WARN      trim regenerable caches and old scratch
#   >=80  PRESSURE  /workspace/DISK_PRESSURE exists; drop out dirs older than 6 h and merged idle lanes
#   >=90  CRITICAL  SIGSTOP every process whose cwd is in /workspace/lanes/*
#   <85 after CRITICAL -> SIGCONT the paused processes
# box_run's remote half separately refuses to START a build below 15 GB free (BOX_INFRA).
# Never touches: /workspace/stock, lanes' src trees of live lanes, toolchains, installs.
set -u
LOG=/workspace/diskwatch.log
STATUS=/workspace/diskwatch.status
PAUSED=/workspace/diskwatch.paused
INTERVAL=${DISKWATCH_INTERVAL:-30}
CLEAN=/workspace/mc2-box/lane_cleanup.sh

log() { echo "$(date -u +%FT%TZ) $*" >> "$LOG"; }
pct() { df --output=pcent / | tail -1 | tr -dc '0-9'; }
avail_gb() { df --output=avail -BG / | tail -1 | tr -dc '0-9'; }

trim_caches() {
  command -v ccache  >/dev/null && ccache -c >/dev/null 2>&1   # trim to max_size (deploy.sh sets it); the cache itself is kept
  command -v sccache >/dev/null && sccache --stop-server >/dev/null 2>&1
  find /workspace/lanes/*/tmp /tmp -mindepth 1 -mmin +120 -delete 2>/dev/null
}

pressure_cleanup() {
  [ -f "$CLEAN" ] && bash "$CLEAN" outs 360 >> "$LOG" 2>&1 && bash "$CLEAN" daily >> "$LOG" 2>&1
  [ -f "$CLEAN" ] && bash "$CLEAN" evict 25 >> "$LOG" 2>&1   # LRU: oldest idle lane dirs until 25 GB free (live/locked/queued/wanted lanes are never evicted)
  find /workspace/lanes -maxdepth 4 \( -name 'core' -o -name 'core.[0-9]*' \) -type f -delete -print 2>/dev/null | while read -r f; do log "deleted core $f"; done
  apt-get clean >/dev/null 2>&1
}

lane_pids() {  # processes whose cwd is inside a lane dir
  for p in /proc/[0-9]*; do
    c=$(readlink "$p/cwd" 2>/dev/null) || continue
    case "$c" in /workspace/lanes/*) echo "${p#/proc/}";; esac
  done
}

state=OK
log "diskwatch start interval=${INTERVAL}s"
while true; do
  p=$(pct); a=$(avail_gb)
  if   [ "$p" -ge 90 ]; then new=CRITICAL
  elif [ "$p" -ge 80 ]; then new=PRESSURE
  elif [ "$p" -ge 70 ]; then new=WARN
  else new=OK; fi

  [ "$new" != "$state" ] && log "state $state -> $new (used ${p}% avail ${a}G)"

  case "$new" in
    WARN)     trim_caches ;;
    PRESSURE) trim_caches; pressure_cleanup ;;
    CRITICAL) trim_caches; pressure_cleanup
              for pid in $(lane_pids); do
                if kill -STOP "$pid" 2>/dev/null; then echo "$pid" >> "$PAUSED"; fi
              done
              [ -s "$PAUSED" ] && sort -u -o "$PAUSED" "$PAUSED" && log "paused $(wc -l < "$PAUSED") lane processes" ;;
  esac

  if [ "$new" = PRESSURE ] || [ "$new" = CRITICAL ]; then touch /workspace/DISK_PRESSURE
  elif [ "$p" -lt 75 ]; then rm -f /workspace/DISK_PRESSURE; fi

  if [ -s "$PAUSED" ] && [ "$p" -lt 85 ]; then
    while read -r pid; do kill -CONT "$pid" 2>/dev/null; done < "$PAUSED"
    log "resumed $(wc -l < "$PAUSED") processes (used ${p}%)"; : > "$PAUSED"
  fi

  printf '{"time":"%s","used_pct":%s,"avail_gb":%s,"state":"%s","paused":%s}\n' \
    "$(date -u +%FT%TZ)" "$p" "$a" "$new" "$( [ -s "$PAUSED" ] && wc -l < "$PAUSED" || echo 0)" > "$STATUS"
  state=$new
  sleep "$INTERVAL"
done
