# source on the box: gr_env.sh <gpu 0-3>   sets the per-GPU run env for /workspace/gamerun (see A:/Games/mc2-ops/box-game-runs.md)
# Tracked copy: tools/box/env.sh (deployed to /workspace/gamerun/env.sh by `box_run.py --deploy-box`).
N=${1:-0}
bus=$(nvidia-smi --query-gpu=pci.bus_id --format=csv,noheader -i "$N"); bus=${bus: -12}
export DRI_PRIME="pci-$(echo "$bus" | tr ':.' '__')" MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1
export MC2_VK_DEVICE=NVIDIA SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy OMP_NUM_THREADS=1
export TMPDIR=/workspace/gamerun/tmp GR_GPU=$N GR_DISPLAY=$((90+N))
[ -f /workspace/locks/cores.env ] && . /workspace/locks/cores.env   # the one core map
_c=GPU${N}_CPUS; export GR_CPUS="${!_c:-$((8*N))-$((8*N+7))}"
# run a command under Xvfb on this GPU, pinned to the GPU's cores
gr() { taskset -c "$GR_CPUS" xvfb-run -n "$GR_DISPLAY" -s "-screen 0 1920x1080x24" "$@"; }
