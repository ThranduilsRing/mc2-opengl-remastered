"""Match attached DXGI outputs to Vulkan device LUIDs; no subprocesses or GPU work."""
import ctypes as c
from functools import lru_cache
import sys
import uuid


def _method(obj, index, result, *args):
    table = c.cast(obj, c.POINTER(c.POINTER(c.c_void_p))).contents
    return c.WINFUNCTYPE(result, c.c_void_p, *args)(table[index])


def _release(obj):
    _method(obj, 2, c.c_ulong)(obj)


def dxgi_outputs():
    """Map raw adapter LUID bytes to whether any output is attached to the desktop."""
    class Desc(c.Structure):
        _fields_ = [("name", c.c_wchar * 128), ("ids", c.c_uint32 * 4),
                    ("memory", c.c_size_t * 3), ("luid", c.c_ubyte * 8), ("flags", c.c_uint32)]

    class Output(c.Structure):
        _fields_ = [("name", c.c_wchar * 32), ("rect", c.c_int32 * 4),
                    ("attached", c.c_int32), ("rotation", c.c_uint32), ("monitor", c.c_void_p)]

    factory = c.c_void_p()
    iid = (c.c_ubyte * 16).from_buffer_copy(uuid.UUID("770aae78-f26f-4dba-a829-253c83d1b387").bytes_le)
    create = c.WinDLL("dxgi").CreateDXGIFactory1
    create.argtypes = [c.c_void_p, c.POINTER(c.c_void_p)]
    create.restype = c.c_int32
    if create(iid, c.byref(factory)) != 0:
        raise RuntimeError("CreateDXGIFactory1 failed")
    result = {}
    try:
        index = 0
        while True:
            adapter = c.c_void_p()
            hr = _method(factory, 12, c.c_int32, c.c_uint32, c.POINTER(c.c_void_p))(factory, index, c.byref(adapter))
            if hr == -2005270526:  # DXGI_ERROR_NOT_FOUND
                break
            if hr != 0:
                raise RuntimeError(f"EnumAdapters1 failed: {hr}")
            try:
                desc = Desc()
                if _method(adapter, 10, c.c_int32, c.POINTER(Desc))(adapter, c.byref(desc)) != 0:
                    raise RuntimeError("GetDesc1 failed")
                attached, output_index = False, 0
                while True:
                    output = c.c_void_p()
                    hr = _method(adapter, 7, c.c_int32, c.c_uint32, c.POINTER(c.c_void_p))(adapter, output_index, c.byref(output))
                    if hr == -2005270526:
                        break
                    if hr != 0:
                        raise RuntimeError(f"EnumOutputs failed: {hr}")
                    try:
                        details = Output()
                        if _method(output, 7, c.c_int32, c.POINTER(Output))(output, c.byref(details)) != 0:
                            raise RuntimeError("output GetDesc failed")
                        attached |= bool(details.attached)
                    finally:
                        _release(output)
                    output_index += 1
                result[bytes(desc.luid)] = attached
            finally:
                _release(adapter)
            index += 1
    finally:
        _release(factory)
    return result


def vulkan_adapters():
    class App(c.Structure):
        _fields_ = [("type", c.c_uint32), ("next", c.c_void_p), ("name", c.c_char_p),
                    ("version", c.c_uint32), ("engine", c.c_char_p), ("engine_version", c.c_uint32), ("api", c.c_uint32)]

    class Create(c.Structure):
        _fields_ = [("type", c.c_uint32), ("next", c.c_void_p), ("flags", c.c_uint32),
                    ("app", c.POINTER(App)), ("layers", c.c_uint32), ("layer_names", c.c_void_p),
                    ("extensions", c.c_uint32), ("extension_names", c.c_void_p)]

    class IDs(c.Structure):
        _fields_ = [("type", c.c_uint32), ("next", c.c_void_p), ("uuid", c.c_ubyte * 16),
                    ("driver_uuid", c.c_ubyte * 16), ("luid", c.c_ubyte * 8),
                    ("node_mask", c.c_uint32), ("valid", c.c_uint32)]

    class Properties(c.Structure):
        _fields_ = [("type", c.c_uint32), ("next", c.c_void_p), ("data", c.c_uint64 * 128)]

    vk = c.WinDLL("vulkan-1")
    vk.vkCreateInstance.argtypes = [c.POINTER(Create), c.c_void_p, c.POINTER(c.c_void_p)]
    vk.vkEnumeratePhysicalDevices.argtypes = [c.c_void_p, c.POINTER(c.c_uint32), c.POINTER(c.c_void_p)]
    vk.vkGetPhysicalDeviceProperties2.argtypes = [c.c_void_p, c.POINTER(Properties)]
    vk.vkDestroyInstance.argtypes = [c.c_void_p, c.c_void_p]
    app = App(type=0, name=b"mc2-gpu-route", api=(1 << 22) | (1 << 12))
    info = Create(type=1, app=c.pointer(app))
    instance = c.c_void_p()
    if vk.vkCreateInstance(c.byref(info), None, c.byref(instance)) != 0:
        raise RuntimeError("vkCreateInstance failed")
    try:
        count = c.c_uint32()
        if vk.vkEnumeratePhysicalDevices(instance, c.byref(count), None) != 0:
            raise RuntimeError("Vulkan device count failed")
        devices = (c.c_void_p * count.value)()
        if vk.vkEnumeratePhysicalDevices(instance, c.byref(count), devices) != 0:
            raise RuntimeError("Vulkan device enumeration failed")
        rows = []
        for device in devices[:count.value]:
            ids = IDs(type=1000071004)
            props = Properties(type=1000059001, next=c.addressof(ids))
            vk.vkGetPhysicalDeviceProperties2(device, c.byref(props))
            raw = bytes(props.data)
            rows.append(dict(name=raw[20:276].split(b"\0", 1)[0].decode("utf-8"),
                             uuid=str(uuid.UUID(bytes=bytes(ids.uuid))),
                             luid=bytes(ids.luid) if ids.valid else None))
        return rows
    finally:
        vk.vkDestroyInstance(instance, None)


@lru_cache(maxsize=1)
def adapters():
    outputs = dxgi_outputs()
    return tuple(dict(row, display=outputs.get(row["luid"])) for row in vulkan_adapters())


@lru_cache(maxsize=1)
def lane_adapter():
    if sys.platform != "win32":
        return None
    try:
        rows = [r for r in adapters() if "7900 XTX" in r["name"]]
        candidates = [r for r in rows if r["display"] is False]
        if len(rows) >= 2 and any(r["display"] is True for r in rows) and len(candidates) == 1:
            return candidates[0]
        reason = "need two XTX adapters, one with attached outputs and one without"
    except Exception as exc:
        reason = str(exc)
    print(f"[GPU_ROUTE] WARNING: {reason}; keeping the default device pick", file=sys.stderr, flush=True)
    return None


def selector(row):
    return f"{row['name']}@{row['uuid']}"


if __name__ == "__main__":
    for row in adapters():
        display = {True: "yes", False: "no", None: "unknown"}[row["display"]]
        print(f"{row['name']} LUID={row['luid'].hex() if row['luid'] else 'invalid'} UUID={row['uuid']} display: {display}")
    chosen = lane_adapter()
    if chosen:
        print(f"MC2_VK_DEVICE={selector(chosen)}")
