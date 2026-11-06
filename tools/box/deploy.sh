#!/bin/bash
# Box-side installer for the tracked tools/box/ files. `box_run.py --deploy-box` uploads tools/box/ to /workspace/mc2-box.incoming and runs:
#   deploy.sh apply   [SRC]   validate SRC, stage it as /workspace/mc2-box/releases/<ts>, back up every target, flip
#                             /workspace/mc2-box/current to the new release in one rename, (re)link the targets, ccache max_size 12G,
#                             restart diskwatch + box-supervisor (the filler workers re-exec between jobs; running rows keep their files)
#   deploy.sh revert  [TS]    flip current back to the release before backup TS (default: the newest) and restore what TS replaced
#   deploy.sh toolchain <name> <tarball>   extract into /workspace/toolchains/<name>.r-<ts>, then flip the <name> symlink in one rename
#                             (a real <name> dir is kept as <name>.orig-<ts>); keeps the two previous versions
#   deploy.sh toolchain <name> --revert    flip <name> back to the newest previous version
# Targets are symlinks into /workspace/mc2-box/current (L), regular-file copies (C: cron and supervisord ignore symlinks) or removed (R).
# Backups: /workspace/mc2-box-backup/<ts>/manifest ("target<TAB>backup-file|LINK:<dest>|ABSENT", "CURRENT<TAB>release", "CCACHE_MAX<TAB>size").
# Nothing else on the box is touched; a lane's /workspace/lanes/<key> is never read or written here.
set -euo pipefail
SRC=${2:-/workspace/mc2-box.incoming}
BOX=/workspace/mc2-box; REL=$BOX/releases; CUR=$BOX/current; BKR=/workspace/mc2-box-backup; TC=/workspace/toolchains
MAP="L:cores.env=/workspace/locks/cores.env
L:env.sh=/workspace/gamerun/env.sh
L:mc2-diskwatch.sh=/usr/local/bin/mc2-diskwatch.sh
L:lane_cleanup.sh=/workspace/mc2-box/lane_cleanup.sh
L:filler/run_job.py=/workspace/filler/run_job.py
L:filler/sync.sh=/workspace/filler/sync.sh
L:filler/status.py=/workspace/filler/status.py
L:filler/worker.sh=/workspace/filler/worker.sh
L:filler/run.sh=/workspace/filler/run.sh
L:filler/stop.sh=/workspace/filler/stop.sh
L:box_supervisor.py=/workspace/bin/box-supervisor
L:box_health_remote.py=/workspace/bin/box-health
L:quiet_slot.py=/workspace/bin/quiet-slot
C:mc2-box-hygiene.cron=/etc/cron.d/mc2-box-hygiene
C:box-supervisor.conf=/etc/supervisor/conf.d/box-supervisor.conf
R:-=/workspace/filler/repin.sh"

die() { echo "[deploy] $*" >&2; exit 2; }
lnx() { ln -sfn "$1" "$2.lnk.$$" && mv -Tf "$2.lnk.$$" "$2"; }       # atomic symlink (re)point
cpx() { cp "$1" "$2.new.$$" && chmod "$3" "$2.new.$$" && mv -f "$2.new.$$" "$2"; }   # atomic copy

validate() {   # every script parses and has LF endings, or nothing is installed
  local f bad=0
  while IFS= read -r f; do
    if grep -q $'\r' "$f"; then echo "[deploy] CRLF in ${f#$1/}" >&2; bad=1; fi
    case "$f" in
      *.sh) bash -n "$f" || { echo "[deploy] bash -n failed: ${f#$1/}" >&2; bad=1; } ;;
      *.py) python3 -c 'import ast, sys; ast.parse(open(sys.argv[1]).read(), sys.argv[1])' "$f" || bad=1 ;;
    esac
  done < <(find "$1" -type f \( -name '*.sh' -o -name '*.py' -o -name '*.env' -o -name '*.cron' -o -name '*.conf' \))
  [ "$bad" = 0 ] || die "validation failed; nothing installed"
  local line
  for line in $MAP; do
    case "$line" in R:*) continue ;; esac
    line=${line#?:}; [ -f "$1/${line%%=*}" ] || die "missing $1/${line%%=*}"
  done
}

restart() {
  local p
  for p in $(pgrep -f 'bash /workspace/filler/repin.sh' || true); do kill "$p" 2>/dev/null || true; done   # retired: box-supervisor repins
  supervisorctl reread >/dev/null 2>&1 || echo "[deploy] supervisorctl reread failed"
  supervisorctl update >/dev/null 2>&1 || echo "[deploy] supervisorctl update failed"
  supervisorctl restart diskwatch >/dev/null 2>&1 || echo "[deploy] diskwatch not restarted (supervisorctl failed)"
  supervisorctl restart box-supervisor >/dev/null 2>&1 || echo "[deploy] box-supervisor not restarted (supervisorctl failed)"
  return 0
}

prune() {   # prune <dir> <glob> <keep>: newest <keep> stay (by name = timestamp)
  local d=$1 g=$2 k=$3 x
  for x in $(cd "$d" && ls -1d $g 2>/dev/null | sort | head -n -"$k" || true); do rm -rf "${d:?}/$x"; done
}
versions() {   # versions <name> [exclude]: kept toolchain versions, oldest first (sorted by the timestamp after the last '-')
  (cd "$TC" && ls -1d "$1".r-* "$1".orig-* 2>/dev/null || true) | grep -vxF "${2:-/}" | awk -F- '{print $NF "\t" $0}' | sort | cut -f2- || true
}

