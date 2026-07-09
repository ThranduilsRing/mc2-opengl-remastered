#!/usr/bin/env python3
"""Check a Vulkan no-interop proof log for forbidden fallback/interoperability paths."""

from __future__ import annotations

import argparse
from pathlib import Path


FORBIDDEN = [
    "[VK_NO_GL_INTEROP_STRICT]",
    "MC2_VK_PRESENT_NATIVE_STRICT abort",
    "present_image_not_vk_owned",
    "glInteropSignalToVk",
    "glInteropWaitFromVk",
    "glInteropViewCreate",
    "vkImageResourceCreate(exportable)",
    "VK_POST_HZB_TRACE",
    "VK_POST_SCREENFX_TRACE",
    "VK_POST_SSAO_TRACE",
    "VK_PRESENT_COMPOSITE_TRACE",
    "Crash Report",
    "DEVICE_LOST",
]


REQUIRED = [
    "[VK_PRESENT]",
    "composite_vk_frames=",
    "gl_handoff_skip_frames=",
    "imgui_vk_frames=",
]


REQUIRED_ENV = [
    "MC2_VK_NO_GL_INTEROP_STRICT=1",
    "MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT=1",
    "MC2_VK_PRESENT_NO_GL_IMPORT=1",
    "MC2_VK_PRESENT_NATIVE_STRICT=1",
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", type=Path)
    ap.add_argument("--env-file", type=Path,
                    help="Optional vk_strict_env.txt written by run_vk_nointerop_strict_visible.py.")
    ap.add_argument("--no-resource-strict", action="store_true",
                    help="Do not require the resource-strict env receipt.")
    args = ap.parse_args()

    text = args.log.read_text(errors="replace")
    failed = False

    for needle in FORBIDDEN:
        count = text.count(needle)
        print(f"{needle}: {count}")
        if count:
            failed = True

    for needle in REQUIRED:
        present = needle in text
        print(f"log required {needle}: {'yes' if present else 'NO'}")
        if not present:
            failed = True

    if args.env_file:
        env_text = args.env_file.read_text(errors="replace")
        required_env = REQUIRED_ENV if not args.no_resource_strict else [
            item for item in REQUIRED_ENV if not item.startswith("MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT=")
        ]
        for needle in required_env:
            present = needle in env_text
            print(f"env required {needle}: {'yes' if present else 'NO'}")
            if not present:
                failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
