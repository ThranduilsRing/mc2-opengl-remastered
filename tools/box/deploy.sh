#!/bin/bash
# Box-side installer for the tracked tools/box/ files. `box_run.py --deploy-box` uploads tools/box/ to /workspace/mc2-box.incoming and runs:
#   deploy.sh apply   [SRC]   back up every target (or note it absent), install atomically, set ccache max_size 8G, restart diskwatch and repin
#   deploy.sh revert  [TS]    restore the backup TS (default: the newest), remove targets that were absent, restore the ccache size
# Backups: /workspace/mc2-box-backup/<ts>/ (manifest = "target<TAB>backup-file|ABSENT"). Nothing else on the box is touched.
set -eu
SRC=${2:-/workspace/mc2-box.incoming}; BKR=/workspace/mc2-box-backup
# source-relative path -> target
MAP="cores.env=/workspace/locks/cores.env
env.sh=/workspace/gamerun/env.sh
mc2-diskwatch.sh=/usr/local/bin/mc2-diskwatch.sh
lane_cleanup.sh=/workspace/mc2-box/lane_cleanup.sh
mc2-box-hygiene.cron=/etc/cron.d/mc2-box-hygiene
filler/run_job.py=/workspace/filler/run_job.py
filler/sync.sh=/workspace/filler/sync.sh
filler/repin.sh=/workspace/filler/repin.sh
filler/status.py=/workspace/filler/status.py"

restart() {
  supervisorctl restart diskwatch >/dev/null 2>&1 || echo "[deploy] diskwatch not restarted (supervisorctl failed)"
  for p in $(pgrep -f 'bash /workspace/filler/repin.sh'); do kill "$p" 2>/dev/null; done
  [ -f /workspace/filler/repin.sh ] && setsid nohup bash /workspace/filler/repin.sh >/dev/null 2>&1 < /dev/null &
  return 0
}

put() {   # put <src> <target>: atomic replace, mode kept for scripts
  local s=$1 t=$2
  mkdir -p "$(dirname "$t")"
  cp "$s" "$t.new.$$" && chmod 0755 "$t.new.$$" && mv -f "$t.new.$$" "$t"
  case "$t" in /etc/cron.d/*|*.env) chmod 0644 "$t";; esac
}

case "${1:-}" in
  apply)
    TS=$(date -u +%Y%m%dT%H%M%SZ); BK=$BKR/$TS; mkdir -p "$BK"; : > "$BK/manifest"
    for line in $MAP; do
      s=$SRC/${line%%=*}; t=${line#*=}
      [ -f "$s" ] || { echo "[deploy] missing $s"; exit 2; }
      if [ -e "$t" ]; then f=$(echo "$t" | tr / _); cp -p "$t" "$BK/$f"; printf '%s\t%s\n' "$t" "$BK/$f" >> "$BK/manifest"
      else printf '%s\tABSENT\n' "$t" >> "$BK/manifest"; fi
    done
    prev=$(ccache -p 2>/dev/null | sed -n 's/.*max_size = \([0-9.]* [A-Za-z]*\).*/\1/p' | head -1 | tr -d ' ')
    printf 'CCACHE_MAX\t%s\n' "${prev:-10G}" >> "$BK/manifest"
    for line in $MAP; do put "$SRC/${line%%=*}" "${line#*=}"; done
    ccache -M 8G
    restart
    echo "[deploy] applied; backup $TS" ;;
  revert)
    TS=${2:-}; [ -n "$TS" ] || TS=$(ls "$BKR" | sort | tail -1)
    BK=$BKR/$TS; [ -f "$BK/manifest" ] || { echo "[deploy] no backup $TS"; exit 2; }
    while IFS=$'\t' read -r t b; do
      case "$t" in
        CCACHE_MAX) ccache -M "$b" ;;
        *) if [ "$b" = ABSENT ]; then rm -f "$t"; else cp -p "$b" "$t.new.$$" && mv -f "$t.new.$$" "$t"; fi ;;
      esac
    done < "$BK/manifest"
    restart
    echo "[deploy] reverted to the state before $TS" ;;
  *) echo "usage: deploy.sh apply [SRC] | revert [TS]"; exit 2 ;;
esac