case "${1:-}" in
  apply)
    validate "$SRC"
    TS=$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$REL" /workspace/bin /workspace/logs
    tmp=$REL/.tmp-$TS; rm -rf "$tmp"; cp -a "$SRC/." "$tmp"
    find "$tmp" -type f \( -name '*.sh' -o -name '*.py' \) -exec chmod 0755 {} +
    find "$tmp" -type f \( -name '*.env' -o -name '*.cron' -o -name '*.conf' \) -exec chmod 0644 {} +
    mv -T "$tmp" "$REL/$TS"
    BK=$BKR/$TS; mkdir -p "$BK"; : > "$BK/manifest"
    printf 'CURRENT\t%s\n' "$(readlink "$CUR" 2>/dev/null || echo ABSENT)" >> "$BK/manifest"
    for line in $MAP; do
      t=${line#*=}
      if [ -L "$t" ]; then printf '%s\tLINK:%s\n' "$t" "$(readlink "$t")" >> "$BK/manifest"
      elif [ -e "$t" ]; then f=$(echo "$t" | tr / _); cp -p "$t" "$BK/$f"; printf '%s\t%s\n' "$t" "$BK/$f" >> "$BK/manifest"
      else printf '%s\tABSENT\n' "$t" >> "$BK/manifest"; fi
    done
    prev=$(ccache -p 2>/dev/null | sed -n 's/.*max_size = \([0-9.]* [A-Za-z]*\).*/\1/p' | head -1 | tr -d ' ' || true)
    printf 'CCACHE_MAX\t%s\n' "${prev:-8G}" >> "$BK/manifest"
    lnx "releases/$TS" "$CUR"   # the one switch: every L target resolves through current
    for line in $MAP; do
      kind=${line%%:*}; rest=${line#?:}; s=${rest%%=*}; t=${rest#*=}
      mkdir -p "$(dirname "$t")"
      case "$kind" in
        L) [ -L "$t" ] && [ "$(readlink "$t")" = "$CUR/$s" ] || lnx "$CUR/$s" "$t" ;;
        C) cpx "$REL/$TS/$s" "$t" 0644 ;;
        R) rm -f "$t" ;;
      esac
    done
    ccache -M 12G >/dev/null 2>&1 || echo "[deploy] ccache -M failed"
    restart
    prune "$REL" '[0-9]*Z' 3; prune "$BKR" '[0-9]*Z' 10
    echo "[deploy] applied release $TS (backup $BK)" ;;
  revert)
    TS=${2:-}; [ -n "$TS" ] || TS=$(ls "$BKR" | sort | tail -1)
    BK=$BKR/$TS; [ -f "$BK/manifest" ] || die "no backup $TS"
    while IFS=$'\t' read -r t b; do
      case "$t" in
        CCACHE_MAX) ccache -M "$b" >/dev/null || true ;;
        CURRENT) case "$b" in ABSENT) rm -f "$CUR" ;; *) [ -d "$BOX/$b" ] && lnx "$b" "$CUR" || echo "[deploy] release $b is gone; current not flipped" ;; esac ;;
        *) case "$b" in
             ABSENT) rm -f "$t" ;;
             LINK:*) lnx "${b#LINK:}" "$t" ;;
             *) cp -p "$b" "$t.new.$$" && mv -f "$t.new.$$" "$t" ;;
           esac ;;
      esac
    done < "$BK/manifest"
    restart
    echo "[deploy] reverted to the state before $TS" ;;
  toolchain)
    name=${2:-}; arg=${3:-}
    case "$name" in ''|*/*|.*) die "usage: deploy.sh toolchain <name> <tarball> | --revert" ;; esac
    if [ "$arg" = --revert ]; then
      cur=$(readlink "$TC/$name" 2>/dev/null || true)
      old=$(versions "$name" "$cur" | tail -1)
      [ -n "$old" ] || die "no previous version of $name"
      lnx "$old" "$TC/$name"; echo "[deploy] toolchain $name -> $old"; exit 0
    fi
    [ -f "$arg" ] || die "no tarball $arg"
    TS=$(date -u +%Y%m%dT%H%M%SZ); tmp=$TC/.$name.tmp-$TS
    rm -rf "$tmp"; mkdir -p "$tmp"; tar xf "$arg" -C "$tmp"; mv -T "$tmp" "$TC/$name.r-$TS"
    if [ -e "$TC/$name" ] && [ ! -L "$TC/$name" ]; then mv -T "$TC/$name" "$TC/$name.orig-$TS"; fi   # first flip: the real dir becomes a version
    lnx "$name.r-$TS" "$TC/$name"
    cur=$name.r-$TS
    for x in $(versions "$name" "$cur" | head -n -2); do rm -rf "${TC:?}/$x"; done
    echo "[deploy] toolchain $name -> $cur" ;;
  *) echo "usage: deploy.sh apply [SRC] | revert [TS] | toolchain <name> <tarball>|--revert"; exit 2 ;;
esac
