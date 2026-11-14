"""Pin Blender Cycles to the second (non-display) GPU. Owner 11-14: ALL Blender work runs on the second GPU, never the display/game GPU.

The PC has two identical RX 7900 XTX cards, so the card is chosen by PCI bus, derived from the Vulkan deviceUUID that
A:/Games/mc2-ops/GPU_SPLIT carries as `lane_uuid=` (AMD encodes the bus in byte 4 of the UUID; Cycles HIP ids end in `_0000:<bus>:00`).
Two ways in: blender_runner.build_blender_cmd adds this file as a startup `--python` (enables only that device), and recipe scripts that
reset preferences or set their own device call `apply(scene)` again. EEVEE/Workbench cannot be pinned (headless Blender has no GPU
device preference and the Windows per-app GPU setting is a registry change): cook scripts use Cycles.
Fail loud: a missing second GPU prints `[BLENDER_GPU] FAIL ...` and exits 3; CPU only with BLENDER_GPU_ALLOW_CPU=1.
Env: BLENDER_GPU_UUID or MC2_LANE_UUID overrides GPU_SPLIT; BLENDER_GPU_BACKEND (default HIP)."""
import os
import re
import sys
from pathlib import Path

GPU_SPLIT = Path(os.environ.get("MC2_GPU_SPLIT_PATH", "A:/Games/mc2-ops/GPU_SPLIT"))


def lane_uuid():
    """The pinned GPU's deviceUUID: BLENDER_GPU_UUID, else GPU_SPLIT `lane_uuid=`, else None (host without a pinned lane GPU)."""
    env = (os.environ.get("BLENDER_GPU_UUID") or os.environ.get("MC2_LANE_UUID") or "").strip().lower()
    if env:
        return env
    try:
        for line in GPU_SPLIT.read_text(encoding="utf-8").splitlines():
            k, _, v = line.partition("=")
            if k.strip() == "lane_uuid" and v.strip():
                return v.strip().lower()
    except OSError:
        pass
    return None


def bus_of_uuid(uuid):
    """PCI bus number encoded in an AMD deviceUUID (`00000000-BB00-0000-...`: byte 4)."""
    m = re.fullmatch(r"[0-9a-f]{8}-([0-9a-f]{2})[0-9a-f]{2}-[0-9a-f-]+", uuid or "")
    if not m:
        raise ValueError(f"lane_uuid {uuid!r} is not a Vulkan UUID")
    return int(m.group(1), 16)


def select_devices(devices, bus):
    """The (device ids) of `devices` [(id, type)] that sit on PCI bus `bus` (Cycles ids end in `_<domain>:<bus hex>:<dev>`)."""
    want = f":{bus:02x}:"
    return [i for i, t in devices if t != "CPU" and want in i.lower()]


def _fail(msg):
    print(f"[BLENDER_GPU] FAIL {msg}", flush=True)
    sys.stdout.flush()
    os._exit(3)


def apply(scene=None):
    """Inside Blender: enable only the pinned GPU for Cycles and (when `scene` is given) set scene.cycles.device. Returns a status string."""
    import bpy
    uuid = lane_uuid()
    allow_cpu = os.environ.get("BLENDER_GPU_ALLOW_CPU") == "1"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    backend = os.environ.get("BLENDER_GPU_BACKEND", "HIP")
    prefs.compute_device_type = backend
    prefs.get_devices()
    devs = [(d.id, d.type) for d in prefs.devices]
    if uuid is None:   # not the PC: no pinned lane GPU, use whatever GPUs of this backend exist
        chosen = [i for i, t in devs if t != "CPU"]
        why = "unpinned (no lane_uuid in GPU_SPLIT)"
    else:
        chosen = select_devices(devs, bus_of_uuid(uuid))
        why = f"uuid={uuid} bus=0x{bus_of_uuid(uuid):02x}"
    if not chosen:
        if allow_cpu:
            print(f"[BLENDER_GPU] CPU (BLENDER_GPU_ALLOW_CPU=1): {why} not found among {devs}", flush=True)
            for d in prefs.devices:
                d.use = False
            if scene is not None:
                scene.cycles.device = "CPU"
            return "cpu"
        _fail(f"second GPU {why} not found among Cycles {backend} devices {devs}; refusing the display GPU (BLENDER_GPU_ALLOW_CPU=1 for CPU)")
    for d in prefs.devices:
        d.use = d.id in chosen
    if scene is not None:
        scene.cycles.device = "GPU"
    print(f"[BLENDER_GPU] {backend} using {chosen} {why}", flush=True)
    return "gpu"


if __name__ == "__main__":   # startup snippet: `blender --python blender_gpu.py`
    try:
        import bpy  # noqa: F401
    except ImportError:
        print(lane_uuid())
    else:
        apply(None)
