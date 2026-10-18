# scripts/smoke_lib/capture_backend.py
"""Which render backend a capture asked for, which one drew it, and the refusal when they differ.

WHAT: `requested(env)` is always Vulkan (the engine ignores MC2_RENDER_BACKEND=gl: render_backend_select.cpp); `check(requested, sidecars)` returns one
message per sidecar whose backend or readback source is not what was asked for. An empty list means the capture is what it says it is.
WHY: a Vulkan context that fails to start used to put the whole session back on GL (the fallback is gone; old sidecars and `MC2_RENDER_BACKEND` typos still produce GL frames). The GL frame is then read back from the GL
framebuffer and written as an ordinary capture, so a "Vulkan arm" of an A/B can be a second GL capture and nobody sees it until the sidecar is
opened. The engine writes the backend that drew and the readback source into every sidecar; the capture drivers fail on a mismatch.
An older engine's sidecar has no `backend`; it is read from `readback_source` (vk_present = Vulkan, gl_fbo = GL), and a sidecar with
neither is a mismatch for a Vulkan request (nothing proves it was drawn by Vulkan).
"""
from __future__ import annotations

from typing import Mapping, Optional, Sequence

GL = "gl"
VULKAN = "vulkan"

_SOURCE_BACKEND = {"vk_present": VULKAN, "gl_fbo": GL}


def requested(env: Optional[Mapping[str, str]]) -> str:
    """The backend `env` selects: always 'vulkan'. The engine has one backend; MC2_RENDER_BACKEND=gl is ignored (render_backend_select.cpp)."""
    return VULKAN


def drew(sidecar: Optional[Mapping[str, object]]) -> Optional[str]:
    """The backend a sidecar says drew the frame, or None when it says nothing usable."""
    if not sidecar:
        return None
    named = str(sidecar.get("backend") or "").lower()
    if named in (GL, VULKAN):
        return named
    return _SOURCE_BACKEND.get(str(sidecar.get("readback_source") or ""))


def check(want: str, sidecars: "Sequence[tuple[str, Optional[Mapping[str, object]]]]") -> "list[str]":
    """One message per (name, sidecar) that is not a `want` capture. A missing sidecar counts: its backend is unknown."""
    problems = []
    for name, sc in sidecars:
        got = drew(sc)
        source = (sc or {}).get("readback_source")
        if got is None:
            problems.append(f"{name}: no sidecar or no backend/readback_source in it (requested {want})")
        elif got != want:
            problems.append(f"{name}: requested {want} but the sidecar says {got} drew it (readback_source={source})")
        elif want == VULKAN and source != "vk_present":
            problems.append(f"{name}: requested vulkan but the pixels came from readback_source={source}, not vk_present")
    return problems
