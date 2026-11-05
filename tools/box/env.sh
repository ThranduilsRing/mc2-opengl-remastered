# source on the box: gr_env.sh <gpu 0-3>   sets the per-GPU run env for /workspace/gamerun (see A:/Games/mc2-ops/box-game-runs.md)
# Tracked copy: tools/box/env.sh (deployed to /workspace/gamerun/env.sh by `box_run.py --deploy-box`).
N=${1:-0}
# GPU 3 is the gate GPU: only a process under the live gpu3.owner (a box_run gate row) may set up its env. Fail closed. (Sourced: no set -e.)
if [ "$N" = 3 ]; then
  _o=$(cut -d' ' -f1 /workspace/locks/gpu3.owner 2>/dev/null); _p=$$; _ok=""
  if [ -n "$_o" ] && [ -d "/proc/$_o" ]; then
    for _i in $(seq 64); do [ "$_p" = "$_o" ] && { _ok=1; break; }; [ "${_p:-0}" -le 1 ] && break; _p=$(sed 's/.*) //' /proc/$_p/stat 2>/dev/null | cut -d' ' -f2); done
  fi
  if [ -z "$_ok" ]; then echo "env.sh: GPU 3 is reserved for gate rows (box_run --gate); refusing (owner=${_o:-none})" >&2; return 1 2>/dev/null || exit 1; fi
fi
bus=$(nvidia-smi --query-gpu=pci.bus_id --format=csv,noheader -i "$N"); bus=${bus: -12}
export DRI_PRIME="pci-$(echo "$bus" | tr ':.' '__')" MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1
export MC2_VK_DEVICE=NVIDIA SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy OMP_NUM_THREADS=1
export TMPDIR=/workspace/gamerun/tmp GR_GPU=$N GR_DISPLAY=$((90+N))
[ -f /workspace/locks/cores.env ] && . /workspace/locks/cores.env   # the one core map
_c=GPU${N}_CPUS; export GR_CPUS="${!_c:-$((8*N))-$((8*N+7))}"
# run a command under Xvfb on this GPU, pinned to the GPU's cores
gr() { taskset -c "$GR_CPUS" xvfb-run -n "$GR_DISPLAY" -s "-screen 0 1920x1080x24" "$@"; }
