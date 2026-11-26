#!/usr/bin/env python3
"""deploy_payload.py -- deploy payload completeness gate (slice S8 seed, manifest v1).

Wraps/validates a deploy of the MC2 runtime payload from a source worktree
build to a runtime target dir (e.g. A:/Games/mc2-opengl/mc2-win64-v0.4 or
.../mc2-win64-0.4c). Payload, by target:
  both    : exe + pdb + ffmpeg DLLs + shaders/ + run-with-log.bat
  game    : + run-mc2.bat
  editor  : + run-editor.bat + tools/terrain_gen/** (authoring support -- the
            editor's Generate-Mission / -gen-map path shells `py -3
            tools\\terrain_gen\\terrain_gen.py`; without it the editor builds
            and launches but every gen/mission-load smoke case fails)
Support files (launch .bat, terrain_gen tree) are source-tracked, not build
outputs, so build -> deploy stays self-complete with no manual copy step.

Hardens against the four documented stale-deploy failure classes
(docs/superpowers/recon/scratch-accel-1-deploy.md):
  A. stale exe via process lock          -> Step 0 lock check, HARD FAIL, never taskkill
  B. stale shaders via cp -r no-overwrite -> per-file copy + post-copy byte diff, HARD FAIL
  C. wrong deploy target                  -> .deployed_manifest.csv identity record
  D. stale PDB                            -> PDB present + hash == source, HARD FAIL
                                             (override: --allow-stale-pdb)

After a successful deploy, writes <target>/.deployed_manifest.csv (manifest v1):
  rows = relpath,sha256,bytes,src_commit,timestamp,mtime_ns

Unchanged files (source size AND mtime_ns match the previous row, and the
deployed copy still has the recorded size) are not re-copied or re-hashed.
--full-verify disables that fast path and copies/hashes every payload file.

--verify-only re-checks an existing target against its manifest (post-hoc
staleness detector). Advisory: exit 0 with a report, unless --strict.

Python (not .sh) to match sibling conventions: scripts/ is python-dominated
(run_smoke.py, check-asset-manifests.py, ...) and we need sha256 + csv + robust
Windows file-lock probing, all stdlib here.

NO engine code. NO run_smoke verdict-path involvement. tier1 not required
(scripts-only, zero runtime change).
"""

import argparse
import atexit
import csv
import ctypes
import datetime
import glob
import hashlib
import itertools
import importlib.util
import json
import os
import ntpath
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
import threading
import zipfile
from contextlib import contextmanager
import console_child  # noqa: E402  (stdio-inheriting children keep their output; scripts/console_child.py)

try:
    import psutil
    _PSUTIL_OK = True
except ImportError:
    _PSUTIL_OK = False

MANIFEST_NAME = ".deployed_manifest.csv"
MANIFEST_VERSION = "v1"
REUSE_NAME = ".deploy-inputs.json"


def editor_only_payload(rel):
    rel = str(rel).replace("\\", "/").casefold()
    return rel in {"mission editor.exe", "mission editor.pdb", "run-editor.bat"} or rel.startswith("tools/terrain_gen/")


_PAUSE = {"depth": 0, "since": 0.0, "total": 0.0}
_PAUSE_LOCK = threading.Lock()


@contextmanager
def phase_paused():
    """Stop every active deploy_phase clock (a cache-miss stock pak pack takes minutes and has its own subprocess timeout)."""
    with _PAUSE_LOCK:
        if _PAUSE["depth"] == 0:
            _PAUSE["since"] = time.monotonic()
        _PAUSE["depth"] += 1
    try:
        yield
    finally:
        with _PAUSE_LOCK:
            _PAUSE["depth"] -= 1
            if _PAUSE["depth"] == 0:
                _PAUSE["total"] += time.monotonic() - _PAUSE["since"]


def _paused_seconds():
    with _PAUSE_LOCK:
        return _PAUSE["total"] + (time.monotonic() - _PAUSE["since"] if _PAUSE["depth"] else 0.0)


@contextmanager
def deploy_phase(name, timeout=600):
    """Report progress and terminate this deploy's children if a phase stalls."""
    if not _PSUTIL_OK:
        fail("bounded deploy phases require psutil (child cleanup on timeout)")
    started = time.monotonic()
    paused_at_start = _paused_seconds()
    done = threading.Event()

    def watch():
        while not done.wait(min(15, timeout)):
            elapsed = time.monotonic() - started - (_paused_seconds() - paused_at_start)
            log(f"phase {name}: running {elapsed:.1f}s / {timeout}s")
            if elapsed >= timeout:
                log(f"ERROR: phase {name} timed out after {elapsed:.1f}s")
                # Only descendants of this deploy, never another lane's processes.
                children = psutil.Process().children(recursive=True)
                for child in reversed(children):
                    try:
                        child.kill()
                    except psutil.Error:
                        pass
                psutil.wait_procs(children, timeout=5)
                os._exit(8)

    log(f"phase {name}: start (timeout {timeout}s)")
    worker = threading.Thread(target=watch, daemon=True)
    worker.start()
    try:
        yield
    finally:
        done.set()
        worker.join()
        log(f"phase {name}: finished {time.monotonic() - started:.3f}s")


def deploy_inputs(src_root):
    """Hash authored inputs, never the multi-GB kit store or cooked outputs."""
    roots = ["data", "mods", "examples", "assets", "scripts", "tools", "cmake", "CMakeLists.txt",
             "run-mc2.bat", "run-editor.bat", "run-with-log.bat", "3rdparty/smaa", "3rdparty/cmaa2"]
    result = subprocess.run(["git", "-C", str(src_root), "ls-files", "-z", "--cached",
                             "--others", "--exclude-standard", "--", *roots],
                            capture_output=True, check=True, timeout=30,
                            **console_child.child_kwargs(redirected=True))
    names = set(os.fsdecode(p) for p in result.stdout.split(b"\0") if p)
    # Loose deploy members may be ignored by Git, but are still live inputs.
    names.update(rel for rel in IMPORTED_STRUCTURE_PAYLOAD + GAME_DATA_PAYLOAD + TERRAIN_LAYER_PAYLOAD
                 if (Path(src_root) / rel).is_file())
    names.update(p.relative_to(src_root).as_posix()
                 for p in (Path(src_root) / EXAMPLE_MODS_SRC).rglob("*") if p.is_file())
    # The importer consumes engine sources too; changes to its source closure
    # invalidate the cook even when the pinned package JSON did not change.
    cmake = Path(src_root) / "tools/tg_import_dump/CMakeLists.txt"
    if cmake.is_file():
        names.update(p for p in re.findall(r"\$\{MC2_ROOT\}/([\w./-]+)", cmake.read_text())
                     if Path(p).suffix in (".cpp", ".h", ".c", ".hpp"))
    for directory in ("mclib", "GameOS"):
        names.update(p.relative_to(src_root).as_posix()
                     for p in (Path(src_root) / directory).rglob("*") if p.suffix in (".h", ".hpp"))
    return {name: sha256_file(Path(src_root) / name) for name in sorted(names)} | {
        "environment": [os.environ.get(k, "") for k in ("MC2_KIT_STORE", "MC2_REF_ROOT")]}


def reusable_payload(src_root, target, inputs):
    """Fail closed on missing inputs, manifests, outputs or sampled byte drift."""
    import random
    from kit_payload import contained_path
    try:
        parents = {}
        saved = json.loads((Path(target) / REUSE_NAME).read_text())
        if saved["inputs"] != inputs:
            return False, "authored kit/mod or deploy inputs changed"
        if saved["manifest"] != sha256_file(Path(target) / MANIFEST_NAME):
            return False, "payload manifest changed"
        manifest = read_manifest(target)
        if not manifest or not saved["outputs"]:
            return False, "missing payload manifest or output inventory"
        for rel, stamp in saved["outputs"].items():
            p = contained_path(target, rel, parents=parents)
            s = p.stat()
            if [s.st_size, s.st_mtime_ns, s.st_ctime_ns] != stamp:
                return False, f"output missing or changed: {rel}"
        for src, (stamp, expected) in saved["sources"].items():
            s = Path(src).stat()
            if [s.st_size, s.st_mtime_ns, s.st_ctime_ns] != stamp:
                return False, f"kit source changed: {src}"
        for kit in ("mods/mc2-industrial-kit", "mods/mc2-vehicle-kit"):
            expected = {rel for rel in saved["outputs"] if rel.startswith(kit + "/")}
            if expected:
                actual = {p.relative_to(target).as_posix() for p in (Path(target) / kit).rglob("*")
                          if p.is_file() and p.name != ".modindex-cache"}
                if actual != expected:
                    return False, f"kit inventory changed: {kit}"
        for src in random.SystemRandom().sample(list(saved["sources"]), min(6, len(saved["sources"]))):
            content = Path(src).read_bytes()
            if src.endswith(".mcasset.json"):
                content = content.replace(b"\r\n", b"\n")
            if hashlib.sha256(content).hexdigest() != saved["sources"][src][1]:
                return False, f"sampled kit source mismatch: {src}"
        # Always include both kit packages and their native recipes. Rotate the
        # sample so repeated deploys don't leave the same files unchecked.
        groups = {}
        for rel in saved["outputs"]:
            group = rel.split("/", 2)[:2] if rel.startswith("mods/") else rel.split("/")[:1]
            groups.setdefault("/".join(group), []).append(rel)
        for paths in groups.values():
            for rel in random.SystemRandom().sample(paths, min(3, len(paths))):
                expected = saved["hashes"][rel]
                if sha256_file(contained_path(target, rel, parents=parents)) != expected:
                    return False, f"sampled hash mismatch: {rel}"
        return True, "authored input hashes and recorded payload match; output samples verified"
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        return False, "missing or invalid reuse/manifest evidence"


def record_deploy_inputs(target, inputs, hashes, items):
    """Publish reuse evidence only after the full deploy and all cooks succeed."""
    from kit_payload import contained_path
    inventory = {rel: row[0] for rel, row in hashes.items()}
    for name in ("industrial-cook.json", "vehicle-cook.json", "trees-cook.json"):
        receipt = Path(target) / "cache/content-model" / name
        if receipt.is_file():
            inventory[receipt.relative_to(target).as_posix()] = sha256_file(receipt)
            for row in json.loads(receipt.read_text())["files"]:
                inventory["cache/content-model/" + row["path"]] = row["sha256"]
    outputs = {}
    parents = {}
    for rel in inventory:
        s = contained_path(target, rel, parents=parents).stat()
        outputs[rel] = [s.st_size, s.st_mtime_ns, s.st_ctime_ns]
    sources = {}
    for src, rel, kind in items:
        if kind not in ("exe", "pdb", "shader", "dll"):
            s = Path(src).stat()
            sources[str(src)] = [[s.st_size, s.st_mtime_ns, s.st_ctime_ns], hashes[rel][0]]
    value = dict(inputs=inputs, manifest=sha256_file(Path(target) / MANIFEST_NAME),
                 outputs=outputs, hashes=inventory, sources=sources)
    pending = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", dir=target, delete=False, encoding="utf-8") as out:
            pending = Path(out.name)
            json.dump(value, out, sort_keys=True)
        os.replace(pending, Path(target) / REUSE_NAME)
    finally:
        if pending is not None:
            pending.unlink(missing_ok=True)
SHADER_EXTS = (".vert", ".frag", ".tesc", ".tese", ".comp")

# Editor target marker (exe_name match, case-insensitive).
EDITOR_EXE = "mission editor.exe"

# Launch scripts that belong in a user download, by target. Sourced from
# src_root (tracked at repo root). run-with-log.bat is the generic stderr-
# capturing launcher and ships with both.
SUPPORT_SCRIPTS = {
    "game":   ["run-mc2.bat", "run-with-log.bat"],
    "editor": ["run-editor.bat", "run-with-log.bat"],
}

# Obsolete payload excluded from runtime bootstraps.
RETIRED_LAUNCHER = "mc2-launcher.exe"

# Editor authoring support trees (src_root-relative dirs copied recursively,
# structure preserved). The editor's -gen-map / Generate Mission path shells
# `py -3 tools\\terrain_gen\\terrain_gen.py`; without this tree the editor
# builds and launches but every gen/mission-load smoke case fails. Tying it to
# the deploy payload keeps "build -> deploy" self-complete (no manual copy).
EDITOR_SUPPORT_TREES = ["tools/terrain_gen"]

# Cook tools shipped with BOTH game and editor installs so modders/users can
# re-cook or inspect assets without a full source checkout.
#   tools/cook_bc6h_hdri.py  -- EXR -> BC6H KTX2 via texconv+ktx (HDRI baking)
#   tools/examples/          -- asset inspection helpers (list_mission_assets etc.)
GAME_COOK_TOOLS = ["tools/cook_bc6h_hdri.py", "tools/examples"]
# Tracked loose vehicle PBR maps (PNG albedo/normal/ORM); mods keep
# their own art (File::open priority). Walked file-by-file.
GAME_LOOSE_ART_TREES = ["data/textures/vehicle_pbr", "data/textures/256Overlays"]

# Repo-shipped imported structure (TERRAIN-CLIFF-MESH-DRESSING-P0): data/tgl is enumerated
# file-by-file, not copied wholesale, so these reach an install only through this list.
# Building/prop replacements ship as mounted content packages (tools/content_pipeline), not here.
IMPORTED_STRUCTURE_PAYLOAD = [
    "data/tgl/spotlight.ini",
    "data/tgl/marblecliff.ini",
    "data/tgl/MarbleCliffGLB.glb",
]

# Retired repo payload that lean-seed installs still carry (the deploy manifest never owned them, so retire_unowned
# cannot prune them). Removed only when the bytes match the retired file. data/tgl/quonset.ini silently replaced the
# stock quonset through [Import] with QuonsetGLB (PBR roof), lighting four huts unlike the stock Quonset2 (quonset-light-2).
RETIRED_SEED_PAYLOAD = {
    # Stock mech retextures retired at mech-pbr-retexture-final; preserve edited bytes.
    "data/textures/mech_pbr/anubis_albedo.png": "362f6de175cb232f5633a628098f5c2e011f28d1b9e8ab8ce31bd6ca909a009f",
    "data/textures/mech_pbr/anubis_normal.ktx2": "acd48b29a4e09ac1d2159d868635a9632925a64697a3637450f5e6fc8b4590a8",
    "data/textures/mech_pbr/anubis_normal.png": "f0e15a0bb882b79a0e2482d12f5e5162e017f1fb9f87d3b7cbcf22825a8bc65b",
    "data/textures/mech_pbr/anubis_orm.ktx2": "7334bfcd89cca0510195c480ef7390cabbdd9ff90fa2f7b3991ab22f09ef69d1",
    "data/textures/mech_pbr/anubis_orm.png": "ab23359d5a4775972086730051a78ec681c64840fddc84399db10ff12a4fbb19",
    "data/textures/mech_pbr/atlas_albedo.png": "0a2e53d29b8ad3815dc79500010c6b7b7d20c7b6773cd6e04b341e317593dcb9",
    "data/textures/mech_pbr/atlas_normal.ktx2": "c89373af0b1d1a1ca98ed7ab3e371613217a7e2561668eab8b5af8ab007e26a5",
    "data/textures/mech_pbr/atlas_normal.png": "a2d964f5bb1517c7c878c3b8e5c76c97864fcd71458d28028320be64b9e069bc",
    "data/textures/mech_pbr/atlas_orm.ktx2": "94074ccd9116914c1669efb099abfa60c96c15a8b4d055749daaf849943b4d12",
    "data/textures/mech_pbr/atlas_orm.png": "dc841e4a47ad0b2d82717660bcf05ba631f41067543b6d41a37e00c9830b8bd6",
    "data/textures/mech_pbr/bloodasp_albedo.png": "5775ae6780e3b448cc4c01954744a3b748ca5e67b2b3220c349c0f62ab0e816e",
    "data/textures/mech_pbr/bloodasp_normal.ktx2": "175edbaa5b11900b02f57caa50e062a622a64e59353fa88ac0aa1ea2187349f3",
    "data/textures/mech_pbr/bloodasp_normal.png": "ecca718ac0ff3841ef2c60cf81c0f971a3bf3945badda920bc1be221070a8fb3",
    "data/textures/mech_pbr/bloodasp_orm.ktx2": "b6627080044ae67e73fbbd954ce9869f7a0915215828362be3831f91b6b3bbcc",
    "data/textures/mech_pbr/bloodasp_orm.png": "6b22d3349c952dba25cf2db62688cb0ed13d4206234ebac81a6aceac769e3d56",
    "data/textures/mech_pbr/bushwacker_albedo.png": "7c69c1d8a401545c1baec45c6c5cb523518cc94e4d6c6206ee279accad94f9f8",
    "data/textures/mech_pbr/bushwacker_normal.ktx2": "86043d93f804491bff2194195594e11757476620f3cfdeb69071b916a3b4243b",
    "data/textures/mech_pbr/bushwacker_normal.png": "0ddba4970695cd721e6ed5e3ed70e275f6b7069b53b87f528310c8e6a0faccb1",
    "data/textures/mech_pbr/bushwacker_orm.ktx2": "af536de4cd25badc5be21fb642f2180d38f9095009a1fbda4b475bb311038d46",
    "data/textures/mech_pbr/bushwacker_orm.png": "7032f488aaa99f9a8ac98442835d2ce3cb3d5219ae5fcb609ff22dbe0fcaeb83",
    "data/textures/mech_pbr/catapult_albedo.png": "9db3976fc1f37d6da42e89fe61881749f8fedfd7fcb3fe07aa5229dce46dfec3",
    "data/textures/mech_pbr/catapult_normal.ktx2": "085a3e5e8aaa2ee6f9bd9bac1c75677d99dd37d64b2fa2b425afc8843e04a56a",
    "data/textures/mech_pbr/catapult_normal.png": "b7941c24941f340aa1e48781a24fa556c9df4c8f9208a9b1651cd017fa2c5357",
    "data/textures/mech_pbr/catapult_orm.ktx2": "908ec9045c9cb3df1efbd341c265114d9ec390eb9df59c213bb7f13c3eb04075",
    "data/textures/mech_pbr/catapult_orm.png": "98d7c03e16234a2284f20de54c6e92b2c247d0a9959e12e1f1a321b1ad825b5b",
    "data/textures/mech_pbr/couger_albedo.png": "099a4b5b876811e9d66c41d451358d0daf6a78885b2b42f7e345693febbc090c",
    "data/textures/mech_pbr/couger_normal.ktx2": "f9800186ab16dba5e5404122ba91a550ea03fc0714a80017319f3f00688d5251",
    "data/textures/mech_pbr/couger_normal.png": "10e6cafffa589b8de17de97ab24756b9b4802f1678599dcf343ec82fd37dccb5",
    "data/textures/mech_pbr/couger_orm.ktx2": "b9485c614464a1a57fb9d52e61b7cef023646da5e519865ca73e7f9806aac641",
    "data/textures/mech_pbr/couger_orm.png": "a9835c46ad99a5af522a6a0d9b4c6ace140e4ac0713b2cb8253e5a8d7ef27be4",
    "data/textures/mech_pbr/cyclops_albedo.png": "e45181a2c4c5471ee7557b90ba70dc12cc5662733c08a064932147257b152dab",
    "data/textures/mech_pbr/cyclops_normal.ktx2": "e7741845640af8b15a00e73dc7fa7a46ac41e9dd1c86fcf9feec0a350f8dfcb2",
    "data/textures/mech_pbr/cyclops_normal.png": "1fe94dbfc9a1610ea3fda61ef8ab113175e41ee4a195e6b523d48019af438af3",
    "data/textures/mech_pbr/cyclops_orm.ktx2": "389f1ab8c2bf9159359cc100f49ba51f34b1bf5dc3530757cf450756f45e28ea",
    "data/textures/mech_pbr/cyclops_orm.png": "bac4fc84d44a46d53fac34891b59ab77221de71d1a3fd5ad53862b4cfb66d37d",
    "data/textures/mech_pbr/enfield_albedo.png": "cc365bc19c3a18147d7f7097378a6c4ab3efc7d533a46a7ca22d87b886e895db",
    "data/textures/mech_pbr/enfield_normal.ktx2": "2f1fff4c42ef8ac662b5f039e24ea8b90d5e7510b67920c0ccf7d15c9c31ea7f",
    "data/textures/mech_pbr/enfield_normal.png": "85b2b06680ccbb6c756ff777b835a4acb6f16640b93436c1f0c04f71e3a77a84",
    "data/textures/mech_pbr/enfield_orm.ktx2": "a90d2d1b31d288a6fd253d531c5cc291acd09a70a544390ce94e05d0016ef887",
    "data/textures/mech_pbr/enfield_orm.png": "570ad424ff1fa5618a597d5a4e9ce68fb9ea0e7c42eea81c42a23e3f667c378d",
    "data/textures/mech_pbr/fireant_albedo.png": "084d7b467b1792034628b67facb71b59ad8237351b67a4604168ad19cd6e0ee9",
    "data/textures/mech_pbr/fireant_normal.ktx2": "69a400aad215fa08a02fdb0ddb1fe29c9ab25079178ebfce69769e8597f4be54",
    "data/textures/mech_pbr/fireant_normal.png": "22d5bf1dfc4b1947560fa7f3714032c25b6c23ed9b176b292eed88486ef3a476",
    "data/textures/mech_pbr/fireant_orm.ktx2": "b110ba1331034c60189a11d79a3e2c17399de89773467861feb67c2d2979f432",
    "data/textures/mech_pbr/fireant_orm.png": "0997c6ac838eff9e54e6bcaf61b5b970e8c75caf1fc76408b7e30c6dc87aa2c8",
    "data/textures/mech_pbr/ghoul_albedo.png": "6c65c4103750539a654905758438de62a1b854e83bbfa2197926a757271abe4e",
    "data/textures/mech_pbr/ghoul_normal.ktx2": "daaca47c990347241de84ff4922cc8655b13fd00cf4a23f6bae30484cfa1d385",
    "data/textures/mech_pbr/ghoul_normal.png": "9c95727055440e3bfde50e4cc93c39216d4f45e1794fb82511868685a0c82444",
    "data/textures/mech_pbr/ghoul_orm.ktx2": "3ef138374da77d4f297a3ae543963f597372875833f34aaaf31153c88e6a0eb0",
    "data/textures/mech_pbr/ghoul_orm.png": "fc605ad3bc25db72062dc17b80e07a139e912681e06f9edd88faaba7d9c5406b",
    "data/textures/mech_pbr/highlander_albedo.png": "b902351f92be824936c4143a2148d4ac65d7457149d387d143b26887be9ca7ce",
    "data/textures/mech_pbr/highlander_normal.ktx2": "e066ae9e425da81e5766dceda2702da300e5c7f85fd06f2749d1a6630d7214c6",
    "data/textures/mech_pbr/highlander_normal.png": "8be7ab8481fca2a45f61cc754446b0a00d76e97a8fbf6e935f5459e9f59808cc",
    "data/textures/mech_pbr/highlander_orm.ktx2": "bde6c673619dcf0b3425dd4fbd4ff557bcc8204e0e6318241803fa51046391ee",
    "data/textures/mech_pbr/highlander_orm.png": "e322fb263aff34a9b045258aa802e38f18663b30e6f53081d5b413a6478ab1a3",
    "data/textures/mech_pbr/hollander_albedo.png": "bf0977bad6dfb8cc28b6ad0bb9d6db19e1c5e3795e0aaa354202e55c2bc6d421",
    "data/textures/mech_pbr/hollander_normal.ktx2": "22b845792503979264a7a63b65687948f5e01b02c0c510d9da706a0d107813f5",
    "data/textures/mech_pbr/hollander_normal.png": "7430c49cad78f46a6171269cba5cc74b4d85f024eadfe5d6934214e1228f0fb4",
    "data/textures/mech_pbr/hollander_orm.ktx2": "5c94bd092aa03516ef965bdc7710abe6ce86ce8e993a82efe2f8e1368fe9eb2b",
    "data/textures/mech_pbr/hollander_orm.png": "f0dab1d010f3c6bbc7308e57a256c010365d22089bd6cabf84a309a8fe9cbc74",
    "data/textures/mech_pbr/hunchback_albedo.png": "671528d64b0587c2176dfba072c694bf6e5b7497472c5edbf5db103c089f34ec",
    "data/textures/mech_pbr/hunchback_normal.ktx2": "906ee13c97b7cc1532f620f359d4bc1c19e6ba8d3cd4adf468a591c10f429faf",
    "data/textures/mech_pbr/hunchback_normal.png": "aea4247e16878b8ce2b44467b40e11f0f80c230d778c07a7e15d0ad3178bdd4e",
    "data/textures/mech_pbr/hunchback_orm.ktx2": "0e98c94d764a081d74d97c7254e44b86322b54fa9b8b0a53bde35d91c29769d3",
    "data/textures/mech_pbr/hunchback_orm.png": "24ebc36c9de6985e5758efc0c1eb219086658000f293466cafec33e35daf19a2",
    "data/textures/mech_pbr/jagermech_albedo.png": "45110f3d6392d44976b6fc714e18ab40259dbb38ceee7dd47557dc07a88c0f97",
    "data/textures/mech_pbr/jagermech_normal.ktx2": "71b999a454f6245c4499b91ac2f526b0fe87f83c530453ec3f61e27061509b2f",
    "data/textures/mech_pbr/jagermech_normal.png": "20ce05d5379c57f2187e8d7977bc469790535204fc3b200c4294be728132450a",
    "data/textures/mech_pbr/jagermech_orm.ktx2": "a2d69db12d407edeea9f206d86f4eee0ddf7131caa4b8c7813f2ee7448be29c9",
    "data/textures/mech_pbr/jagermech_orm.png": "58106304e4c76b7d8cce4cc21c763754977d456099e496d91ebd73b6f34174c3",
    "data/textures/mech_pbr/lauhu_albedo.png": "dbd2ac1de72a041ba1162168f88c4f5e1248497d0e1ed43786e0812e8ce31cbe",
    "data/textures/mech_pbr/lauhu_normal.ktx2": "bd5a3601fce5ef68f71af2a31fb35ae08f75608666c302516571218c1aee630f",
    "data/textures/mech_pbr/lauhu_normal.png": "2d2d807e007f116da47bad8111e62e198244922b2fe7c97b9528ee9ce4fee5b4",
    "data/textures/mech_pbr/lauhu_orm.ktx2": "8fbc6e8afcae36886b7d605b525f650e6e2016d5a94477cef4d5e245e49b5546",
    "data/textures/mech_pbr/lauhu_orm.png": "3cafdc331761a90830e491c89715eeab474693d8617ad7883d4039896604e71e",
    "data/textures/mech_pbr/madcat_albedo.png": "fc067745b79bf0309d142e0ef27dadbc402b06fc5eea1b566defaf8e8a4d4582",
    "data/textures/mech_pbr/madcat_normal.ktx2": "979f0ad25a714bf824c6a1dfa263fec3846d4d5c2506a031087eac1050b45761",
    "data/textures/mech_pbr/madcat_normal.png": "1d75c5559feaa750ced69d2d31b4034963c2c3a0d5a5bc7d0eeb507601c69a19",
    "data/textures/mech_pbr/madcat_orm.ktx2": "1ae70184dd5f9c954010ca08049ee3b077cc4c486fb944f0eb38897126444d4a",
    "data/textures/mech_pbr/madcat_orm.png": "236ea30ae0bbfd013619abd0d6946e8ec4bee37def35c7c41b2f2e0b104bf039",
    "data/textures/mech_pbr/menshen_albedo.png": "0b04f3e3f776e3268d1fa06f8272146adf28655d78ddf8e77b46277d6ff788ce",
    "data/textures/mech_pbr/menshen_normal.ktx2": "186f7b6b860a4b21f1eaa5fa8df1d77c50ea968939cb01ecc05f77ecccbd764d",
    "data/textures/mech_pbr/menshen_normal.png": "a8d6ccb0fae39759ebb7a14bf575910b00a60c5bbaa10f7e4d7131e8b2e6f8b7",
    "data/textures/mech_pbr/menshen_orm.ktx2": "d655d9d29baf570d36e33aa6b0c91371c7ecf7572bee1965771b08c738d36204",
    "data/textures/mech_pbr/menshen_orm.png": "2333ee39de7a818c083ca8806afeafe23a70107023228655b7278a51bea6085d",
    "data/textures/mech_pbr/raven_albedo.png": "7fc0fee4c4bcf40f759e5bc93177af9d3e919a0fd88662bf2b3aba9b2ef473e3",
    "data/textures/mech_pbr/raven_normal.ktx2": "99b49418426599961e2e1f12e1f104d8cd3b5cec35269e19c563cd406bf08e24",
    "data/textures/mech_pbr/raven_normal.png": "b37cb9afd840fcc85552c17a15bbdd0d2552873c3c03ea73f0084d9a363ed33c",
    "data/textures/mech_pbr/raven_orm.ktx2": "f597d10a3fe642be7ad5db5e812f8d0cb33564b82ff678865501998edf63344b",
    "data/textures/mech_pbr/raven_orm.png": "bc41a8cc4a2ddfe60b0fc7083a9499403170e772102640e8bbe4d9008b3187ee",
    "data/textures/mech_pbr/ryoken_albedo.png": "1132cd3d7b7522eaaffc738d0cf52e458acc2654d657a7457335c42af30bb7fb",
    "data/textures/mech_pbr/ryoken_normal.ktx2": "571d7c6ba4b05ce28771c910be8e600689807824edb5dbc7d9307436eb94b0fd",
    "data/textures/mech_pbr/ryoken_normal.png": "5c74ed2a2f33e276cdfbfaf4df3d2f3653f1f310004f4eea6da7db874cbabf0b",
    "data/textures/mech_pbr/ryoken_orm.ktx2": "aa0e624bc1f27101299d680cd274c01183d62dcaaeceb63c6bb73294321d7160",
    "data/textures/mech_pbr/ryoken_orm.png": "02161a9a601488369d18ef2319685a110f631208605fa9f078cf3718a86b7282",
    "data/textures/mech_pbr/shadowcat_albedo.png": "1b76538c9ca9727cecdae92ac9cbbb20d1e57b2fdc6a98eb1a4ffc877c05bfbf",
    "data/textures/mech_pbr/shadowcat_normal.ktx2": "37ea309db4e1252e1c2aab85266b39c866c813577658c35a603dee179ae58221",
    "data/textures/mech_pbr/shadowcat_normal.png": "3cf5e4f139f2bffa707b0281ed6b0ce7c3885f4034b148ffde0981f693668d21",
    "data/textures/mech_pbr/shadowcat_orm.ktx2": "167a433c2cbc6f1f911fbcf41730fead9a64371d275955c4ee4692b3c1403137",
    "data/textures/mech_pbr/shadowcat_orm.png": "0c5dccaf7d3b6def8a7b66d822f6984441a64097c786748b2170224382e134f2",
    "data/textures/mech_pbr/shayu_albedo.png": "489cc758b64e0453a5c32035715c0270d04601c5c1afa8b117aed2d0abb7d4e6",
    "data/textures/mech_pbr/shayu_normal.ktx2": "ec25827a4d0fdcd529c7c535ae41539e00203659329e606a83abc04bb665835e",
    "data/textures/mech_pbr/shayu_normal.png": "62346268eb67f5d0c6e6d46531bf1956230e728afdc306fbcb29f9b5e8bf1373",
    "data/textures/mech_pbr/shayu_orm.ktx2": "f424daa727ed0c1371a8c33b1d7bf48f050d730f6f3860001a381be985caa9d1",
    "data/textures/mech_pbr/shayu_orm.png": "deb3b26fed7c3de9578128fb5f52c376d0bc0fbe9ea818c1deecebdf5242b2bb",
    "data/textures/mech_pbr/shootist_albedo.png": "24d930a61d03ec0a867463c3192f660f1e09e53f1325703ee4db1d007ec40668",
    "data/textures/mech_pbr/shootist_normal.ktx2": "1395e7dfa3f7dde8522312056ec1ab0e7d5315426a0f2cf58b210a0eec945922",
    "data/textures/mech_pbr/shootist_normal.png": "64626c54a88a5fc2bc777f9c38e6709d21d806d78f585fddcfd6cd9c0d4a9a23",
    "data/textures/mech_pbr/shootist_orm.ktx2": "4771e0a295a2e5de546f54646f7fd447f0834821da36a1c01ed2f9a34f0bbdf5",
    "data/textures/mech_pbr/shootist_orm.png": "19a5d1c570990f4745f808e1175f29d16b22e4272d3b22a0512fe19d4805b8b9",
    "data/textures/mech_pbr/starslayer_albedo.png": "96330eabe4453ea9d3a01fc3e6175d89de4f96142905fc64c48f3e144ce020bf",
    "data/textures/mech_pbr/starslayer_normal.ktx2": "38e60e3fb7467723bb3091c6d8b2e99279234cbb9a1d2217b4518c2543ccec3b",
    "data/textures/mech_pbr/starslayer_normal.png": "863d4a57c2487ded7a96ba1c07fe530140c671178d078d3ce0dff7f0ad2d0605",
    "data/textures/mech_pbr/starslayer_orm.ktx2": "9aa62b51ce59e65a1a1ade48c33f2447891c7cfc368dfd724c1e9e7d9556e77c",
    "data/textures/mech_pbr/starslayer_orm.png": "25138e3148042a608e024f8eb97928261764e22bb69f34fcd2b47f99d3b3d16f",
    "data/textures/mech_pbr/thor_albedo.png": "2c23064e5515cc3a9b2954a6954f90a80bc627415bac95548a4fd5f75fb0d129",
    "data/textures/mech_pbr/thor_normal.ktx2": "10f65df71dcc78835bdcb7aedd90a05aa56bd94f17145ad7f12bc8b4156b06c8",
    "data/textures/mech_pbr/thor_normal.png": "7b59102c9c3683f4717eabf8825dfc84b62511cc1547429f031bf630434715d3",
    "data/textures/mech_pbr/thor_orm.ktx2": "9658a90f1575f8cd8272e8579cb2cbac72c51354a7abacce4acc11f4e9d2f4fd",
    "data/textures/mech_pbr/thor_orm.png": "97ad8d0b73602651684c042d76a84cffcef6f72ba1eab54248bc0ca5ce726e07",
    "data/textures/mech_pbr/uller_albedo.png": "9ff83561a572a7b3772feb8b9aee602fd63640ae6b719189307a454d35f80ea6",
    "data/textures/mech_pbr/uller_normal.ktx2": "24a059cbf7de6e78538f7f918b00bcbce8eb5b29b38b8bd258f484cc0953b64b",
    "data/textures/mech_pbr/uller_normal.png": "c8bb10a06cc812791a92c0ed0b7de037e58a9d9079a07ecdbbd54660567a5f69",
    "data/textures/mech_pbr/uller_orm.ktx2": "ba71c78684f5df808624204fd10aa6627f5d795a15f2926b4b85dcea8ddc1136",
    "data/textures/mech_pbr/uller_orm.png": "1685eb6016a02c9ebb502800acb0a80364d37141a3b8723f376d1070bc761a36",
    "data/textures/mech_pbr/urbanmech_albedo.png": "ac552b64a116cfc0ddd1ea4f530bffa1af377933ddf3abd775b211e3601c063a",
    "data/textures/mech_pbr/urbanmech_normal.ktx2": "7d033d5a8bfedd4e735414ac8ea53e5daf3e9d86a0aa7bb6dd1c0d93bf30f65f",
    "data/textures/mech_pbr/urbanmech_normal.png": "60ae7883a57d61ba489ad6d14329c0ac7f43baaaafdb012e6466d616745a8250",
    "data/textures/mech_pbr/urbanmech_orm.ktx2": "375680af76445d0b1c74c62b86a3b76339cae0bb8ae85b7c87aacffae31e573b",
    "data/textures/mech_pbr/urbanmech_orm.png": "f468a63e360c77c7b80333a75daa069c37788ccd64cc82c14ea1abe477bc85f9",
    "data/textures/mech_pbr/vutlure_albedo.png": "ea37c5fee8684bbe11da62bc7f054795ca1359ba789a6496edd76f623a03371a",
    "data/textures/mech_pbr/vutlure_normal.ktx2": "cc8ae2e9de55ed106d36b4bd4d41941bf2e24f95a816c09bdb908a66267f5e14",
    "data/textures/mech_pbr/vutlure_normal.png": "6c33dac150e87cd313c97eb4054289dc19303867bed417538e244b47d16b66f7",
    "data/textures/mech_pbr/vutlure_orm.ktx2": "b3e204f38f308b986a3576e6fc26d1ce811206d2c2f6bf98fdc73ca22e150376",
    "data/textures/mech_pbr/vutlure_orm.png": "82565eab322a72e93bbbd96a90292b58f68d3aa831606f88d59a5bcbaf49e401",
    "data/textures/mech_pbr/werewolf_albedo.png": "5cc39f7e351546f620c39314ae7ad70fdbf1fbb5464bf5b0f96b0170af3ccbbf",
    "data/textures/mech_pbr/werewolf_normal.ktx2": "a20989558e9027959f0a3828fe554804f25b3d55dc677e7fb443fe58b8dcb5ba",
    "data/textures/mech_pbr/werewolf_normal.png": "0b12373d00aaababf84ad5bbf08bd2dfaa740001556a1b88dd385fcb3a0dd58d",
    "data/textures/mech_pbr/werewolf_orm.ktx2": "1f9cb48eb1783789456bef8aca96672a9a8d24fe2f557a249c94dadbc05f4f49",
    "data/textures/mech_pbr/werewolf_orm.png": "ec5fec763f3222f6008742326c20e80058d5727589942fd9bea4ab428c397326",
    "data/textures/mech_pbr/wolfhound_albedo.png": "a72aff0067eeae2c7b83122d6c64f7333f0f3a25d09fb300845e9fa43ef35166",
    "data/textures/mech_pbr/wolfhound_normal.ktx2": "9e3aa422742dd94497ebfd29a81e69629537b50effa60a440527326eb589a561",
    "data/textures/mech_pbr/wolfhound_normal.png": "009a095f4e9af3645b81d3def80408b3d14d6514118fc6cba889cd8e3f7d7ef9",
    "data/textures/mech_pbr/wolfhound_orm.ktx2": "fc4876d6e3a0dc223da156b78a1914e786d24a8fb0dd684b99742e1727c26f8f",
    "data/textures/mech_pbr/wolfhound_orm.png": "88cbead3b742ba485a5e9ca7021cdcccc6399c0f036130f88f1c6c7f4e387baa",
    "data/textures/mech_pbr/zeus_albedo.png": "b9f13c91d18117dbddd0d098f9109bc5e40ac27fa8bd41184a9f4ecf6462901c",
    "data/textures/mech_pbr/zeus_normal.ktx2": "b673ecc4d0d914e37a60d39221933dca51cf063d305d22083f3adc3a5f135f28",
    "data/textures/mech_pbr/zeus_normal.png": "5846cb06b17ed68607bffcf7e0a0cd33a7d6813b55ab248762ca32c78d1cdb34",
    "data/textures/mech_pbr/zeus_orm.ktx2": "cdafc756e309f0f06031f4a16c8f91df7cd684aa8a47584336f3d06c4d8a6576",
    "data/textures/mech_pbr/zeus_orm.png": "2b91626576c401fbf12ea26955e15b18c4a8d42f2174c64b857ce8607f1e1a9d",
    "data/tgl/quonset.ini": "71fb4ec98d1cf2149e1ef6dfc5568de1442da54f010acac3329de9ceaa64fce2",
    "data/tgl/QuonsetGLB.glb": "f11603d9d313367db94e2334739c9c595714336ddab41f92035d7ee26a22dff1",
    "data/tgl/QuonsetGLB.mcasset.json": "bcc095d0a858de35445a35c8fc24024da1dcd050803156a028228cdc216f199f",
    "data/tgl/hangar.ini": "91d5dee7aa635fb3dfb4d352dcbe52d663f0b0730446f5cb8d6eb11adcee6ae3",
    "data/tgl/hangar.ini.cookstamp": "7190167cb86787999627e64504ce2196ec5e93f83e26844427db2798efea60f1",
    "data/tgl/quonset.ini.cookstamp": "6258c7713cdcdb61701fba3e9d1840c0792f5aa4bd8583febf550a7517ea48d0",
    "data/tgl/HangarGLB.glb": "77ec151d3e4d5977a30506db8dd22b97c1daddb3acf742356b743ab2fcba3af3",
    "data/tgl/HangarGLB.mcasset.json": "45a8de06ad0100929615c66525d34f34ae4beecb748f1d0ecfb4c2a93a71ac02",
}


def prune_retired_seed_payload(target_dir, preserve=()):
    preserved = {str(p).replace("\\", "/").strip("/").casefold() for p in preserve}
    for rel, digest in RETIRED_SEED_PAYLOAD.items():
        key = rel.replace("\\", "/").strip("/").casefold()
        if any(key == p or key.startswith(p + "/") for p in preserved):
            log(f"preserved retired seed payload: {rel}")
            continue
        p = Path(target_dir) / rel
        if p.is_file() and sha256_file(p) == digest:
            p.unlink()
            log(f"pruned retired seed payload: {rel}")


# Renderer config JSON read directly off "data/<name>.json" relative paths by
# GameOS/gameos (terrain_material_lib.cpp, visual_tuning_profile.cpp) -- both
# game AND editor installs load these (shared renderer core), so they must
# ship even though neither is a build output. Missing file = silent no-op at
# runtime (both readers tolerate absence), but shipping without them silently
# drops per-mission/material tuning that was authored assuming the file loads.
GAME_DATA_PAYLOAD = [
    "data/textures/moon_detail_normal.ktx2",         # Existing moon inventory normal, selected by mc2_13.biome.fit.
    "CREDITS.txt",
    "THIRD_PARTY_NOTICES.txt",
    "data/art/asset_sizes.csv",
    *[f"data/art/gui/mcl_cm_op{op}_{view}.tga"
      for op, count in ((1, 6), (2, 3), (3, 4)) for view in range(1, count + 1)],
    "data/art/gui/mcl_cm_5.png",
    "data/art/gui/mcl_cm_5.tga",
    "data/art/gui/mcl_mn_4.png",
    "data/art/gui/mcl_mn_4.tga",
    "data/terrain_materials.json",
    "data/missions/mc2_13.biome.fit",
    "data/visual_tuning.json",
    # ImGui UI fonts (GuiRuntime resolves exe/data/fonts/ui first); without them every defs UI text falls back
    # to the ImGui bitmap default font (blocky, low-res at 4K).
    "data/fonts/ui/Roboto-Medium.ttf",
    "data/fonts/ui/Cousine-Regular.ttf",
    "data/fonts/ui/LiberationSansNarrow-Bold.ttf",
    "data/fonts/ui/LiberationSans-Regular.ttf",
    "data/fonts/ui/LiberationSansNarrow-Regular.ttf",
    # Native vegetation packs these tracked cards into its 4x2 atlas at load.
    "data/vegetation/cards/grass_01.png",
    "data/vegetation/cards/grass_02.png",
    "data/vegetation/cards/grass_03.png",
    "data/vegetation/cards/grass_04.png",
    "data/vegetation/cards/bush_round.png",
    "data/vegetation/cards/bush_heath.png",
    "data/vegetation/cards/sapling_leaf.png",
    "data/vegetation/cards/sapling_pine.png",
    "data/vegetation/cooked/grass_01.ktx2",
    "data/vegetation/cooked/grass_02.ktx2",
    "data/vegetation/cooked/grass_03.ktx2",
    "data/vegetation/cooked/grass_04.ktx2",
    "data/vegetation/cooked/bush_round.ktx2",
    "data/vegetation/cooked/bush_heath.ktx2",
    "data/vegetation/cooked/sapling_leaf.ktx2",
    "data/vegetation/cooked/sapling_pine.ktx2",
    "data/vegetation/schemas/grasslands.json",
    "data/vegetation/schemas/snow.json",
    "data/vegetation/schemas/desert.json",
    "data/vegetation/schemas/swamp.json",


]

# TERRAIN-MATERIAL-TEXTURES-1: per-layer BC7 sRGB albedo KTX2s read off
# "data/terrain_layers/<channel>_albedo.ktx2" by the LOD-chunk albedo-array
# loader (none today: the GL consumer was retired; a Vulkan one is on the backlog).
# Loose cooked sidecars (gitignored; regenerate via
# tools/mc2texcook/cook_terrain_layers.py -- must be mip-complete BC7 sRGB).
# OPTIONAL at deploy time: a missing layer file only logs and skips (the
# engine fail-softs to the legacy tint path), matching the runtime contract.
TERRAIN_LAYER_PAYLOAD = [
    "data/terrain_layers/rock_albedo.ktx2",
    "data/terrain_layers/grass_albedo.ktx2",
    "data/terrain_layers/dirt_albedo.ktx2",
    "data/terrain_layers/concrete_albedo.ktx2",
    "data/terrain_layers/snow_albedo.ktx2",
    "data/terrain_layers/cliff_albedo.ktx2",
    "data/terrain_layers/concrete_detail_pad.ktx2",    # CONCRETE-ROAD-PBR-DETAIL (cook_concrete_detail.py)
    "data/terrain_layers/concrete_detail_road.ktx2",
    "data/terrain_layers/cliff_rock_ah.ktx2",          # TERRAIN-CLIFF-ROCK (cook_cliff_rock.py --select riverbed)
    "data/terrain_layers/cliff_rock_nra.ktx2",
    "data/terrain_layers/grass_turf_normal.ktx2",      # GRASS-GROUND-NORMAL (cook_grass_turf.py), detail-normal layer 1
    "data/terrain_layers/dirt_sand_normal.ktx2",       # TERRAIN-NORMAL-UNIFORM (cook_dirt_sand.py), detail-normal layer 2
    "data/terrain_layers/forest_floor_normal.ktx2",    # TERRAIN-NORMAL-UNIFORM (cook_forest_floor.py), optional detail-normal layer 6
]

# The two concrete/road detail KTX2s are generated, not tracked (about 1.4 MB each): cooked on demand at deploy time when absent.
CONCRETE_DETAIL_COOK = "tools/mc2texcook/cook_concrete_detail.py"
CONCRETE_DETAIL_SRC = "G:/mc2-assets/pbr/concrete-road"


ALLOW_STOCK_TREES = False   # --allow-stock-trees: a failed cook downgrades to a WARN and the install ships stock data


def cook_failed(msg):
    """A deploy cook failed: exit non-zero (never ship stock data silently) unless --allow-stock-trees was passed."""
    if ALLOW_STOCK_TREES:
        log("[DEPLOY] WARN: " + msg + " (continuing: --allow-stock-trees)")
        return
    fail(msg + " (pass --allow-stock-trees to ship stock data deliberately)")


def ensure_concrete_detail(src_root, cooker=subprocess.run):
    """Cook data/terrain_layers/concrete_detail_{pad,road}.ktx2 when missing (soft: the engine fail-softs without them)."""
    out = os.path.join(src_root, "data", "terrain_layers")
    if all(os.path.isfile(os.path.join(src_root, r)) for r in TERRAIN_LAYER_PAYLOAD if "concrete_detail" in r):
        return
    if not os.path.isdir(CONCRETE_DETAIL_SRC):
        log(f"[DEPLOY] WARN: concrete detail KTX2s absent and no cook source at {CONCRETE_DETAIL_SRC}: "
            "concrete/road detail will be missing from this install")
        return
    os.makedirs(out, exist_ok=True)
    log("concrete detail KTX2s absent: cooking them (about 15 s)")
    try:
        r = cooker([sys.executable, os.path.join(src_root, CONCRETE_DETAIL_COOK), "--src", CONCRETE_DETAIL_SRC, "--out", out],
                   capture_output=True, text=True, timeout=300, env=dict(os.environ, OMP_NUM_THREADS="1"), **console_child.child_kwargs(redirected=True))
    except subprocess.TimeoutExpired:
        cook_failed("concrete detail cook timed out after 300 s")
        return
    if r.returncode:
        cook_failed("concrete detail cook failed rc=%s: %s" % (r.returncode, (r.stdout + r.stderr).strip()[-300:]))


# TERRAIN-CLIFF-ROCK: the cliff rock pair is generated too (CC0 Poly Haven sets; licences in tools/mc2texcook/cliff_rock_provenance.json).
CLIFF_ROCK_COOK = "tools/mc2texcook/cook_cliff_rock.py"
CLIFF_ROCK_SRC = "C:/Users/Joe/Downloads/GameAsset/Terrain"
CLIFF_ROCK_SET = "riverbed"


def ensure_cliff_rock(src_root, cooker=subprocess.run):
    """Cook data/terrain_layers/cliff_rock_{ah,nra}.ktx2 when missing (soft: steep faces keep the base look without them)."""
    out = os.path.join(src_root, "data", "terrain_layers")
    if all(os.path.isfile(os.path.join(src_root, r)) for r in TERRAIN_LAYER_PAYLOAD if "cliff_rock" in r):
        return
    if not os.path.isdir(CLIFF_ROCK_SRC):
        log(f"[DEPLOY] WARN: cliff rock KTX2s absent and no cook source at {CLIFF_ROCK_SRC}: cliffs keep the base look")
        return
    os.makedirs(out, exist_ok=True)
    log("cliff rock KTX2s absent: cooking them")
    try:
        r = cooker([sys.executable, os.path.join(src_root, CLIFF_ROCK_COOK), "--src", CLIFF_ROCK_SRC, "--out", out,
                    "--sets", CLIFF_ROCK_SET, "--select", CLIFF_ROCK_SET],
                   capture_output=True, text=True, timeout=300, env=dict(os.environ, OMP_NUM_THREADS="1"), **console_child.child_kwargs(redirected=True))
    except subprocess.TimeoutExpired:
        cook_failed("cliff rock cook timed out after 300 s")
        return
    if r.returncode:
        cook_failed("cliff rock cook failed rc=%s: %s" % (r.returncode, (r.stdout + r.stderr).strip()[-300:]))

# STOCK-TREE-UPSCALE: 1024 stock tree atlases, generated from the stock 128 TGAs (tools/tree_retexture/gen_stock_trees.py,
# pins in data/kit/tree-upscale.json; about 1.4 MB each, so not tracked). They land as loose same-stem sidecars that the
# texture loader prefers over the stock 128 KTX2; a missing one renders stock.
TREE_UPSCALE_COOK = "tools/tree_retexture/gen_stock_trees.py"
TREE_UPSCALE_PINS = "data/kit/tree-upscale.json"


def tree_upscale_payload(src_root):
    with open(os.path.join(src_root, TREE_UPSCALE_PINS), encoding="utf-8") as f:
        pins = json.load(f)
    return {f"data/tgl/128/{stem}.ktx2": row["ktx2_sha256"] for stem, row in sorted(pins.items())}


from smoke_lib.paths import ASSET_STORE_ROOT, INSTALL_ROOT, LEAN_SEED_ROOT

def ensure_tree_upscale(src_root, cooker=subprocess.run):
    """Generate the upscaled tree sidecars when absent or not the pinned bytes (soft: stock trees render without them)."""
    rows = tree_upscale_payload(src_root)

    def current():
        def sha(rel):
            with open(os.path.join(src_root, rel), "rb") as f:
                return hashlib.sha256(f.read()).hexdigest()
        return all(os.path.isfile(os.path.join(src_root, r)) and sha(r) == want for r, want in rows.items())
    if current():
        return
    # Lane worktrees can reuse published bytes only when they match the checked-in pins.
    # This avoids a redundant multi-minute cook inside the 300 s enumeration phase.
    import asset_parity
    references = (Path(os.environ.get('MC2_LEAN_SEED', LEAN_SEED_ROOT)),
                  asset_parity.default_ref())
    for rel, want in rows.items():
        dst = Path(src_root) / rel
        if dst.is_file() and sha256_file(dst) == want:
            continue
        for reference in references:
            source = reference / rel
            if source.is_file() and sha256_file(source) == want:
                asset_parity._copy(source, dst)
                if sha256_file(dst) != want:
                    fail(f'pinned tree copy hash mismatch: {rel}')
                log(f'reused pinned tree: {rel}')
                break
    if current():
        return
    log("tree upscale KTX2s absent or stale: generating them (a few minutes)")
    try:
        r = cooker([sys.executable, os.path.join(src_root, TREE_UPSCALE_COOK), "--out", os.path.join(src_root, "data", "tgl", "128")],
                   capture_output=True, text=True, timeout=1800, env=dict(os.environ, OMP_NUM_THREADS="1"), **console_child.child_kwargs(redirected=True))
    except subprocess.TimeoutExpired:
        log("[DEPLOY] WARN: tree upscale cook timed out after 1800 s (stock trees ship)")
        return
    if r.returncode or not current():
        log("[DEPLOY] WARN: tree upscale cook failed or does not match the pins (stock trees ship): " + (r.stdout + r.stderr).strip()[-300:])
        for rel in rows:
            p = os.path.join(src_root, rel)
            if os.path.isfile(p):
                os.remove(p)


# ACTOR-PBR-MIPS: stock vehicle normal + ORM PNGs get same-stem BC7 KTX2 sidecars with a full mip chain, cooked at deploy
# (tools/content_pipeline/cook_actor_pbr.py, pins in data/kit/actor-pbr-pins.json; generated, not tracked). The loader prefers the
# sidecar over the PNG; a missing or stale one is removed and the PNG renders as before.
ACTOR_PBR_COOK = "tools/content_pipeline/cook_actor_pbr.py"
ACTOR_PBR_PINS = "data/kit/actor-pbr-pins.json"


def actor_pbr_payload(src_root):
    path = os.path.join(src_root, ACTOR_PBR_PINS)
    if not os.path.isfile(path):
        return {}
    with open(path, encoding="utf-8") as f:
        return {rel: row["ktx2_sha256"] for rel, row in sorted(json.load(f).items())}


def ensure_actor_pbr(src_root, cooker=subprocess.run):
    """Generate the actor PBR sidecars when absent or not the pinned bytes (soft: PNGs render without them)."""
    rows = actor_pbr_payload(src_root)
    if not rows:
        return

    def current():
        return all(os.path.isfile(os.path.join(src_root, r)) and sha256_file(os.path.join(src_root, r)) == want
                   for r, want in rows.items())
    if current():
        return
    import asset_parity
    references = (Path(os.environ.get('MC2_LEAN_SEED', LEAN_SEED_ROOT)), asset_parity.default_ref())
    for rel, want in rows.items():
        dst = Path(src_root) / rel
        if dst.is_file() and sha256_file(dst) == want:
            continue
        for reference in references:
            source = reference / rel
            if source.is_file() and sha256_file(source) == want:
                asset_parity._copy(source, dst)
                break
    if current():
        return
    log("actor PBR KTX2s absent or stale: generating them (tens of minutes cold)")
    try:
        # Outside the bounded deploy clock: a cold cook takes tens of minutes and has its own 5400 s timeout.
        with phase_paused():
            r = cooker([sys.executable, os.path.join(src_root, ACTOR_PBR_COOK), "--root", src_root, "--jobs", "4"],
                       capture_output=True, text=True, timeout=5400, env=dict(os.environ, OMP_NUM_THREADS="1"), **console_child.child_kwargs(redirected=True))
    except subprocess.TimeoutExpired:
        log("[DEPLOY] WARN: actor PBR cook timed out after 5400 s (PNG maps ship)")
        return
    if r.returncode or not current():
        log("[DEPLOY] WARN: actor PBR cook failed or does not match the pins (PNG maps ship): " + (r.stdout + r.stderr).strip()[-300:])
        for rel in rows:
            p = os.path.join(src_root, rel)
            if os.path.isfile(p):
                os.remove(p)


# GRASS-GROUND-NORMAL: the top-down turf normal that replaces detail-normal layer 1 (grass), generated from CC0 Poly Haven
# sparse_grass + dry_ground_01 (owner download 2026-10-23). Recooked when absent or older than its cooker.
GRASS_TURF_COOK = "tools/mc2texcook/cook_grass_turf.py"
GRASS_TURF_SRC = "G:/mc2-assets/pbr/dl-2026-10-23/ground"  # drive-path: ok PC-only cook source; absent on the laptop (soft skip, the laptop gets the published ktx2)
GRASS_TURF_REL = "data/terrain_layers/grass_turf_normal.ktx2"


def _ensure_cooked_normal(src_root, cook, rel, what, cooker, src=None):
    """Cook a data/terrain_layers detail normal when missing or stale (soft: the slot keeps its base layer without it)."""
    dst = os.path.join(src_root, rel)
    script = os.path.join(src_root, cook)
    if os.path.isfile(dst) and (not os.path.isfile(script) or os.path.getmtime(dst) >= os.path.getmtime(script)):
        return
    src = src or GRASS_TURF_SRC   # each cook is gated on its OWN source dir
    if not os.path.isdir(src):
        log(f"[DEPLOY] WARN: {what} absent and no cook source at {src}: the slot keeps the base layer")
        return
    log(f"{what} absent or stale: cooking it")
    try:
        r = cooker([sys.executable, script, "--src", src, "--out", os.path.dirname(dst)],
                   capture_output=True, text=True, timeout=300, env=dict(os.environ, OMP_NUM_THREADS="1"), **console_child.child_kwargs(redirected=True))
    except subprocess.TimeoutExpired:
        log(f"[DEPLOY] WARN: {what} cook timed out after 300 s (skipped)")
        return
    if r.returncode:
        log(f"[DEPLOY] WARN: {what} cook failed (skipped): " + (r.stdout + r.stderr).strip()[-300:])


def ensure_grass_turf(src_root, cooker=subprocess.run):
    _ensure_cooked_normal(src_root, GRASS_TURF_COOK, GRASS_TURF_REL, "grass turf normal", cooker)


# TERRAIN-NORMAL-UNIFORM: the sand/gravel normal that replaces detail-normal layer 2 (dirt), from CC0 Poly Haven sand_01 + dry_ground_01.
DIRT_SAND_COOK = "tools/mc2texcook/cook_dirt_sand.py"
DIRT_SAND_REL = "data/terrain_layers/dirt_sand_normal.ktx2"


def ensure_dirt_sand(src_root, cooker=subprocess.run):
    _ensure_cooked_normal(src_root, DIRT_SAND_COOK, DIRT_SAND_REL, "dirt sand normal", cooker)


# Under-forest ground (detail-normal array layer 6) from CC0 Poly Haven forest_ground_04.
FOREST_FLOOR_COOK = "tools/mc2texcook/cook_forest_floor.py"
FOREST_FLOOR_REL = "data/terrain_layers/forest_floor_normal.ktx2"


FOREST_FLOOR_SRC = "E:/mc2-ref/terrain-normals-2026-11-24"  # drive-path: ok PC-only cook source; mirrors cook_forest_floor.SOURCE_DIR


def cook_mod_visual_heights(src_root, target):
    """Reuse the offline sidecar cook for installed mod maps; retain authored bakes."""
    cook = Path(src_root) / "tools/terrain_beautify/visual_heightfield.py"
    paks = sorted((Path(target) / "mods").glob("*/data/missions/*.pak"))
    if not paks:
        return
    sys.path.insert(0, str(cook.parent))
    from mission_terrain_analyzer import locate_mapdata, read_packets, extract_layers
    from height_sig import height_sig
    for pak in paks:
        sidecar = pak.parent / (pak.stem + ".beauty") / "visual_height_4x.r32"
        md = locate_mapdata(read_packets(pak))
        if md is None:
            log(f"mod visual height: {pak.name} has no terrain MapData; skipping")
            continue
        _, side, blocks = md
        sig = sidecar.with_name("gameplay_height.sig")
        expected_sig = height_sig(extract_layers(side, blocks, 0.0)["elev"])
        if (sidecar.is_file() and sidecar.stat().st_size == ((side - 1) * 4 + 1) ** 2 * 4
                and (not sig.is_file() or sig.read_text(encoding="ascii").strip() == expected_sig)):
            continue
        try:
            subprocess.run([sys.executable, str(cook), pak.stem, "--missions-dir", str(pak.parent),
                        "--out", str(pak.parent), "--allow-nonstock", "--reauth", "--reauth-passes", "4", "--cliff-drift", "56",
                        "--cliff-melt-passes", "4"], check=True, **console_child.child_kwargs())
        except subprocess.CalledProcessError as exc:
            log(f"mod visual height: {pak.name} cook failed (rc={exc.returncode}); skipping")


def ensure_forest_floor(src_root, cooker=subprocess.run):
    _ensure_cooked_normal(src_root, FOREST_FLOOR_COOK, FOREST_FLOOR_REL, "forest floor normal", cooker, FOREST_FLOOR_SRC)

# EXAMPLE-MODS-1: worked example checkbox mods, source-tracked under
# examples/mods/. Ship to <target>/mods/<id>/ so the launcher enumerates them
# as add-on checkboxes (a mods/ folder needs mod.json AND a data/ overlay dir
# to count as a stackable add-on; data-less examples like sample_bad_mod /
# core_unlocks are lint/doc fixtures and are NOT shipped).
EXAMPLE_MODS_SRC = "examples/mods"

# UNIT/EQUIPMENT-DEFS deploy: generated definition sidecars consumed by the
# source-flip loaders (MC2_UNIT_DEFINITION_SOURCE=generated reads
# data/defs/units/core/..., MC2_EQUIPMENT_DEFINITION_SOURCE=generated reads
# data/defs/equipment/core/...). NOT committed in the repo -- regenerated at
# deploy time by the offline converters (tools/unit_converter/) from stock
# mc2srcdata, into <build64>/generated_defs/, then shipped like any payload.
# Converters are deterministic (tested); stock srcdata is the single source of
# truth, so deploy-time generation cannot drift from the converter the way a
# committed snapshot could. Soft-skips (with a loud log) when mc2srcdata or
# python is unavailable, since the engine defaults to the legacy path anyway.
GENERATED_DEFS = [
    # (converter script, extra args, out subdir, target rel root)
    ("tools/unit_converter/unit_converter.py", ["--batch"], "units",
     "data/defs/units"),
    ("tools/unit_converter/equipment_converter.py", [], "equipment",
     "data/defs/equipment"),
]

def find_srcdata(src_root):
    """Locate the stock mc2srcdata root (untracked, lives at the main repo
    root, not inside worktrees). Candidates, first hit wins:
    $MC2_SRCDATA, <src_root>/mc2srcdata, <src_root>/../../../mc2srcdata
    (worktree layout: .claude/worktrees/<name>), A:/Games/mc2-opengl-src/mc2srcdata."""
    cands = []
    env = os.environ.get("MC2_SRCDATA")
    if env:
        cands.append(env)
    cands.append(os.path.join(src_root, "mc2srcdata"))
    cands.append(os.path.abspath(os.path.join(src_root, "..", "..", "..",
                                              "mc2srcdata")))
    cands.append("A:/Games/mc2-opengl-src/mc2srcdata")
    for c in cands:
        if os.path.isdir(c):
            return os.path.abspath(c)
    return None


def generate_defs_payload(src_root, build64_root):
    """Run the offline converters --batch into <build64>/generated_defs/ and
    return payload items [(abs, rel, 'support')]. Empty list (loud log) when
    srcdata is missing or a converter fails -- the engine default is the legacy
    path, so a defs-less install still works; only the generated-source toggle
    goes inert."""
    srcdata = find_srcdata(src_root)
    if not srcdata:
        log("generated defs SKIPPED: mc2srcdata not found (set MC2_SRCDATA); "
            "the launcher's 'Data-driven definitions' toggle will be inert")
        return []
    gen_root = os.path.join(build64_root or os.path.join(src_root, "build64"),
                            "generated_defs")
    items = []
    for script, extra, sub, rel_root in GENERATED_DEFS:
        out_dir = os.path.join(gen_root, sub)
        cmd = [sys.executable, os.path.join(src_root, script),
               "--srcdata", srcdata, "--out", out_dir] + extra
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600, **console_child.child_kwargs(redirected=True))
        if r.returncode != 0:
            log(f"generated defs SKIPPED: {script} failed rc={r.returncode}\n"
                f"{(r.stderr or r.stdout or '').strip()[:2000]}")
            return []
        n = 0
        for dirpath, _dirs, files in os.walk(out_dir):
            for fn in sorted(files):
                sp = os.path.join(dirpath, fn)
                rel = rel_root + "/" + os.path.relpath(sp, out_dir).replace(os.sep, "/")
                items.append((sp, rel, "support"))
                n += 1
        log(f"generated defs: {script} -> {n} files under {rel_root}/")
    return items


def example_mods_payload(src_root):
    """Enumerate shippable example mods (mod.json + data/ overlay) from
    examples/mods/ -> <target>/mods/<id>/ payload items."""
    root = os.path.join(src_root, EXAMPLE_MODS_SRC)
    items = []
    if not os.path.isdir(root):
        return items
    for entry in sorted(os.listdir(root)):
        mod_dir = os.path.join(root, entry)
        if not os.path.isdir(mod_dir):
            continue
        if not os.path.isfile(os.path.join(mod_dir, "mod.json")):
            continue
        if not os.path.isdir(os.path.join(mod_dir, "data")):
            log(f"example mod '{entry}' has no data/ overlay (lint/doc fixture) -- not shipped")
            continue
        n = 0
        for dirpath, _dirs, files in os.walk(mod_dir):
            for fn in sorted(files):
                sp = os.path.join(dirpath, fn)
                rel = "mods/" + entry + "/" + os.path.relpath(sp, mod_dir).replace(os.sep, "/")
                items.append((sp, rel, "support"))
                n += 1
        log(f"example mod '{entry}': {n} files -> mods/{entry}/")
    return items


def magic_brain_145(source):
    """FixABL's 1.45 call migration, preserving bytes outside the changed calls.

    The original VB6 binary's UTF-16 replacement table (0x427c..0x47d0)
    redirects coreAttack/coreAttackTactic and drops obsolete state/range args.
    Parse arguments rather than matching its hard-coded variable spellings.
    """
    text = source.decode('latin1')
    pattern = r'//[^\r\n]*|/\*.*?\*/|"[^"\r\n]*"|\b(coreAttackTactic|coreAttack|magicAttack|coreGuard|corePatrolDelayed|corePatrol|coreEscort|coreWait)\s*\('
    edits = []
    for match in re.finditer(pattern, text, re.I | re.S):
        # Commented-out examples must stay comments, including block comments.
        if match.group(1) is None:
            continue
        start = match.end()
        depth, args, arg_start = 1, [], start
        for pos in range(start, len(text)):
            ch = text[pos]
            if ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
                if depth == 0:
                    args.append(text[arg_start:pos].strip())
                    break
            elif ch == ',' and depth == 1:
                args.append(text[arg_start:pos].strip())
                arg_start = pos + 1
        else:
            raise ValueError('unterminated ABL call')
        name = match.group(1).lower()
        if name in ('coreattack', 'coreattacktactic', 'magicattack'):
            replacement = 'magicAttack(' + args[0] + ')'
        else:
            arity = {'coreguard': 2, 'corepatrol': 2, 'corepatroldelayed': 3,
                     'coreescort': 1, 'corewait': 2}[name]
            if len(args) <= arity:
                continue
            replacement = match.group(1) + '(' + ', '.join(args[:arity]) + ')'
        edits.append((match.start(), pos + 1, replacement))
    for start, end, replacement in reversed(edits):
        text = text[:start] + replacement + text[end:]
    return text.encode('latin1')


def magic_brain_payload(src_root, target):
    """Build the rule-selected profile from stock FST brains, never loose mod brains."""
    sys.path.insert(0, str(src_root))
    from tools.mc2x_import.fst import fst_entries, decompress_entry
    profile = Path(src_root) / "mods/magic-brain-1.45"
    items = [(str(p), p.relative_to(src_root).as_posix(), "support")
             for p in profile.rglob("*") if p.is_file()]
    generated = Path(src_root) / "build/magic-brain-1.45"
    for off, comp, real, name, is_lz, data in fst_entries(str(Path(target) / "mission.fst")):
        key = name.lower().replace('\\', '/')
        if not key.startswith("data/missions/warriors/") or not key.endswith((".abl", ".abi")):
            continue
        source = decompress_entry(off, comp, real, is_lz, data)
        fixed = magic_brain_145(source)
        if fixed == source:
            continue
        dest = generated / key
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(fixed)
        items.append((str(dest), "mods/magic-brain-1.45/" + key, "support"))
    return items


FFMPEG_DLLS = [
    "avcodec-61.dll",
    "avformat-61.dll",
    "avutil-59.dll",
    "swscale-8.dll",
    "swresample-5.dll",
]
RUNTIME_DLLS = Path(__file__).with_name("release-dlls.txt").read_text().splitlines()


def log(msg):
    print(f"[deploy_payload] {msg}", flush=True)


_NINJA_SPV = re.compile(r"shaders[\\/]vulkan[\\/]([A-Za-z0-9_.\-]+\.spv)")


def baked_spv_files(b64):
    """The build's own SPIR-V outputs under <b64>/shaders/vulkan. A Ninja tree names every output in build.ninja; a .spv
    left in the directory by a retired CMake rule is not an output and never ships (W129: 20 dead .spv, 2026-10-09..11-04,
    shipped to v0.4 on every PC deploy). A tree whose build.ninja names no .spv (VS tree, test fixture) falls back to the
    directory listing."""
    spv_dir = Path(b64) / "shaders" / "vulkan"
    files = sorted(p for p in spv_dir.glob("*.spv") if p.is_file()) if spv_dir.is_dir() else []
    ninja = Path(b64) / "build.ninja"
    produced = set(_NINJA_SPV.findall(ninja.read_text(encoding="utf-8", errors="replace"))) if ninja.is_file() else set()
    if not produced:
        return files
    stale = [p.name for p in files if p.name not in produced]
    if stale:
        log(f"skipped {len(stale)} stale .spv no build rule produces: {', '.join(stale)}")
    return [p for p in files if p.name in produced]


_last_failure = []


def _reprint_failure_last():
    # Cleanup logs (lease release) print after fail(); repeat the refusal as the LAST line so `| tail` shows it.
    if _last_failure:
        print(f"[deploy_payload] FAIL (exit {_last_failure[1]}): {_last_failure[0].splitlines()[0]}", file=sys.stderr)


atexit.register(_reprint_failure_last)


def fail(msg, code=1):
    print(f"[deploy_payload] FAIL: {msg}", file=sys.stderr)
    _last_failure[:] = [msg, code]
    sys.exit(code)


def disk_space(path, need_bytes, warn_bytes=0, label=None, quiet=False):
    """Measure the destination volume before a write can truncate its files."""
    if need_bytes < 0 or warn_bytes < 0:
        raise ValueError("disk-space thresholds must be nonnegative")
    probe = os.path.abspath(path)
    while not os.path.exists(probe):
        parent = os.path.dirname(probe)
        if parent == probe:
            raise OSError(f"no existing ancestor of {path}")
        probe = parent
    if not os.path.isdir(probe):
        probe = os.path.dirname(probe)
    usage = shutil.disk_usage(probe)
    rc = 3 if usage.free < need_bytes else 1 if usage.free < need_bytes + warn_bytes else 0
    verdict = {0: "OK", 1: "LOW", 3: "CRITICAL"}[rc]
    if rc or not quiet:
        print(f"[DISK_GUARD] {verdict}: {usage.free} bytes free, {need_bytes} needed "
              f"for {label or 'write'} on {probe}", file=sys.stderr if rc == 3 else sys.stdout)
    return rc, {"verdict": verdict, "path": probe, "free_bytes": usage.free,
                "need_bytes": need_bytes, "warn_margin_bytes": warn_bytes, "op": label}


def disk_space_main(argv=None):
    """Read-only entry point shared by release, isolated deploy and capture callers."""
    import json
    import math
    parser = argparse.ArgumentParser(description="Check free space before writing")
    parser.add_argument("--path", default=repo_root())
    need = parser.add_mutually_exclusive_group(required=True)
    need.add_argument("--need-bytes", type=int)
    need.add_argument("--need-gb", type=float)
    parser.add_argument("--warn-gb", type=float, default=5.0)
    parser.add_argument("--label")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    try:
        if not math.isfinite(args.warn_gb) or (args.need_gb is not None and not math.isfinite(args.need_gb)):
            raise ValueError("disk-space thresholds must be finite")
        if args.warn_gb < 0 or (args.need_gb is not None and args.need_gb < 0):
            raise ValueError("disk-space thresholds must be nonnegative")
        required = args.need_bytes if args.need_bytes is not None else int(args.need_gb * 1024 ** 3)
        rc, facts = disk_space(args.path, required, int(args.warn_gb * 1024 ** 3), args.label, args.quiet)
        if args.json:
            print(json.dumps(facts))
        return rc
    except (OSError, ValueError) as exc:
        print(f"[DISK_GUARD] ERROR: {exc}", file=sys.stderr)
        return 2


def check_free_space_for_payload(items, target):
    """Require the measured payload plus headroom, with a 2 GiB floor."""
    if os.environ.get("MC2_DEPLOY_SKIP_DISK_GUARD") == "1":
        log("WARNING: MC2_DEPLOY_SKIP_DISK_GUARD=1 -- free-space preflight BYPASSED BY REQUEST.")
        return
    total = 0
    for src, _rel, _kind in items:
        try:
            total += os.path.getsize(src)
        except OSError:
            pass  # Optional payload entries need no space when absent.
    required = max(2 * 1024 ** 3, (total * 115 + 99) // 100)
    try:
        rc, _facts = disk_space(target, required, label="deploy payload")
    except (OSError, ValueError) as exc:
        fail(f"cannot measure deploy free space: {exc} -- nothing was written")
    if rc == 3:
        fail("insufficient free space for deploy payload -- nothing was written")


def check_hardlink_isolation(items, target):
    """LINK-GUARD-1: refuse to deploy into an install that is somebody else's.

    Motivated by the 2026-07-30 shared-install incident. Four deploy dirs under
    A:/Games/mc2-opengl shared one hardlinked mc2.exe, one shader tree, and --
    as the guard's first real run revealed -- ~1530 files each, including
    "Mission Editor.exe", every DLL, .deployed_manifest.csv, and
    .mc2_deploy_lane.json (the lane lock itself). deploy() below copies with
    shutil.copyfile, which opens the destination "wb": that TRUNCATES AND
    REWRITES THE SHARED INODE rather than replacing the directory entry, so a
    deploy into one of those dirs rewrote the other three in place.

    Nothing already in this script could see it. check_locks() asks whether a
    PROCESS holds the target; the lane lock and smoke lease ask whether another
    lane holds THIS PATH; DEPLOY_ALLOWLIST asks whether the path is canonical
    (mc2-win64-v0.4d-rc1 is BOTH allowlisted AND in the shared group, so the
    allowlist actively steered lanes into it). All path-keyed -- and a hardlink
    is exactly the case where two paths are one object. Every one of those
    guards returned a correct answer to its own question and a deploy still
    silently destroyed another lane's comparison baseline.

    The payload item list is passed through verbatim, so this is not a
    heuristic at the one site where being wrong costs evidence: the guard
    checks precisely the relpaths deploy() is about to overwrite, no more and
    no fewer.

    Verdict handling follows the DISK-GUARD-3 reasoning (c5d934d5):
      OK (0)           -> proceed silently.
      LINKED-DATA (1)  -> proceed. Read-only data sharing is CORRECT and is
                          what makes a private install affordable; it is only
                          reachable here via --data-links, which we do not pass.
      SHARED (3) /
      ERROR (2)        -> fatal, nothing written.
      guard missing, un-runnable, or an unrecognised code -> ALSO FATAL.

    Failing open was rejected for the same reason it was rejected for the disk
    guard: this guard's whole job is to prevent a silent, hard-to-attribute
    corruption, and degrading to unguarded on a missing dependency retires the
    protection exactly when the checkout is in a state nobody has verified.
    check-hardlink-isolation.py is a sibling file in this same repo.

    LOUD escape hatch: MC2_DEPLOY_ALLOW_SHARED_LINKS=1.
    """
    if os.environ.get("MC2_DEPLOY_ALLOW_SHARED_LINKS") == "1":
        log("WARNING: MC2_DEPLOY_ALLOW_SHARED_LINKS=1 -- hardlink-isolation "
            "preflight BYPASSED BY REQUEST. If this target shares inodes with "
            "another install, the deploy below rewrites that install's files "
            "in place and nothing afterwards will report it.")
        return

    guard = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "check-hardlink-isolation.py")
    if not os.path.isfile(guard):
        fail(f"hardlink-isolation guard not found at {guard} â€” refusing to "
             "deploy UNGUARDED. check-hardlink-isolation.py is a sibling of "
             "this script in the same repo; its absence means a broken or "
             "partial checkout. Restore it, or set "
             "MC2_DEPLOY_ALLOW_SHARED_LINKS=1 to bypass deliberately.")

    # Hand the guard the EXACT relpaths about to be written, via a temp file
    # (a 1500-entry argv is not portable).
    listfile = None
    try:
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False,
                                         encoding="utf-8", newline="\n") as fh:
            listfile = fh.name
            for _src, rel, _kind in items:
                fh.write(rel.replace(os.sep, "/") + "\n")
        log(f"link guard: checking {len(items)} payload relpath(s) in {target}")
        try:
            r = subprocess.run(
                [sys.executable, guard, "--target", target,
                 "--payload-list", listfile, "--quiet",
                 "--label", "deploy payload"],
                timeout=120, **console_child.child_kwargs())
            rc = r.returncode
        except Exception as e:  # noqa: BLE001
            fail(f"hardlink-isolation guard could not run ({e}) â€” refusing to "
                 "deploy UNGUARDED. Fix the guard invocation, or set "
                 "MC2_DEPLOY_ALLOW_SHARED_LINKS=1 to bypass deliberately.")
    finally:
        if listfile:
            try:
                os.unlink(listfile)
            except OSError:
                pass

    if rc == 3:
        fail("deploy target is NOT hardlink-isolated â€” NOTHING was written. "
             "See the [LINK_GUARD] lines above for the other dirs in this "
             "target's link group and the make-isolated-deploy.py remedy.")
    if rc == 2:
        fail("hardlink-isolation guard reported ERROR â€” refusing to deploy "
             "without a real verdict. See the [LINK_GUARD] lines above.")
    if rc == 1:
        log("link guard: read-only data is shared with another install "
            "(expected and desirable); written artifacts are private.")
    elif rc != 0:
        fail(f"hardlink-isolation guard returned unrecognised exit code {rc} "
             "â€” refusing to deploy on an unreadable verdict. Set "
             "MC2_DEPLOY_ALLOW_SHARED_LINKS=1 to bypass deliberately.")


ASIDE_RECEIPT = ".deploy-asides.json"
_ASIDE_RE = re.compile(r"\.old-\d+(-\d+)?$")
_ASIDE_COUNTER = itertools.count(1)
_ASIDE_TARGET = [None]  # the install this process is deploying into; set once its folder lease is held


def _aside_receipt_path(target_dir):
    return os.path.join(target_dir, ASIDE_RECEIPT)


def _read_aside_receipt(target_dir):
    try:
        with open(_aside_receipt_path(target_dir), encoding="utf-8") as f:
            data = json.load(f)
        return [p for p in data if isinstance(p, str)] if isinstance(data, list) else []
    except (OSError, ValueError):
        return []


def _write_aside_receipt(target_dir, paths):
    receipt = _aside_receipt_path(target_dir)
    try:
        if paths:
            with open(receipt, "w", encoding="utf-8") as f:
                json.dump(sorted(set(paths)), f)
        elif os.path.exists(receipt):
            os.unlink(receipt)
    except OSError as e:
        log(f"WARNING: could not update {receipt}: {e}")


def record_aside(aside, target_dir=None):
    """Remember a held aside in the install's receipt so a later deploy deletes it."""
    root = target_dir or _ASIDE_TARGET[0] or os.path.dirname(aside)
    _write_aside_receipt(root, _read_aside_receipt(root) + [os.path.abspath(aside)])


def sweep_asides(target_dir):
    """Delete the asides listed in the install's receipt. Only names matching .old-<pid>[-<n>] inside target_dir are touched; one still held stays listed."""
    root = os.path.normcase(os.path.abspath(target_dir))
    keep, listed = [], _read_aside_receipt(target_dir)
    for path in listed:
        full = os.path.normcase(os.path.abspath(path))
        if not _ASIDE_RE.search(full) or os.path.commonpath([root, full]) != root or full == root:
            continue  # not ours to delete: dropped from the receipt, never unlinked
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        except OSError:
            log(f"WARNING: old link still held, left in place: {path}")
            keep.append(path)
    if listed:
        _write_aside_receipt(target_dir, keep)


def replace_file(src, dst):
    """Replace a destination; a hardlinked one whose replace is refused is renamed aside, never written through.

    The shared pool inode keeps its bytes: only this install's directory entry is swapped for a private copy.
    """
    try:
        _replace_in_place(src, dst)
        return
    except PermissionError as first:
        try:
            shared = os.path.isfile(dst) and os.stat(dst).st_nlink > 1
        except OSError:
            shared = False
        if not shared:
            raise
        aside = f"{dst}.old-{os.getpid()}-{next(_ASIDE_COUNTER)}"
        try:
            os.replace(dst, aside)
        except OSError as e:
            raise PermissionError(first.errno, f"{dst} is hardlinked and could not be renamed aside "
                                  f"(in use by another process, open handle on this path): {e}") from e
    try:
        _replace_in_place(src, dst)
    except OSError:
        try:
            os.replace(aside, dst)  # put the old entry back rather than leave the install without the file
        except OSError:
            pass
        raise
    try:
        os.unlink(aside)
    except OSError:
        record_aside(aside)  # still held; the next deploy into this install (or this one's finally) deletes it


def _replace_in_place(src, dst):
    """Replace without changing attributes on a possibly shared destination inode."""
    try:
        os.replace(src, dst)
        return
    except PermissionError:
        if os.name != 'nt' or not os.path.isfile(dst) or os.stat(dst).st_mode & stat.S_IWRITE:
            raise
    # MoveFileEx (os.replace) rejects read-only destinations. FileRenameInfoEx
    # can replace that directory entry without clearing the shared read-only bit.
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.SetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.SetFileInformationByHandle.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    name = os.path.abspath(dst).encode('utf-16-le')

    class RenameInfo(ctypes.Structure):
        # Kernel32 expects a NUL-terminated FileName even though FileNameLength
        # excludes the terminator. Structure padding is not guaranteed to leave
        # room for it; otherwise adjacent memory can become part of the path.
        _fields_ = [('Flags', wintypes.DWORD), ('RootDirectory', wintypes.HANDLE),
                    ('FileNameLength', wintypes.DWORD), ('FileName', ctypes.c_byte * (len(name) + 2))]

    info = RenameInfo()
    info.Flags = 0x01 | 0x02 | 0x40  # REPLACE_IF_EXISTS | POSIX_SEMANTICS | IGNORE_READONLY_ATTRIBUTE
    info.FileNameLength = len(name)
    # SetFileInformationByHandle needs a NUL-terminated WCHAR path even though
    # FileNameLength excludes that terminator. Structure initialization zeros it.
    ctypes.memmove(ctypes.addressof(info) + RenameInfo.FileName.offset, name, len(name))
    handle = kernel.CreateFileW(os.path.abspath(src), 0x00010000, 7, None, 3, 0, None)  # DELETE, share all, OPEN_EXISTING
    if handle == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not kernel.SetFileInformationByHandle(handle, 22, ctypes.byref(info), ctypes.sizeof(info)):  # FileRenameInfoEx
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(handle)


def unlink_for_replace(path):
    """Detach shared files before clearing Windows read-only protection."""
    if os.path.lexists(path):
        mode = os.stat(path).st_mode
        if not mode & stat.S_IWRITE:
            if os.stat(path).st_nlink > 1:
                fd, private = tempfile.mkstemp(prefix=Path(path).name + '.private-', dir=Path(path).parent)
                os.close(fd)
                try:
                    shutil.copy2(path, private)
                    os.chmod(private, mode | stat.S_IWRITE)
                    replace_file(private, path)
                    # Clear the destination's retained DOS read-only attribute
                    # only after replacement has detached the shared inode.
                    os.chmod(path, mode | stat.S_IWRITE)
                finally:
                    unlink_for_replace(private)
            else:
                os.chmod(path, mode | stat.S_IWRITE)
        os.unlink(path)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def read_manifest(target_dir):
    """relpath -> (sha256, bytes, mtime_ns) from the target's CSV, or {}.

    Rows written before the mtime_ns column have no cache entry (mtime_ns
    None), so the first deploy after the upgrade copies and hashes as before.
    """
    path = os.path.join(target_dir, MANIFEST_NAME)
    if not os.path.isfile(path):
        return {}
    out = {}
    try:
        with open(path, newline="", encoding="utf-8") as f:
            rows = list(csv.reader(f))
    except (OSError, csv.Error):
        return {}
    if not rows or rows[0][:2] != ["manifest_version", MANIFEST_VERSION]:
        return {}
    for row in rows[2:]:
        if len(row) < 3 or not row[0]:
            continue
        try:
            size = int(row[2])
        except ValueError:
            continue
        mtime_ns = None
        if len(row) >= 6 and row[5].isdigit():
            mtime_ns = int(row[5])
        out[row[0]] = (row[1], size, mtime_ns)
    return out


def source_stamp(src):
    """(size, mtime_ns) of a source file, or None when it does not exist."""
    try:
        st = os.stat(src)
    except OSError:
        return None
    return (st.st_size, st.st_mtime_ns)


# TERRAIN-MATERIAL-TEXTURES-1-FIX (fix D): read a KTX2 file's levelCount from the
# header. The KTX2 v2 layout is a 12-byte identifier followed by a fixed header:
# vkFormat(u32) typeSize(u32) pixelWidth(u32) pixelHeight(u32) pixelDepth(u32)
# layerCount(u32) faceCount(u32) levelCount(u32) ... -- so levelCount is the 8th
# u32 after the identifier (byte offset 12 + 7*4 = 40). Returns the level count,
# or None if the file is not a recognizable KTX2 (never blocks a non-KTX2 asset).
def _ktx2_level_count(path):
    import struct
    KTX2_MAGIC = b"\xabKTX 20\xbb\r\n\x1a\n"
    try:
        with open(path, "rb") as f:
            head = f.read(44)
        if len(head) < 44 or head[:12] != KTX2_MAGIC:
            return None
        # 8 u32 fields at offset 12; levelCount is the 8th (index 7).
        fields = struct.unpack("<8I", head[12:44])
        return fields[7]
    except OSError:
        return None


def repo_root():
    """Source worktree root = parent of the scripts/ dir this file lives in."""
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git_head(root):
    try:
        out = subprocess.run(
            ["git", "-C", root, "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=30,
            **console_child.child_kwargs(redirected=True),
        )
        if out.returncode == 0:
            return out.stdout.strip()
    except Exception:
        pass
    return "unknown"


def read_build_fingerprint(build_dir):
    """Parse generated/build_fingerprint.h next to the build dir.

    Returns {"sha": str, "dirty": bool} or None if the header is missing /
    unparsable. build_dir is usually <build64*>/RelWithDebInfo; the header
    lives at <build64*>/generated/build_fingerprint.h (also probed directly
    under build_dir for non-config layouts).

    WHY: the manifest's src_commit is git HEAD *now*, but the exe being
    deployed embeds the git state *at build time*. A dirty-tree build deploys
    an exe whose exact source state is unattributable -- the 2026-07-12 grey-
    regression P0 burned hours on exactly this (dirty pre-commit build,
    fingerprint said the prior sha). This reads the same header the exe
    compiled in, so the manifest can record what actually shipped.
    """
    for probe in (
        os.path.join(build_dir, "..", "generated", "build_fingerprint.h"),
        os.path.join(build_dir, "generated", "build_fingerprint.h"),
    ):
        pth = os.path.abspath(probe)
        if not os.path.isfile(pth):
            continue
        try:
            with open(pth, encoding="utf-8", errors="replace") as f:
                text = f.read()
            m_sha = re.search(r'#define MC2_BUILD_GIT_SHA\s+"([0-9a-fA-F]+)"', text)
            m_dirty = re.search(r"#define MC2_BUILD_GIT_DIRTY\s+(\d+)", text)
            if m_sha and m_dirty:
                return {"sha": m_sha.group(1), "dirty": m_dirty.group(1) != "0"}
        except OSError:
            pass
    return None


# ---------------------------------------------------------------------------
# Step 0: exe lock check (Mistake A). Windows: a running process holding the
# target exe makes overwrite silently fail / be refused. Probe by attempting
# an exclusive-open of the existing target binary. NEVER taskkill.
# ---------------------------------------------------------------------------

def is_file_locked_windows(path):
    if not os.path.exists(path):
        return False
    GENERIC_WRITE = 0x40000000
    OPEN_EXISTING = 3
    FILE_SHARE_NONE = 0
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    k32 = ctypes.windll.kernel32
    k32.CreateFileW.restype = ctypes.c_void_p  # default c_int truncates -1
    handle = k32.CreateFileW(path, GENERIC_WRITE, FILE_SHARE_NONE,
                             None, OPEN_EXISTING, 0, None)
    if handle is None or handle == INVALID_HANDLE_VALUE:
        return True  # ERROR_SHARING_VIOLATION et al -> locked
    k32.CloseHandle(ctypes.c_void_p(handle))
    return False


def _norm(p):
    """Normalize a path for case-insensitive Windows comparison."""
    return os.path.normcase(os.path.abspath(str(p)))


def _same_path(a, b):
    """True if a and b refer to the same file (os.path.samefile when possible)."""
    try:
        return os.path.samefile(a, b)
    except (OSError, ValueError):
        return _norm(a) == _norm(b)


def _proc_exe_paths_psutil(exe_name):
    """Yield (pid, exe_path_or_None) for every process whose name matches exe_name.

    exe_path is None when the path cannot be read (access denied, zombie, etc.).
    """
    name_lower = exe_name.lower()
    for proc in psutil.process_iter(["pid", "name"]):
        try:
            if proc.info["name"] and proc.info["name"].lower() == name_lower:
                try:
                    yield proc.pid, proc.exe()
                except (psutil.AccessDenied, psutil.ZombieProcess, OSError):
                    yield proc.pid, None
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass


def _proc_exe_paths_wmic(exe_name):
    """Fallback when psutil is unavailable: query WMIC for ExecutablePath.

    Yields (pid, exe_path_or_None).  pid is int when parseable, else str.
    """
    results = []
    try:
        out = subprocess.run(
            ["wmic", "process", "where",
             f"name='{exe_name}'", "get", "ProcessId,ExecutablePath", "/FORMAT:CSV"],
            capture_output=True, text=True, timeout=30,
            **console_child.child_kwargs(redirected=True),
        )
        for line in out.stdout.splitlines():
            line = line.strip()
            if not line or line.lower().startswith("node"):
                continue
            parts = line.split(",")
            # CSV columns: Node, ExecutablePath, ProcessId
            if len(parts) >= 3:
                exe_path = parts[1].strip() or None
                pid_str = parts[2].strip()
                try:
                    pid = int(pid_str)
                except ValueError:
                    pid = pid_str
                results.append((pid, exe_path))
    except Exception:
        pass
    return results


def _enumerate_matching_processes(exe_name):
    """Return list of (pid, exe_path_or_None) for all running processes named exe_name."""
    if _PSUTIL_OK:
        return list(_proc_exe_paths_psutil(exe_name))
    return _proc_exe_paths_wmic(exe_name)


def _check_running_processes(target_exe_resolved, exe_name):
    """Path-aware running-process check.

    - If a running process exe matches target_exe_resolved: HARD FAIL (returns
      a non-empty error string).
    - Processes from other folders: log info, allow.
    - Processes whose path cannot be read: log warning, allow (do NOT block
      unless the file-lock probe below catches them).

    Returns an error string if deploy must be blocked, or '' if clear.
    """
    procs = _enumerate_matching_processes(exe_name)
    if not procs:
        return ""

    blocking = []
    other = []
    unreadable = []

    for pid, proc_exe in procs:
        if proc_exe is None:
            unreadable.append(pid)
            continue
        proc_resolved = os.path.normcase(os.path.abspath(proc_exe))
        if _same_path(proc_exe, target_exe_resolved):
            blocking.append((pid, proc_exe))
        else:
            other.append((pid, proc_exe))

    for pid in unreadable:
        log(f"WARNING: running process PID {pid} ({exe_name}) path unreadable "
            "(access denied / zombie) -- not blocking deploy")
    for pid, path in other:
        log(f"INFO: running {exe_name} PID {pid} from OTHER folder "
            f"({path}) -- deploy continues")

    if blocking:
        lines = "\n".join(f"  PID {pid}: {path}" for pid, path in blocking)
        return (
            f"target exe is running from THIS deploy folder:\n{lines}\n"
            "  Close that instance and re-run. "
            "This script will NEVER taskkill automatically."
        )
    return ""


#: How long a deploy waits for a game running from its target folder to exit (another lane's smoke run, usually). MC2_DEPLOY_WAIT_S overrides.
DEFAULT_DEPLOY_WAIT_S = 30.0


def _deploy_lease_module():
    try:
        from smoke_lib import deploy_lease
    except ImportError:
        from scripts.smoke_lib import deploy_lease
    return deploy_lease


def take_folder_lease(target_dir, wait_s):
    """Hold the smoke folder lease while this script writes into the install.

    A smoke run holds the lease of the folder it plays from, and a deploy under it replaces the exe the run was started from; a lane that deploys into an
    install somebody else leased must wait for the holder, not overwrite it. Waits up to `wait_s` seconds with a visible countdown; a lease that is still
    live after that is fatal. Returns True when the lease is ours to release.
    """
    lease = _deploy_lease_module()
    try:
        lease.acquire_lease_waiting(target_dir, "deploy_payload", wait_s)
    except lease.LeaseError as exc:
        fail(f"{exc}\n  Another session leased this install (a smoke run, or a lane deploying); this script waited {wait_s:.0f}s and never overwrites a leased install.",
             code=2)
    log(f"folder lease taken: {target_dir}")
    return True


def release_folder_lease(target_dir):
    try:
        _deploy_lease_module().release_lease(target_dir)
        log(f"folder lease released: {target_dir}")
    except Exception as exc:  # noqa: BLE001 -- the deploy is done; a stale entry expires by TTL
        log(f"folder lease release failed ({exc}); it expires by TTL")


def check_locks(target_dir, exe_name, wait_s=0.0):
    target_exe = os.path.join(target_dir, exe_name)
    target_exe_resolved = os.path.normcase(os.path.abspath(target_exe))

    # Step 1: path-aware running-process check (psutil / wmic). A game running from this folder is waited for (bounded, visible countdown): it is
    # normally a smoke run that ends by itself; this script never ends it.
    err = _check_running_processes(target_exe_resolved, exe_name)
    if err and wait_s > 0:
        try:
            from smoke_lib import bounded_wait
        except ImportError:
            from scripts.smoke_lib import bounded_wait

        def still_running():
            lines = _check_running_processes(target_exe_resolved, exe_name).splitlines()
            return " ".join(s.strip() for s in lines[:2]) if lines else ""

        bounded_wait.wait_until(still_running, f"deploy into {target_dir}", wait_s)
        err = _check_running_processes(target_exe_resolved, exe_name)
    if err:
        fail(
            f"{err}\n"
            "  Copy-to-locked-exe silently fails on Windows (stale-exe trap, "
            "Mistake A).",
            code=2,
        )

    # Step 2: file-lock probe as a safety net (catches cases where psutil
    # could not enumerate the process but the file handle is still held).
    if sys.platform == "win32" and is_file_locked_windows(target_exe):
        fail(
            f"target exe is LOCKED by an unidentified process: {target_exe}\n"
            "  psutil/wmic found no matching process by name, but the file "
            "cannot be opened for writing. Close any process holding it and re-run. "
            "This script will NEVER taskkill automatically.",
            code=2,
        )

    log(f"lock check OK: {target_exe} not held by a running process from this deploy folder")


# ---------------------------------------------------------------------------
# Payload enumeration
# ---------------------------------------------------------------------------

def find_build_root(build_dir, explicit_root=None):
    """Locate the build64-family root (holds mc2.dir).

    Historically this walked up for an ancestor named literally 'build64', which
    fails for sibling build dirs like build64_island / build64_editor. Now:
      1. explicit_root (--build64-root) wins if given;
      2. else walk up accepting any ancestor whose basename STARTS WITH 'build64'
         (build64, build64_island, ...);
      3. else '' (non-standard layout; callers skip the launcher/freshness guard).
    """
    if explicit_root:
        return os.path.abspath(explicit_root).rstrip(os.sep)
    bd = os.path.abspath(build_dir).rstrip(os.sep)
    b64 = bd
    while b64 and not os.path.basename(b64).startswith("build64"):
        parent = os.path.dirname(b64)
        if parent == b64:
            return ""
        b64 = parent
    return b64


def is_single_config(build_dir, b64):
    """True for a Ninja-style tree: outputs land directly in the build root
    (no <config> subdir), because there is exactly one config per tree.

    WHY: the VS tree nests everything under a config name (RelWithDebInfo/,
    mc2.dir/RelWithDebInfo/); Ninja trees
    (build64-ninja) do not. Detecting the layout from the tree itself keeps
    both usable without forcing either into the other's directory shape.
    """
    if not b64:
        return False
    return os.path.isfile(os.path.join(b64, "build.ninja"))


def mc2_obj_dir(b64, cfg, single_config):
    """Where the mc2 target's .obj files live (the exe-freshness reference)."""
    if single_config:
        return os.path.join(b64, "CMakeFiles", "mc2.dir")
    return os.path.join(b64, "mc2.dir", cfg)


def check_exe_fresh(exe_src, build_dir, allow_stale_exe, build64_root=None):
    """Mistake E: stale exe deployed because the source exe was not relinked.

    The linked exe must be NEWER than every compiled .obj that feeds it. An incremental build
    that compiled objects but did not relink the exe -- or a deploy racing a still-running build --
    leaves the exe OLDER than its objects, so deploy faithfully copies a stale binary (this
    actually happened: a deploy shipped a pre-slice exe ~2.5 KB smaller than the rebuilt one).
    Compare exe mtime against the newest .obj under build64/mc2.dir/<config>/
    (VS) or build64-ninja/CMakeFiles/mc2.dir/ (Ninja).
    """
    if not os.path.isfile(exe_src):
        return
    exe_m = os.path.getmtime(exe_src)
    bd = os.path.abspath(build_dir).rstrip(os.sep)
    cfg = os.path.basename(bd)                      # e.g. RelWithDebInfo
    b64 = find_build_root(build_dir, build64_root)
    if not b64:
        return                                      # non-standard layout; skip the guard
    objdir = mc2_obj_dir(b64, cfg, is_single_config(build_dir, b64))
    newest_m, newest_p = 0.0, None
    for root, _dirs, files in os.walk(objdir):
        for f in files:
            if f.endswith((".obj", ".o")):
                m = os.path.getmtime(os.path.join(root, f))
                if m > newest_m:
                    newest_m, newest_p = m, os.path.join(root, f)
    if newest_p and newest_m > exe_m + 1.0:         # 1s slack for filesystem timestamp jitter
        msg = (f"STALE EXE (Mistake E): {exe_src}\n"
               f"  exe mtime {exe_m:.0f} is OLDER than object {newest_p}\n"
               f"  obj mtime {newest_m:.0f} -- the build did not relink (incremental skip or a\n"
               f"  deploy racing an in-flight build). Rebuild the exe before deploying.")
        if allow_stale_exe:
            log(f"WARNING (--allow-stale-exe): {msg}")
        else:
            fail(msg + "\n  (override: --allow-stale-exe)")


def enumerate_payload(src_root, build_dir, exe_name, pdb_name,
                      require_build=True, build64_root=None, code_only=False, cook=None,
                      no_example_mods=False):
    """Returns list of (src_abspath, target_relpath, kind).

    cook (default: require_build): run the on-demand texture cooks (concrete detail, cliff rock, tree upscale; up to
    minutes of subprocess time) before listing. Enumeration for tests, manifests and dry runs passes cook=False and
    only lists the sidecars that are already on disk.

    require_build=False (manifest-only): the build outputs may be gone; we
    only need the relpath list to know which target files to re-hash, so a
    missing source exe/dll is tolerated (the entry is still emitted so the
    deployed copy can be recorded).
    """
    items = []
    native = exe_name == "mc2"
    exe_src = os.path.join(build_dir, exe_name)
    if require_build and not os.path.isfile(exe_src):
        fail(f"source exe not found: {exe_src}")
    items.append((exe_src, exe_name, "exe"))

    # Out-of-process minidump writer (sentry-native crashpad backend): mc2 copies it next to itself at link
    # time and gos_crashpad.cpp starts it from the exe's folder. Without it the in-process dump stays in charge.
    if exe_name.lower() != EDITOR_EXE:
        handler_name = "crashpad_handler" if native else "crashpad_handler.exe"
        handler_src = os.path.join(build_dir, handler_name)
        if require_build and not os.path.isfile(handler_src):
            fail(f"crashpad handler not found: {handler_src}")
        items.append((handler_src, handler_name, "exe"))

    pdb_src = os.path.join(build_dir, pdb_name)
    if not native:
        items.append((pdb_src, pdb_name, "pdb"))  # presence checked later

    for dll in (() if native else RUNTIME_DLLS):
        p = os.path.join(build_dir, dll)
        if os.path.isfile(p):
            items.append((p, dll, "dll"))
        elif require_build:
            fail(f"required runtime DLL missing from build: {p}")

    b64 = find_build_root(build_dir, build64_root)
    # Baked Vulkan SPIR-V (VULKAN-SHADER-TOOLCHAIN-1): glslangValidator/glslc
    # compiles shaders/vulkan/*.{vert,frag} to <build64-root>/shaders/vulkan/*.spv
    # (CMAKE_BINARY_DIR, i.e. the build64 root, NOT the per-config build_dir --
    # the CMake build root). The Vulkan island
    # loaders (vulkan_*_island.cpp) read
    # MC2_VULKAN_SPV_DIR if set, else default to "shaders/vulkan" relative to
    # the exe's CWD -- so deployed .spv must land at <target>/shaders/vulkan/.
    # Without this, any deployed install hits "cannot open spv" ->
    # vk_device_init_failed for every Vulkan region/island, even though the
    # exe + GL shaders deployed fine (found by VK-REGION-SSAO-1 content pass).
    # Vulkan is mandatory. A release/deploy without baked shaders cannot run.
    vk_spv_dir = os.path.join(b64, "shaders", "vulkan") if b64 else ""
    if vk_spv_dir and os.path.isdir(vk_spv_dir):
        for p in baked_spv_files(b64):
            items.append((str(p), f"shaders/vulkan/{p.name}", "shader"))
    if require_build and not any(rel.startswith("shaders/vulkan/") and rel.endswith(".spv")
                                 for _src, rel, _kind in items):
        fail(f"No baked Vulkan SPIR-V at {vk_spv_dir or '<no build root>'}; build shaders before deploying")

    # Deploy the ENTIRE shaders/ tree recursively (every file, all extensions),
    # not an extension allowlist. Shaders #include each other across stages and
    # extensions (.comp pulls .glsl/.hglsl; .frag pulls include/*.hglsl); a
    # per-ext allowlist silently dropped .comp + root .glsl and the GPU-driven
    # terrain cull/lighting/water compute then failed to compile -> NaN frustum
    # planes -> terrain culled to black, while smoke still PASSED (compile-fail
    # is non-fatal sticky-disable, not a crash). Only skip test fixtures.
    shader_dir = os.path.join(src_root, "shaders")
    if not os.path.isdir(shader_dir):
        fail(f"source shader dir not found: {shader_dir}")
    for dirpath, dirs, files in os.walk(shader_dir):
        dirs[:] = [d for d in dirs if d.lower() != "fixtures"]
        for fn in sorted(files):
            p = os.path.join(dirpath, fn)
            # GUARD (2026-07-02): refuse to ship shader sources containing git
            # conflict markers. A mid-merge deploy shipped a frag with <<<<<<<
            # markers -> GLSL compile fail -> shader_error smoke FAIL on the
            # user's viewing lane. Cheap scan; text shader sources only.
            if fn.lower().endswith((".frag", ".vert", ".comp", ".glsl", ".hglsl",
                                    ".tesc", ".tese", ".geom")):
                try:
                    with open(p, "rb") as fh:
                        blob = fh.read()
                    if b"<<<<<<<" in blob or b">>>>>>>" in blob:
                        fail(f"shader source contains git conflict markers: {p}")
                except OSError as e:
                    fail(f"cannot read shader source {p}: {e}")
            rel = os.path.relpath(p, src_root).replace(os.sep, "/")
            items.append((p, rel, "shader"))

    if code_only:
        return items

    # Lookup textures are embedded in the exe; deploy notices, not reference sources.
    for tree in ("3rdparty/smaa", "3rdparty/cmaa2"):
        if require_build and not os.path.isdir(os.path.join(src_root, tree)):
            fail(f"spatial AA payload tree missing in source: {tree}")
        for directory, _dirs, files in os.walk(os.path.join(src_root, tree)):
            for fn in sorted(files):
                if not fn.lower().startswith(("license", "notice", "upstream")):
                    continue
                path = os.path.join(directory, fn)
                rel = os.path.relpath(path, src_root).replace(os.sep, "/")
                items.append((path, rel, "support"))

    # Portable community importer and docs; the batch launcher is Windows-only.
    if not native:
        items.append((os.path.join(src_root, "import-campaign.bat"), "import-campaign.bat", "support"))
    for rel in ("tools/import_campaign.py", "THIRD_PARTY_NOTICES.txt", "docs/modding/IMPORT-CAMPAIGNS.md",
                "docs/modding/campaign-import-status.md", "docs/modding/campaign-import-window.jpg"):
        items.append((os.path.join(src_root, rel), rel, "support"))
    for tree in ("tools/mc2_import", "tools/mc2x_import", "tools/mco_import"):
        for directory, dirs, files in os.walk(os.path.join(src_root, tree)):
            dirs[:] = [d for d in dirs if d != "__pycache__"]
            for fn in sorted(files):
                if fn.endswith((".pyc", ".pyo")):
                    continue
                path = os.path.join(directory, fn)
                items.append((path, os.path.relpath(path, src_root).replace(os.sep, "/"), "support"))

    items.append((os.path.join(src_root, "scripts", "console_child.py"), "scripts/console_child.py", "support"))
    items.append((os.path.join(src_root, "scripts", "smoke_lib", "child_registry.py"), "scripts/smoke_lib/child_registry.py", "support"))
    for rel in IMPORTED_STRUCTURE_PAYLOAD:
        p = os.path.join(src_root, rel)
        if require_build and not os.path.isfile(p):
            fail(f"imported structure payload missing in source: {p}")
        items.append((p, rel, "support"))

    for rel in GAME_DATA_PAYLOAD:
        p = os.path.join(src_root, rel)
        if require_build and not os.path.isfile(p):
            fail(f"game data payload missing in source: {p}")
        items.append((p, rel, "support"))

    for path in sorted((Path(src_root) / "data/art").glob("mc2_component_*.tga")):
        items.append((str(path), path.relative_to(src_root).as_posix(), "support"))
    # MENU-PLANET-MODERN-1: main-menu backdrop (hi-res tiles + the fit that places them).
    for pat in ("mcl_menu_*.tga", "mcl_sp.fit"):
        for path in sorted((Path(src_root) / "data/art").glob(pat)):
            items.append((str(path), path.relative_to(src_root).as_posix(), "support"))
    # Optional cooked logistics overrides; missing art retains the original PAK/atlas.
    for folder in ("data/art/mechicons", "data/art/missionmaps", "data/art/pilots"):
        for path in sorted((Path(src_root) / folder).rglob("*.png")):
            items.append((str(path), path.relative_to(src_root).as_posix(), "support"))

    # UI-CHROME-SPLIT-1: per-widget UI chrome sources + manifest (mclib/ui_chrome_sheets.cpp re-packs them at load).
    for path in sorted((Path(src_root) / "data/art/ui").rglob("*")):
        if path.suffix in (".png", ".json"):
            items.append((str(path), path.relative_to(src_root).as_posix(), "support"))

    # UI-DEFS-LOCKSTEP-HARDEN-1 (BLACK-MENU-BISECT-1): data/defs/** ships in
    # lockstep with the exe, exactly like shaders/. Since the ui-phase1 merge
    # the front-end menus ARE defs/ui pages; an exe running against a
    # version-skewed defs/ui package rendered an opaque black menu, silently.
    # Hard requirements:
    #   - data/defs/ must exist in source (else FAIL -- an exe without its
    #     defs is a broken deploy, not an optional extra)
    #   - data/defs/ui/PACKAGE.fit must exist (the runtime version key the
    #     exe checks at boot; a deploy without it fail-softs every menu to
    #     the legacy path on arrival)
    #   - post-copy byte diff hard-fails on mismatch (kind "defs" falls into
    #     the same hard-fail bucket as shaders in deploy())
    defs_dir = os.path.join(src_root, "data", "defs")
    if not os.path.isdir(defs_dir):
        fail(f"source data/defs dir not found: {defs_dir}\n"
             "  The front-end UI is defs/ui pages; an exe deployed without "
             "matching data/defs shows legacy-fallback menus at best.")
    pkg_fit = os.path.join(defs_dir, "ui", "PACKAGE.fit")
    if not os.path.isfile(pkg_fit):
        fail(f"data/defs/ui/PACKAGE.fit missing in source: {pkg_fit}\n"
             "  The exe checks this version key at boot (UI-DEFS-LOCKSTEP-"
             "HARDEN-1); deploying without it makes every defs/ui page load "
             "fail-soft to legacy on the target.")
    for dirpath, _dirs, files in os.walk(defs_dir):
        for fn in sorted(files):
            sp = os.path.join(dirpath, fn)
            rel = os.path.relpath(sp, src_root).replace(os.sep, "/")
            items.append((sp, rel, "defs"))

    # TERRAIN-MATERIAL-TEXTURES-1: optional loose cooked sidecars -- skip (log)
    # when absent instead of failing; the engine fail-softs without them.
    # TERRAIN-MATERIAL-TEXTURES-1-FIX (fix D, MIPS ROOT CAUSE): the "levels=1
    # deployed" bug was a stale pre-mip cook getting picked up because deploy just
    # copies whatever _albedo.ktx2 is on disk. Guard it here: read the KTX2
    # levelCount and HARD-FAIL if a present layer is mip-incomplete, so a stale
    # cook can never reach the payload silently. Re-cook, then re-deploy.
    if require_build if cook is None else cook:
        ensure_concrete_detail(src_root)
        ensure_cliff_rock(src_root)
        ensure_tree_upscale(src_root)
        ensure_actor_pbr(src_root)
    for rel in tree_upscale_payload(src_root):
        p = os.path.join(src_root, rel)
        if os.path.isfile(p):
            items.append((p, rel, "support"))
    ensure_grass_turf(src_root)
    ensure_dirt_sand(src_root)
    ensure_forest_floor(src_root)
    for rel in TERRAIN_LAYER_PAYLOAD:
        p = os.path.join(src_root, rel)
        if not os.path.isfile(p):
            log(f"terrain layer payload absent (skipped, engine fail-softs): {rel}")
            continue
        lvls = _ktx2_level_count(p)
        if lvls is not None and lvls <= 1:
            fail(f"terrain layer payload is mip-incomplete (levelCount={lvls}, expected >1): "
                 f"{rel} -- re-cook with tools/mc2texcook/cook_terrain_layers.py (mips=True) "
                 f"BEFORE deploying, or the engine ships a shimmering single-level albedo array.")
        items.append((p, rel, "support"))

    # Support payload: launch scripts (per target) + editor authoring trees.
    # These are source-tracked, not build outputs, so they ship even though
    # they never appear in build_dir. require_build is irrelevant here: a
    # missing SOURCE support file is a real error (loud copy failure), while
    # manifest-only tolerates a missing TARGET copy downstream as usual.
    is_editor = exe_name.lower() == EDITOR_EXE
    for script in (() if native else SUPPORT_SCRIPTS["editor" if is_editor else "game"]):
        sp = os.path.join(src_root, script)
        items.append((sp, script, "support"))
    if is_editor:
        for tree in EDITOR_SUPPORT_TREES:
            tree_abs = os.path.join(src_root, tree)
            if require_build and not os.path.isdir(tree_abs):
                fail(f"editor support tree not found in source: {tree_abs}\n"
                     "  The editor's Generate-Mission path needs this tree; "
                     "a deploy without it builds a non-functional editor.")
            for dirpath, _dirs, files in os.walk(tree_abs):
                for fn in sorted(files):
                    sp = os.path.join(dirpath, fn)
                    rel = os.path.relpath(sp, src_root).replace(os.sep, "/")
                    items.append((sp, rel, "support"))
    # Example mods (EXAMPLE-MODS-1) -> <target>/mods/<id>/ (launcher add-on
    # checkboxes). Ships with both game and editor installs.
    if no_example_mods:
        log("[DEPLOY] example mods skipped (--no-example-mods)")
    else:
        items.extend(example_mods_payload(src_root))

    # Generated unit/equipment definitions -> <target>/data/defs/{units,equipment}/
    # so MC2_*_DEFINITION_SOURCE=generated works from a normal install. Converter
    # runs are skipped on manifest-only passes (require_build=False): no fresh
    # generation side effects, existing target copies keep their hashes.
    if require_build:
        items.extend(generate_defs_payload(src_root, b64))

    # Cook tools ship with both game and editor installs (modder/user asset pipeline).
    for entry in GAME_COOK_TOOLS + GAME_LOOSE_ART_TREES:
        entry_abs = os.path.join(src_root, entry)
        if os.path.isfile(entry_abs):
            items.append((entry_abs, entry.replace(os.sep, "/"), "support"))
        elif os.path.isdir(entry_abs):
            for dirpath, dirs, files in os.walk(entry_abs):
                dirs[:] = [d for d in dirs if d != "__pycache__"]   # W138h: the tree cook imports tools/examples; bytecode is not payload
                for fn in sorted(files):
                    if fn.endswith((".pyc", ".pyo")):
                        continue
                    sp = os.path.join(dirpath, fn)
                    rel = os.path.relpath(sp, src_root).replace(os.sep, "/")
                    items.append((sp, rel, "support"))
    from asset_parity import is_deploy_artifact
    return [item for item in items if not is_deploy_artifact(item[1])]


# ---------------------------------------------------------------------------
# Deploy + verify
# ---------------------------------------------------------------------------


# The detail-normal layers terrain_tier1.frag samples get lossless RGBA8 mip-complete sidecars next to the deployed TGAs, cooked in the
# target from the verified TGAs (generated, about 22 MB each, never in git). Soft: without them the engine imports the TGAs.
ENGINE_NORMAL_COOK = Path(__file__).resolve().parents[1] / "tools" / "mc2texcook" / "cook_terrain_layers.py"
# Bump when the cooked texels change meaning; the stamp in the target forces one recook. 2 = RGBA normal order (1 was TGA B, G, R). 3 = lossless RGBA8 + box mips (2 was BC7).
ENGINE_NORMAL_COOK_VERSION = "3"


def ensure_engine_normal_cooks(target_dir, cooker=subprocess.run):
    tex = os.path.join(target_dir, "data", "textures")
    stamp = os.path.join(tex, "engine_normals.cookver")
    try:
        with open(stamp, encoding="utf-8") as f:
            current = f.read().strip() == ENGINE_NORMAL_COOK_VERSION
    except OSError:
        current = False
    stale = [i for i in range(6) if not current or not os.path.isfile(os.path.join(tex, f"mat{i}_normal.ktx2")) or
             os.path.getmtime(os.path.join(tex, f"mat{i}_normal.ktx2")) < os.path.getmtime(os.path.join(tex, f"mat{i}_normal.tga"))]
    if not stale:
        return
    log(f"detail-normal sidecars absent or stale for layers {stale}: cooking them (a few seconds)")
    try:
        r = cooker([sys.executable, str(ENGINE_NORMAL_COOK), "--engine-normals", tex] + ([] if current else ["--force"]),
                   capture_output=True, text=True, timeout=900, env=dict(os.environ, OMP_NUM_THREADS="1"), **console_child.child_kwargs(redirected=True))
    except subprocess.TimeoutExpired:
        cook_failed("detail-normal cook timed out after 900 s")
        return
    if r.returncode:
        cook_failed("detail-normal cook failed rc=%s: %s" % (r.returncode, (r.stdout + r.stderr).strip()[-300:]))
        return
    with open(stamp, "w", encoding="utf-8") as f:
        f.write(ENGINE_NORMAL_COOK_VERSION + "\n")


def verify_engine_normals_present(target_dir):
    """A code-only deploy skips the cook; the target must still hold all six normals and a current cook stamp (asset_parity does not check them)."""
    tex = os.path.join(target_dir, "data", "textures")
    problems = [f"mat{i}_normal.ktx2 missing" for i in range(6) if not os.path.isfile(os.path.join(tex, f"mat{i}_normal.ktx2"))]
    try:
        with open(os.path.join(tex, "engine_normals.cookver"), encoding="utf-8") as f:
            found = f.read().strip()
    except OSError:
        found = None
    if found != ENGINE_NORMAL_COOK_VERSION:
        problems.append(f"engine_normals.cookver is {found!r}, expected {ENGINE_NORMAL_COOK_VERSION!r}")
    if problems:
        fail(f"engine normals in {tex} not deploy-ready ({'; '.join(problems)}); run a full (non code-only) deploy to cook them")


# MC2_GOLDEN_MATERIAL_ROOT: a copy of the golden install's 14 material files (the Linux box has no A: drive); hashes are still checked.
GOLDEN_MATERIAL_ROOT = Path(os.environ.get("MC2_GOLDEN_MATERIAL_ROOT") or "A:/Games/mc2-opengl/mc2-win64-v0.4c")
MATERIAL_MANIFEST = Path(__file__).with_name("terrain_materials.json")


def ensure_terrain_materials(target_dir):
    """Verify required modern normals and displacement maps; read golden assets without modifying them.

    Replace destination entries atomically so shared data hardlinks stay intact.
    Correct files are never rewritten. Refuse unavailable or changed golden bytes.
    """
    hashes = {}
    try:
        manifest = json.loads(MATERIAL_MANIFEST.read_text(encoding="utf-8"))
        expected_paths = ({f"data/textures/mat{i}_normal.tga" for i in range(9)} |
                          {f"data/textures/mat{i}_displacement.tga" for i in range(4)} |
                          {"data/textures/detail_displacement.tga"})
        if set(manifest) != expected_paths or any(
                not re.fullmatch(r"[0-9a-f]{40}", h) for h in manifest.values()):
            raise ValueError("invalid terrain material manifest")
        for rel, expected in manifest.items():
            dst = Path(target_dir) / rel
            if not dst.is_file() or hashlib.sha1(dst.read_bytes()).hexdigest() != expected:
                src = GOLDEN_MATERIAL_ROOT / rel
                content = src.read_bytes()
                if hashlib.sha1(content).hexdigest() != expected:
                    raise ValueError(f"golden terrain material hash mismatch: {src}")
                dst.parent.mkdir(parents=True, exist_ok=True)
                temporary = None
                try:
                    with tempfile.NamedTemporaryFile(dir=dst.parent, delete=False) as out:
                        temporary = Path(out.name)
                        out.write(content)
                    unlink_for_replace(dst)
                    os.replace(temporary, dst)
                finally:
                    if temporary is not None and temporary.exists():
                        temporary.unlink()
                log(f"restored required terrain material: {rel}")
            before = dst.stat()
            content = dst.read_bytes()
            after = dst.stat()
            if hashlib.sha1(content).hexdigest() != expected:
                raise ValueError(f"deployed terrain material hash mismatch: {dst}")
            # Stamped like every other row (None when the file changed while it was read), so the next prelaunch trusts it.
            stable = (before.st_mtime_ns, before.st_size) == (after.st_mtime_ns, after.st_size)
            hashes[rel] = (hashlib.sha256(content).hexdigest(), len(content), None, after.st_mtime_ns if stable else None)
        log("terrain materials verified: 14/14")
    except (OSError, ValueError, TypeError) as exc:
        fail(f"required terrain materials unavailable: {exc}")
    return hashes


def deploy(items, target_dir, allow_stale_pdb, preserve=(), cache=None,
           full_verify=False, code_only=False, verified=None, deploying_target="game",
           verify_normals=True):
    """Copy + verify the payload.

    Hash receipts retain source and destination metadata plus the hash verified
    at write time. Metadata drift rehashes bytes, including same-size corruption.
    Executables are always hashed on both sides. --full-verify copies everything.
    """
    from asset_parity import is_deploy_artifact
    items = [item for item in items if not is_deploy_artifact(item[1])]
    # Retention must reach retire_unowned(), before it can delete static files.
    # Shader inventories still belong entirely to the current build.
    src_hashes = carry_rows({rel: row for rel, row in (cache or {}).items()
                  if not rel.startswith(("shaders/", "shaders_vk/"))}, target_dir) if code_only else ensure_terrain_materials(target_dir)
    if not code_only:
        ensure_engine_normal_cooks(target_dir)
    elif verify_normals:
        # The shader-inventory dry run deploys into an empty temp dir with no textures.
        verify_engine_normals_present(target_dir)
    shader_mismatches = []
    pdb_problem = None
    preserve = {p.replace(os.sep, "/") for p in preserve}
    cache = cache or {}
    from kit_payload import HashReceipts, contained_path
    receipts = HashReceipts(Path(target_dir) / ".deploy-receipts.json")
    kit_root = Path(target_dir).resolve()
    kit_parents = {}
    skipped = 0
    pdb_skipped = False
    copy_seconds = 0.0
    verify_started = time.perf_counter()

    for item in items:
        src, rel, kind = item
        expected = getattr(item, "declared_sha", None)
        normalize = kind != "intentional" and rel.endswith(".mcasset.json")
        dst = os.path.join(target_dir, rel)
        if kind == "intentional" and expected is None:
            fail(f"intentional asset missing declared sha256: {rel}")
        if verified and rel in verified and (expected is None or verified[rel] == expected):
            # Lane seed: source hash and the verified seed file identity plus
            # NTFS ChangeTime match. No write, no second read of unchanged bytes.
            stamp = source_stamp(src)
            dst_st = os.stat(dst)  # `verified` came from prepare_seed_delta, which confirmed this file's identity before deploy() started
            src_hashes[rel] = (verified[rel], dst_st.st_size, stamp[1] if stamp else None, dst_st.st_mtime_ns)
            skipped += 1
            continue
        if kind in ("kit", "intentional"):
            dst = str(contained_path(kit_root, rel, parents=kit_parents))
        if kind == "pdb" and not os.path.isfile(src):
            pdb_problem = f"source PDB missing: {src}"
            continue
        if rel.replace(os.sep, "/") in preserve and os.path.isfile(dst):
            if kind == "intentional" and receipts.digest(dst, force=full_verify, hasher=sha256_file) != expected:
                fail(f"REFUSING preserved intentional asset differs from declaration: {rel}")
            if kind == "exe" and sha256_file(src) != sha256_file(dst):
                fail(f"REFUSING preserved executable differs from source: {dst}")
            # user-owned file at target (e.g. hand-edited run-with-log.bat):
            # keep the deployed copy, record its hash so the manifest matches
            # what's actually on disk.
            p_hash, p_size, p_stamp = hash_with_stamp(dst, sha256_file)
            src_hashes[rel] = (p_hash, p_size, None, p_stamp)
            log(f"preserved {rel} (existing target copy kept)")
            continue
        def source_hash(path):
            if normalize:
                return hashlib.sha256(Path(path).read_bytes().replace(b"\r\n", b"\n")).hexdigest()
            return sha256_file(path)

        s_hash = receipts.digest(src, force=full_verify or kind == "exe", hasher=source_hash,
                                 variant='lf' if normalize else '')
        if expected is not None and s_hash != expected:
            fail(f"intentional asset source differs from declaration: {rel}")
        if (not full_verify or kind == "intentional") and os.path.isfile(dst):
            d_hash, d_size, d_stamp = hash_with_stamp(
                dst, lambda path: receipts.digest(path, force=full_verify or kind == "exe", hasher=sha256_file))
            if s_hash == d_hash:
                stamp = source_stamp(src)
                src_hashes[rel] = (d_hash, d_size, stamp[1], d_stamp)
                skipped += 1
                pdb_skipped = pdb_skipped or kind == "pdb"
                continue
        os.makedirs(os.path.dirname(dst) or target_dir, exist_ok=True)
        try:
            copy_started = time.perf_counter()
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(dir=os.path.dirname(dst), delete=False) as out:
                    temporary = out.name
                    if normalize:
                        # Normalize checkout line endings before publishing the sidecar.
                        out.write(Path(src).read_bytes().replace(b"\r\n", b"\n"))
                if not normalize:
                    shutil.copyfile(src, temporary)
                if kind == "exe" and os.name != "nt":
                    shutil.copymode(src, temporary)
                if expected is not None and sha256_file(temporary) != expected:
                    fail(f"intentional asset copied bytes differ from declaration: {rel}")
                replace_file(temporary, dst)
            finally:
                if temporary and os.path.exists(temporary):
                    os.unlink(temporary)
            copy_seconds += time.perf_counter() - copy_started
        except PermissionError as e:
            fail(f"copy refused (file locked?): {dst} ({e})\n"
                 "  A process is holding this file. Close it and re-run; "
                 "this script never taskkills.", 2)
        d_hash, d_size, d_stamp = hash_with_stamp(
            dst, lambda path: receipts.digest(path, force=True, hasher=sha256_file))
        if expected is not None and d_hash != expected:
            fail(f"intentional asset deployed bytes differ from declaration: {rel}")
        stamp = source_stamp(src)
        src_hashes[rel] = (d_hash, d_size,
                           stamp[1] if stamp is not None else None, d_stamp)
        if s_hash != d_hash:
            if kind == "exe":
                fail(f"post-copy exe hash MISMATCH: {rel}\n"
                     f"  source   {s_hash}\n  deployed {d_hash}\n"
                     "  Deployed exe is STALE (locked file / copy failure).", 3)
            elif kind == "shader":
                shader_mismatches.append(rel)
            elif kind == "pdb":
                pdb_problem = f"deployed PDB hash != source ({rel})"
            else:
                shader_mismatches.append(rel)  # DLLs hard-fail too
        log(f"deployed {rel} ({'OK' if s_hash == d_hash else 'MISMATCH'})")

    log(f"phase hash/verify seconds={time.perf_counter() - verify_started - copy_seconds:.3f}")
    log(f"phase copy seconds={copy_seconds:.3f}")
    if skipped:
        log(f"skipped {skipped} unchanged file(s) "
            f"(verified hash receipts match; --full-verify to re-copy)")

    if shader_mismatches:
        fail("post-copy diff MISMATCH (stale files in target):\n  "
             + "\n  ".join(shader_mismatches), 4)

    if pdb_problem:
        if allow_stale_pdb:
            log(f"WARNING (--allow-stale-pdb): {pdb_problem}")
        else:
            fail(f"{pdb_problem}\n  Stale PDB = wrong Tracy symbols "
                 "(Mistake D). Use --allow-stale-pdb to override.", 5)
    elif pdb_skipped:
        log("PDB check OK: unchanged (size+mtime_ns match the manifest)")
    else:
        log("PDB check OK: deployed PDB matches source hash")

    sweep_asides(target_dir)
    prune_retired_seed_payload(target_dir, preserve)
    retire_unowned(target_dir, src_hashes, preserve,
                   deploying_target=deploying_target,
                   own_shaders=any(kind == "shader" and rel.startswith("shaders/vulkan/") and rel.endswith(".spv")
                                   for _src, rel, kind in items))
    receipts.save()
    return src_hashes


def retire_unowned(target_dir, keep, preserve=(), own_shaders=False, deploying_target="game"):
    """Retire files the new payload (`keep`, its relpaths) no longer carries.

    Manifest rows of the previous deploy are retired only when unchanged. Paths are resolved before unlinking so a malformed
    manifest or a directory junction cannot escape the target. Unmanifested files are never deleted, including shaders
    and diagnostic output. Declared intentional assets are protected even when cooked source files are absent.
    `own_shaders` is retained for existing callers; it grants no additional ownership."""
    import asset_parity

    source_root = Path(__file__).resolve().parents[1]
    intentional = asset_parity.intentional_changes(git_head(source_root), source_root)
    root = Path(target_dir).resolve()
    shared = any(root == Path(preset["target_dir"]).resolve() for preset in TARGET_PRESETS.values())
    previous = read_manifest(target_dir)
    stamps = {rel: row[3] for rel, row in carry_rows(previous, target_dir).items()}
    preserved = {p.replace("\\", "/").strip("/").casefold() for p in preserve}
    preserved.update(p.casefold() for p in intentional)

    def protected(rel):
        candidates = [rel]
        resolved = (root / rel).resolve()
        if resolved.is_relative_to(root):
            candidates.append(resolved.relative_to(root).as_posix())
        for candidate in candidates:
            parts = candidate.replace("\\", "/").casefold().split("/")
            if (candidate.replace("\\", "/").casefold() == PAK_REL  # ensure_stock_pak owns the pak: a failed repack keeps the old one
                    or any(p in {"mods", "savegame", "captures", "_pngcap", "_rdoc"} for p in parts)
                    or parts[-1].endswith(".rdc")
                    or parts[-1] in {"options.cfg", "prefs.cfg"}
                    or any("/".join(parts) == p or "/".join(parts).startswith(p + "/") for p in preserved)):
                return True
        return False

    for rel, (digest, size, _mtime) in previous.items():
        if rel in keep or protected(rel):
            continue
        retired = root / rel
        resolved = retired.resolve()
        if not resolved.is_relative_to(root) or resolved == root:
            fail(f"retired payload path escapes target: {rel}")
        parts = tuple(part.casefold() for part in Path(rel).parts)
        if deploying_target == "game" and editor_only_payload(rel):
            if retired.is_file():
                keep[rel] = (digest, size, None, stamps.get(rel) or verified_stamp(retired, digest))  # earlier stamp, or one taken with a matching hash now
            continue
        if parts[0] in {"mods", "savegame"} or parts[:2] == ("data", "savegame"):
            continue
        # The game and editor presets share one install: each one's root executables, symbols and launchers are owned by
        # the other preset's payload, not retired by this one (2026-11-14: an editor deploy pruned mc2.exe/mc2.pdb).
        if not editor_only_payload(rel) and len(parts) == 1 and (parts[0].endswith((".exe", ".pdb")) or (parts[0].startswith("run-") and parts[0].endswith(".bat"))):
            if shared and retired.is_file():
                # Carry the previous claim, never bless changed bytes or authorize a source-mtime skip.
                keep[rel] = (digest, size, None, stamps.get(rel) or verified_stamp(retired, digest))
            continue
        if retired.is_file():
            rparts = tuple(part.casefold() for part in resolved.relative_to(root).parts)
            if ("crashes" in parts or "crashes" in rparts
                    or retired.suffix.casefold() in {".dmp", ".mdmp"}
                    or resolved.suffix.casefold() in {".dmp", ".mdmp"}):
                fail(f"archive crash evidence before deploying: {retired}")
            if sha256_file(retired) != digest:
                log(f"preserved modified retired payload: {rel}")
                continue
            retired.unlink()
            log(f"pruned retired payload: {rel}")


def hash_with_stamp(path, hasher):
    """(digest, size, dest_mtime_ns) of `path`. The stamp is the mtime_ns the file had around the hash; it is None when
    the file's size or mtime changed while it was being hashed, so a row can never carry a stamp for bytes it did not hash."""
    before = os.stat(path)
    digest = hasher(path)
    after = os.stat(path)
    stable = (before.st_mtime_ns, before.st_size) == (after.st_mtime_ns, after.st_size)
    return digest, after.st_size, after.st_mtime_ns if stable else None


def verified_stamp(path, digest):
    """dest_mtime_ns for a row that carries an earlier hash: sha256 the file now and stamp it only when it still matches."""
    try:
        got, _size, stamp = hash_with_stamp(path, sha256_file)
    except OSError:
        return None
    return stamp if got == digest else None


def restamp_rewritten(target_dir, hashes):
    """Run just before write_manifest, after every step that can touch the install (kit cook, asset sync, tree cook, SPIR-V,
    DLL fixes). A stamped row whose file no longer has the stamped size+mtime was rewritten after its hash was taken:
    re-hash it: same digest -> re-stamped at that time; different digest -> the row keeps its verified hash with no stamp and
    no source-mtime skip (never bless changed bytes; the pre-launch check reports it). Returns the rewritten rels."""
    rewritten = []
    for rel in sorted(hashes):
        row = tuple(hashes[rel])
        stamp = row[3] if len(row) > 3 else None
        if stamp is None:
            continue
        path = os.path.join(target_dir, rel)
        try:
            st = os.stat(path)
            if st.st_mtime_ns == stamp and st.st_size == row[1]:
                continue
            digest, size, new_stamp = hash_with_stamp(path, sha256_file)
        except OSError:
            hashes[rel] = (*row[:3], None)
            continue
        if digest == row[0]:
            hashes[rel] = (digest, size, row[2] if len(row) > 2 else None, new_stamp)  # same bytes rewritten: stamp valid now
        else:
            # Never bless changed bytes: keep the hash the deploy verified, drop the stamp, so the pre-launch check flags it.
            hashes[rel] = (*row[:2], None, None)
            log(f"WARNING: {rel} changed after its deploy hash was taken; the manifest keeps the verified hash and no stamp")
        rewritten.append(rel)
    if rewritten:
        log(f"{len(rewritten)} file(s) rewritten after their deploy hash: {', '.join(rewritten[:5])}")
    return rewritten


def write_manifest(target_dir, hashes, src_commit):
    ts = datetime.datetime.now().isoformat(timespec="seconds")
    path = os.path.join(target_dir, MANIFEST_NAME)
    pending = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", dir=target_dir, newline="", delete=False) as f:
            pending = f.name
            w = csv.writer(f)
            w.writerow(["manifest_version", MANIFEST_VERSION, "", "", "", ""])
            w.writerow(["relpath", "sha256", "bytes", "src_commit", "timestamp",
                        "mtime_ns", "dest_mtime_ns"])
            for rel in sorted(hashes):
                entry = tuple(hashes[rel])
                h, size = entry[0], entry[1]
                mtime_ns = entry[2] if len(entry) > 2 else None
                # dest_mtime_ns: the target file's own mtime captured WHEN its hash was taken (4th entry
                # field; hash_with_stamp / verified_stamp), so the pre-launch staleness check can skip
                # re-hashing an untouched file. Never stat here: a row without a stamp is written
                # without one, because a stamp taken now would bless whatever bytes are on disk now.
                dest_mtime_ns = entry[3] if len(entry) > 3 else None
                w.writerow([rel, h, size, src_commit, ts,
                            "" if mtime_ns is None else mtime_ns,
                            "" if dest_mtime_ns is None else dest_mtime_ns])
        replace_file(pending, path)
    finally:
        if pending and os.path.exists(pending):
            os.unlink(pending)
    log(f"manifest written: {path} ({len(hashes)} rows, {MANIFEST_VERSION}, "
        f"src_commit {src_commit[:12]})")


def hash_present_payload(items, target_dir):
    """Re-hash the payload files that are ALREADY in target_dir (no copy).

    Used by --write-manifest-only: another deploy path (deploy-editor.sh,
    the manual recipe, etc.) already copied the binaries in; we just record
    them in the SAME .deployed_manifest.csv that --verify-only checks, so
    the CSV does not go stale behind the JSON manifest.
    Files missing from the target are skipped (not every payload item is
    present in every target -- e.g. an editor-only deploy has no mc2.exe).
    """
    hashes = {}
    present, absent = 0, 0
    for item in items:
        src, rel, kind = item
        dst = os.path.join(target_dir, rel)
        if os.path.isfile(dst):
            # No mtime_ns: this path cannot prove the deployed bytes came from
            # the current source (another deploy path copied them), so the row
            # must never authorize a future skip. A real deploy fills it in.
            digest, size, stamp = hash_with_stamp(dst, sha256_file)
            if getattr(item, "declared_sha", digest) != digest:
                fail(f"manifest-only intentional asset differs from declaration: {rel}")
            hashes[rel] = (digest, size, None, stamp)
            present += 1
        else:
            absent += 1
    log(f"manifest-only: {present} payload files present in target, "
        f"{absent} absent (skipped)")
    return hashes


def write_manifest_only(src_root, build_dir, exe_name, pdb_name, target_dir,
                        build64_root=None, no_example_mods=False):
    """Refresh <target>/.deployed_manifest.csv from files ALREADY in target.

    No copy, no lock check (we are not writing binaries -- only the CSV).
    Merges with an existing CSV so a partial deploy (editor-only into a
    shared install) does not drop the game's rows, mirroring the JSON
    manifest's --merge behaviour.
    """
    if not os.path.isdir(target_dir):
        fail(f"target dir does not exist: {target_dir}")
    items = enumerate_payload(src_root, build_dir, exe_name, pdb_name,
                              require_build=False, build64_root=build64_root,
                              no_example_mods=no_example_mods)
    declared = declared_assets(src_root)
    from kit_payload import contained_path
    items = [item for item in items if item[1] not in declared]
    items += [IntentionalItem(contained_path(target_dir, rel), rel, sha) for rel, sha in declared.items()]
    hashes = hash_present_payload(items, target_dir)

    # Merge: keep prior rows for payload files we did not just re-hash but
    # that still exist on disk (e.g. game exe rows when refreshing after an
    # editor-only copy).
    existing = os.path.join(target_dir, MANIFEST_NAME)
    if os.path.isfile(existing):
        try:
            with open(existing, newline="") as f:
                rows = list(csv.reader(f))
            if rows and rows[0][:2] == ["manifest_version", MANIFEST_VERSION]:
                for row in rows[2:]:
                    if len(row) < 3:
                        continue
                    rel, h, size = row[0], row[1], row[2]
                    if rel in hashes:
                        continue  # freshly re-hashed wins
                    p = os.path.join(target_dir, rel)
                    if os.path.isfile(p):
                        hashes[rel] = (h, size, None, verified_stamp(p, h))  # carried hash: stamped only if the file still matches it
        except Exception as e:
            log(f"WARNING: could not merge existing CSV manifest ({e}); replacing")

    if not hashes:
        fail("no payload files present in target -- nothing to record")

    # LINK-GUARD-1: this path writes no binaries, but it DOES write
    # .deployed_manifest.csv -- and in the 2026-07-30 incident that file was
    # itself hardlinked four ways (nlink=4). write_manifest() below opens it
    # "w", which truncates the shared inode, so a manifest refresh here would
    # silently overwrite three other installs' manifests with this tree's
    # hashes: every one of them would then read as coherent while describing
    # somebody else's payload. Guarding only the copy path would have left the
    # cheapest way to forge a clean manifest wide open.
    check_hardlink_isolation([(None, MANIFEST_NAME, "manifest")], target_dir)

    write_manifest(target_dir, hashes, git_head(src_root))
    log("manifest-only COMPLETE -- .deployed_manifest.csv refreshed (no copy)")


PRELAUNCH_NOTICE_SECONDS = 5.0
_UNSTAMPED_NOTED = set()


def carry_rows(rows, target_dir):
    """Previous-manifest rows re-emitted without re-hashing: each keeps the dest_mtime_ns the earlier deploy recorded (None when
    it had none), never a stamp taken now, so write_manifest cannot bless bytes that changed since they were hashed."""
    try:
        with open(os.path.join(target_dir, MANIFEST_NAME), newline="", encoding="utf-8") as f:
            stamps = {r[0]: int(r[6]) for r in list(csv.reader(f))[2:] if len(r) >= 7 and r[6].isdigit()}
    except (OSError, csv.Error):
        stamps = {}
    return {rel: (*tuple(row)[:3], stamps.get(rel)) for rel, row in rows.items()}


def staleness_report(target_dir, full=False, notice=None):
    """Pure: compare a deployed tree against its .deployed_manifest.csv. Returns a dict --
    NO sys.exit -- so callers other than the CLI (run_smoke) can surface drift without
    reimplementing the hashing/compare:

      {has_manifest, version_ok, ok (int), stale [rel...], missing [rel...],
       src_commit, manifest_path, hashed (int), skipped (int)}

    Fast path (default): a file whose size and mtime_ns equal the dest_mtime_ns the deploy
    recorded next to its hash is trusted, not re-hashed; anything else (changed stamp, no
    stamp, e.g. a manifest written before dest_mtime_ns existed) is sha256-hashed as before.
    full=True hashes every file (provenance callers: merge_gate, --verify-only).
    notice(msg) is called once when the hashing runs longer than PRELAUNCH_NOTICE_SECONDS
    (default: print to stderr) so the wait is never silent."""
    path = os.path.join(target_dir, MANIFEST_NAME)
    rep = {"manifest_path": path, "has_manifest": False, "version_ok": True,
           "ok": 0, "stale": [], "missing": [], "src_commit": None, "hashed": 0, "skipped": 0}
    if not os.path.isfile(path):
        return rep
    rep["has_manifest"] = True
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    if not rows or rows[0][:2] != ["manifest_version", MANIFEST_VERSION]:
        rep["version_ok"] = False
        return rep
    if notice is None:
        def notice(msg):
            print(msg, file=sys.stderr, flush=True)
    started = time.monotonic()
    announced = False
    unstamped = 0
    data = [row for row in rows[2:] if len(row) >= 3]
    for n, row in enumerate(data):
        rel, expect_hash = row[0], row[1]
        if rep["src_commit"] is None and len(row) >= 4:
            rep["src_commit"] = row[3]
        p = os.path.join(target_dir, rel)
        try:
            st = os.stat(p)
        except OSError:
            st = None
        if st is None or not stat.S_ISREG(st.st_mode):
            rep["missing"].append(rel)
            continue
        stamp = int(row[6]) if len(row) >= 7 and row[6].isdigit() else None
        if stamp is None:
            unstamped += 1
        elif not full and stamp == st.st_mtime_ns and row[2].isdigit() and int(row[2]) == st.st_size:
            rep["skipped"] += 1
            rep["ok"] += 1
            continue
        if not announced and time.monotonic() - started > PRELAUNCH_NOTICE_SECONDS:
            announced = True
            notice(f"[PRELAUNCH] hashing install files against the manifest ({len(data) - n} left of {len(data)}, "
                   f"{target_dir}); this can take minutes on a cold disk")
        rep["hashed"] += 1
        if sha256_file(p) != expect_hash:
            rep["stale"].append(rel)
        else:
            rep["ok"] += 1
    if unstamped and not full and target_dir not in _UNSTAMPED_NOTED:
        _UNSTAMPED_NOTED.add(target_dir)  # once per process per install
        notice(f"[PRELAUNCH] {target_dir}: {unstamped} manifest rows carry no dest_mtime_ns (deployed before the stamp "
               f"existed, or carried over unverified), so they are sha256-hashed on every check; a deploy that "
               f"re-verifies them records their stamps")
    return rep


REQUIRED_DIRS = ("assets", "data", "shaders")


def _pe_imports(path):
    """Imported + delay-loaded DLL names (lowercase) of a PE file. Raises RuntimeError when pefile is missing (py -3 -m pip
    install pefile) or the file is not a readable PE: an empty answer would let the completeness check pass having parsed nothing."""
    try:
        import pefile
    except ImportError as e:
        raise RuntimeError("the install completeness check needs pefile (py -3 -m pip install pefile)") from e
    try:
        pe = pefile.PE(path, fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
                                               pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"]])
        names = {e.dll.decode().lower() for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])}
        names |= {e.dll.decode().lower() for e in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", [])}
        pe.close()
        return names
    except Exception as e:  # noqa: BLE001
        raise RuntimeError(f"cannot read PE imports of {path}: {e}") from e


def check_install_complete(target, exe_name, build_dir, fix=True):
    """Every non-system DLL the exe (transitively) imports must sit next to it, and assets/ data/ shaders/ must exist and be non-empty.
    Missing DLLs are copied from build_dir (fix=True); returns a sorted list of what is still missing (DLL names and 'dir/')."""
    if exe_name == "mc2":
        exe = os.path.join(target, exe_name)
        if not os.path.isfile(exe):
            return [f"{exe_name} (not found in target)"]
        result = subprocess.run(["ldd", exe], capture_output=True, text=True,
                                **console_child.child_kwargs(redirected=True))
        if result.returncode:
            return [f"cannot read ELF dependencies: {result.stderr.strip() or result.stdout.strip()}"]
        missing = [line.split()[0] for line in result.stdout.splitlines() if "=> not found" in line]
        for name in REQUIRED_DIRS:
            directory = os.path.join(target, name)
            if not (os.path.isdir(directory) and any(os.scandir(directory))):
                missing.append(name + "/")
        return sorted(missing)
    sysdirs = [os.path.join(os.environ.get("SystemRoot", "C:/Windows"), d) for d in ("System32", "SysWOW64")]
    runtimes = {d.lower() for d in RUNTIME_DLLS}

    def is_system(n):
        return n not in runtimes and (n.startswith(("api-ms-", "ext-ms-")) or any(os.path.exists(os.path.join(d, n)) for d in sysdirs))

    exe = next((c for c in (os.path.join(target, exe_name), os.path.join(build_dir, exe_name)) if os.path.isfile(c)), None)
    if exe is None:
        return [f"{exe_name} (not found in target or build dir)"]
    try:
        todo = (_pe_imports(exe) if exe else set()) | runtimes
        for name in ("mc2_asset_viewer.exe", "ui_editor.exe", "mc2fx_preview.exe", "mc2weapon_gui.exe"):
            tool = os.path.join(target, name)
            if os.path.isfile(tool):
                todo |= _pe_imports(tool)
    except RuntimeError as e:
        return [str(e)]
    if fix:
        seed_build_dlls(build_dir, repo_root())
    seen, missing = set(), []
    while todo:
        n = todo.pop()
        if n in seen or is_system(n):
            continue
        seen.add(n)
        dst, src = os.path.join(target, n), os.path.join(build_dir, n)
        if not os.path.isfile(dst):
            if fix and os.path.isfile(src):
                shutil.copy2(src, dst)
                log(f"completeness: copied missing {n} from build dir")
            else:
                missing.append(n)
                continue
        try:
            todo |= _pe_imports(dst)
        except RuntimeError as e:
            missing.append(str(e))
    for d in REQUIRED_DIRS:
        dpath = os.path.join(target, d)
        if not (os.path.isdir(dpath) and any(os.scandir(dpath))):
            missing.append(d + "/")
    return sorted(missing)


def verify_only(target_dir, strict):
    rep = staleness_report(target_dir, full=True)
    if not rep["has_manifest"]:
        msg = (f"no manifest at {rep['manifest_path']} -- target never deployed "
               "via this tool")
        if strict:
            fail(msg)
        log(f"ADVISORY: {msg}")
        return 0
    if not rep["version_ok"]:
        fail(f"unrecognized manifest version in {rep['manifest_path']}")
    stale, missing, ok = rep["stale"], rep["missing"], rep["ok"]
    log(f"verify: {ok} match, {len(stale)} stale, {len(missing)} missing")
    for r in stale:
        log(f"  STALE:   {r}")
    for r in missing:
        log(f"  MISSING: {r}")
    if (stale or missing):
        if strict:
            fail("target drifted from manifest (--strict)", 6)
        log("ADVISORY: target drifted from manifest (exit 0; use --strict to gate)")
    else:
        log("target matches manifest")
    return 0


# Canned target presets (--target). Presets only SET DEFAULTS; explicit
# positional target / --build-dir / --exe-name always override.
#   game   -> live game install   A:/Games/mc2-opengl/mc2-win64-v0.4, mc2.exe,
#             build64/RelWithDebInfo
#   editor -> live editor install A:/Games/mc2-opengl/mc2-win64-v0.4 (dev install; 2026-10-05 reset),
#             "Mission Editor.exe", build64/out/editor/RelWithDebInfo (EditRel)
TARGET_PRESETS = {
    "game": {
        "target_dir": "A:/Games/mc2-opengl/mc2-win64-v0.4",
        "exe_name": "mc2.exe" if sys.platform == "win32" else "mc2",
        "build_subdir": os.path.join("build64", "RelWithDebInfo"),
        "ninja_subdir": "build64-ninja",
    },
    "editor": {
        "target_dir": "A:/Games/mc2-opengl/mc2-win64-v0.4",
        "exe_name": "Mission Editor.exe",
        "build_subdir": os.path.join("build64", "out", "editor", "RelWithDebInfo"),
        "ninja_subdir": os.path.join("build64-ninja", "out", "editor"),
    },
}

def is_v04_game_publish(target, exe_name):
    """True when this deploy lands the game exe in the v0.4 preset install, however the target was spelled."""
    game = TARGET_PRESETS["game"]
    try:
        same_dir = os.path.normcase(os.path.realpath(target)) == os.path.normcase(os.path.realpath(game["target_dir"]))
    except (OSError, TypeError, ValueError):
        return False
    return same_dir and os.path.basename(str(exe_name)).casefold() in ("mc2", "mc2.exe")


# DEPLOY-TARGET-GUARD: deploy ONLY to one of the canonical deploy folders. Each full
# install is ~5GB; spinning up a per-lane/per-experiment deploy folder (e.g.
# mc2-<lane>-test) bloats A:/Games/mc2-opengl by tens of GB. Deploy is REFUSED to any
# target not in this set unless --allow-new-target is passed (deliberate new install /
# release cut). The folders below are the shared dev deploy targets -- pick a free one
# (md5-verify after; a sibling may hold the lease).
DEPLOY_ALLOWLIST = {
    "mc2-win64-v0.4",
    "mc2-win64-lane2",
    "mc2-win64-lane3",
}


def refuse_golden_target(target):
    """Golden installs are read-only, including through a junction or override."""
    if target and any(p.name.casefold() in {"mc2-win64-v0.4c", "mc2-win64-0.4c"}
                      for p in (Path(target), *Path(target).parents, Path(target).resolve(), *Path(target).resolve().parents)):
        fail(f"golden install is never a deploy target: {target}")


def implicit_target_refusal(src_root, explicit_target):
    """A lane worktree (A:/Games/mc2-<lane>, not mc2-main) must name its own install; None when the deploy may proceed."""
    if explicit_target:
        return None
    root = (ntpath.normpath(src_root) if ntpath.splitdrive(src_root)[0]
            else os.path.normpath(os.path.abspath(src_root))).replace("\\", "/")
    parent, name = os.path.split(root)
    if parent.lower() == "a:/games" and name.lower().startswith("mc2-") and name.lower() != "mc2-main":
        return (f"no deploy target named, and {name} is a lane worktree: the implicit default install is mc2-main's. "
                f"Name your own install, e.g. G:/mc2-installs/<lane> with --allow-new-target.")
    return None


BOOTSTRAP_SKIP = {"cache", "crashes", "shaders", "debug_state", "_pngcap", "_rdoc", MANIFEST_NAME, RETIRED_LAUNCHER}


def bootstrap_install(src, target, exe_name, pdb_name):
    """One-time creation of a NEW lane install: copy the base files (data, assets, fst archives, DLLs, cfg) from an existing
    install; the deploy that follows adds the exe, launcher, pdb, shaders and manifest. Real copies (never hardlinks: LINK-GUARD-1)."""
    src, target = os.path.abspath(src), os.path.abspath(target)
    if not (os.path.isdir(os.path.join(src, "data")) and os.path.isdir(os.path.join(src, "assets"))):
        fail(f"--bootstrap-from {src}: not an install (needs data/ and assets/)")
    if _same_path(src, target):
        fail("--bootstrap-from names the target itself")
    if os.path.isdir(target) and any(os.scandir(target)):
        log(f"bootstrap: {target} already has files; not copying from {src}")
        return
    skip = BOOTSTRAP_SKIP | {exe_name, pdb_name}
    from asset_parity import is_deploy_artifact
    names = [n for n in sorted(os.listdir(src)) if n not in skip and not is_deploy_artifact(n)
             and not n.lower().endswith(".pdb")]
    if exe_name == "mc2":
        names = [n for n in names if n != ".mc2_deploy_lane.json"
                 and not n.lower().endswith((".exe", ".dll", ".bat"))]
    need = sum(os.path.getsize(p) for n in names for p in
               ([os.path.join(src, n)] if os.path.isfile(os.path.join(src, n)) else
                (os.path.join(d, f) for d, _, fs in os.walk(os.path.join(src, n)) for f in fs)))
    rc, _ = disk_space(os.path.dirname(target) or target, need, label="bootstrap")
    if rc == 3:
        fail(f"bootstrap: not enough free space for {need / 2**30:.1f} GiB", 3)
    log(f"bootstrap: copying {len(names)} entries ({need / 2**30:.1f} GiB) {src} -> {target}")
    parent, basename = os.path.dirname(target), os.path.basename(target)
    os.makedirs(parent, exist_ok=True)
    for existing in Path(parent).glob(basename + ".partial*"):
        log(f"bootstrap: preserved pre-existing staging path: {existing}")
    partial = tempfile.mkdtemp(prefix=basename + ".partial-", dir=parent)
    try:
        for n in names:
            a, b = os.path.join(src, n), os.path.join(partial, n)
            if os.path.isdir(a):
                shutil.copytree(a, b, ignore=lambda directory, names: [name for name in names
                    if is_deploy_artifact(Path(directory, name).relative_to(src).as_posix())])
            else:
                shutil.copy2(a, b)
        if os.path.isdir(target):
            os.rmdir(target)  # empty (checked above)
        os.rename(partial, target)
    except BaseException:
        shutil.rmtree(partial)
        raise


def published_shader_sources(source_root, build):
    """The shaders/ inventory a published install must hold exactly: every source file except fixtures plus the build's baked SPIR-V."""
    source_root, build = Path(source_root), Path(build)
    expected = {p.relative_to(source_root).as_posix(): p
                for p in (source_root / 'shaders').rglob('*')
                if p.is_file() and 'fixtures' not in p.relative_to(source_root / 'shaders').parts}
    baked = baked_spv_files(build)
    if not baked:
        raise ValueError(f'{build}: missing baked Vulkan shaders')
    expected.update({'shaders/vulkan/' + p.name: p for p in baked})
    return expected


def shader_inventory_problems(source_root, published):
    """Dry run of the deploy row's shader half on a copy of the published install's shaders/ (about 2 MB): what the deploy
    ships, minus what it retires, must equal what verify_published_identity expects. Laptop-gate fast red (W129 run 2 found
    this mismatch only after contracts, harnesses and capture-before). Returns problem lines."""
    source_root, build = Path(source_root), Path(source_root) / 'build64-ninja'
    try:
        expected = published_shader_sources(source_root, build)
        items = enumerate_payload(str(source_root), str(build), 'mc2.exe', 'mc2.pdb', build64_root=str(build), cook=False)
    except (ValueError, SystemExit) as exc:
        return [f'shader inventory: {exc}']
    shipped = {rel: src for src, rel, _kind in items if rel.startswith('shaders/')}
    bad = []
    if set(shipped) != set(expected):
        bad.append(f'shader inventory: deploy ships {sorted(set(shipped) - set(expected))[:5]} extra, '
                   f'{sorted(set(expected) - set(shipped))[:5]} missing vs verify_published_identity')
    with tempfile.TemporaryDirectory(prefix='shader-inventory-') as tmp:
        if (Path(published) / 'shaders').is_dir():
            shutil.copytree(Path(published) / 'shaders', Path(tmp) / 'shaders')
        # Ownership is required to retire unchanged shader sources, just as in a real deploy.
        manifest = Path(published) / MANIFEST_NAME
        if manifest.is_file():
            shutil.copyfile(manifest, Path(tmp) / MANIFEST_NAME)
        hashes = deploy([(src, rel, kind) for src, rel, kind in items if rel in shipped],
                        tmp, False, code_only=True, verify_normals=False)
        write_manifest(tmp, hashes, "shader-inventory")
        left = {p.relative_to(tmp).as_posix() for p in (Path(tmp) / 'shaders').rglob('*') if p.is_file()}
    if left != set(expected):
        bad.append(f'shader inventory: after deploy {published} would hold {sorted(left - set(expected))[:5]} extra, '
                   f'{sorted(set(expected) - left)[:5]} missing (published shader inventory != HEAD build)')
    return bad


def require_pak_ok(target):
    """Post-publish verdict, not a guard: raise unless <target>/.pak_deploy_status (written by the publish) starts with OK, so a v0.4
    publish that would read FSTs is reported failed. The pre-publish checks are in ensure_stock_pak (the cached pak is judged before it
    is copied) and deploy_from_payload (the incoming pak is judged before it overwrites the installed one)."""
    try:
        status = (Path(target) / PAK_STATUS_NAME).read_text(encoding="utf-8").strip()
    except OSError:
        status = "missing"
    if not status.startswith("OK"):
        raise ValueError(f"{target}: pak deploy status is not OK ({status[:200]}); the published install would read FSTs")


def verify_published_identity(target, source_root, head, require_pak=False):
    """Hash the published exe/shaders against the clean HEAD build, not its own CSV. require_pak (the v0.4 publish): also
    refuse a FALLBACK/SKIP/missing .pak_deploy_status."""
    from exe_fingerprint import extract, sha_agrees, build_current
    target, source_root = Path(target), Path(source_root)
    if require_pak:
        require_pak_ok(target)
    build = source_root / 'build64-ninja'
    for exe in (build / 'mc2.exe', target / 'mc2.exe'):
        fp = extract(exe)
        if not fp.get('present') or not build_current(fp.get('sha'), head, source_root, bool(fp.get('dirty'))):
            raise ValueError(f'{exe}: fingerprint != clean HEAD {head}')
    expected = {'mc2.exe': build / 'mc2.exe'}
    expected.update(published_shader_sources(source_root, build))
    with (target / '.deployed_manifest.csv').open(encoding='utf-8-sig', newline='') as stream:
        if next(csv.reader(stream), [])[:2] != ['manifest_version', 'v1']:
            raise ValueError('unsupported published manifest')
        rows = {row['relpath']: row for row in csv.DictReader(stream)}
    for rel, src in expected.items():
        row = rows.get(rel, {})
        dst = target / rel
        if (not sha_agrees(row.get('src_commit'), head) or not dst.is_file()
                or int(row.get('bytes', -1)) != dst.stat().st_size
                or sha256_file(str(src)) != row.get('sha256') or sha256_file(str(dst)) != row.get('sha256')):
            raise ValueError(f'{dst}: published bytes/manifest != HEAD build')
    # Every other manifest row (DLLs, data/, 3rdparty) is byte-checked on the deployed file too: the deploy's fast path and the
    # seed/reuse paths trust size+mtime, so a swapped DLL or data file was invisible here (meta-0220 P1#5). ~15 s for 5.6 GB warm.
    for rel, row in rows.items():
        if rel in expected:
            continue
        dst = target / rel
        if not dst.is_file() or int(row.get('bytes', -1)) != dst.stat().st_size or sha256_file(str(dst)) != row.get('sha256'):
            raise ValueError(f'{dst}: published bytes != manifest row')
    deployed_shaders = {p.relative_to(target).as_posix() for p in (target / 'shaders').rglob('*') if p.is_file()}
    if deployed_shaders != {rel for rel in expected if rel.startswith('shaders/')}:
        raise ValueError(f'{target}: published shader inventory != HEAD build')


def deploy_from_payload(payload, target, src_root, head):
    """Publish a payload built and gated elsewhere (tools/laptop_gate.py): payload/files/<rel> holds every file
    that differs from this install, payload/.deployed_manifest.csv is the full manifest. Every copied byte is
    sha256-checked against its manifest row, untouched files are sha256-checked, the exe fingerprint must be a clean HEAD build."""
    from exe_fingerprint import extract, build_current
    payload, target = Path(payload), Path(target)
    manifest = payload / '.deployed_manifest.csv'
    with manifest.open(encoding='utf-8-sig', newline='') as stream:
        if next(csv.reader(stream), [])[:2] != ['manifest_version', 'v1']:
            fail('unsupported payload manifest')
        rows = {row['relpath']: row for row in csv.DictReader(stream)}
    from asset_parity import is_deploy_artifact
    if any(is_deploy_artifact(rel) for rel in rows):
        fail("payload contains diagnostic or backup artifacts; republish a clean payload")
    staged = payload / 'files'
    stamps = {}  # rel -> dest_mtime_ns taken with the destination's own sha256, never inherited from the payload
    refused = None  # why the incoming pak was not installed (it is judged before it can overwrite the pak that is there)
    for rel, row in sorted(rows.items(), key=lambda item: item[0] == PAK_REL):  # the pak last: its FSTs may be in this payload
        src, dst = staged / rel, target / rel
        if src.is_file():
            if sha256_file(str(src)) != row['sha256']:
                fail(f'payload file {rel}: sha256 != manifest')
            if rel == PAK_REL and registered_fsts(target) is not None and (refused := pak_defect(src, target)):
                log(f"WARN [PAK_DEPLOY] incoming payload pak refused ({refused}); the installed pak is kept")
                continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            fd, tmp = tempfile.mkstemp(prefix=dst.name + '.payload-', dir=dst.parent)
            os.close(fd)
            try:
                shutil.copy2(src, tmp)
                replace_file(tmp, dst)
            finally:
                unlink_for_replace(tmp)
            copied = True
        elif not dst.is_file() or dst.stat().st_size != int(row['bytes']):
            fail(f'{rel}: not in the payload and the target copy does not match the manifest (size or sha256); the PC v0.4 drifted since the delta was taken')
        else:
            copied = False
        digest, _size, stamps[rel] = hash_with_stamp(dst, sha256_file)  # copied or untouched: the destination's own bytes
        if digest != row['sha256']:
            fail(f'{rel}: ' + ('copied file does not match the manifest sha256' if copied else
                 'not in the payload and the target copy does not match the manifest (size or sha256); the PC v0.4 drifted since the delta was taken'))
    exe = target / 'mc2.exe'
    fp = extract(exe)
    if (sha256_file(str(exe)) != rows['mc2.exe']['sha256'] or not fp.get('present')
            or not build_current(fp.get('sha'), head, Path(src_root), bool(fp.get('dirty')))):
        fail(f'{exe}: fingerprint != clean HEAD {head}')
    # The incoming pak was packed elsewhere: judge it against the FSTs now installed here and record the outcome.
    # An incoming pak that failed the check above never overwrote the installed one; the installed pak (carried by the payload
    # or not in its manifest at all) is judged too, and recorded as it is on disk.
    if refused:
        rows.pop(PAK_REL, None)
    on_disk = (target / PAK_REL).is_file()
    defect = quarantine_bad_pak(target) if on_disk else None
    carried = None
    if defect:
        rows.pop(PAK_REL, None)
        stamps.pop(PAK_REL, None)
        status = f"FALLBACK payload pak refused and moved aside ({defect})" + (f"; incoming refused ({refused})" if refused else "")
    elif on_disk:
        if PAK_REL not in rows:
            carried = hash_with_stamp(target / PAK_REL, sha256_file)
        if registered_fsts(target) is None:
            status = "SKIP pak not judged (no FSTs)"
        else:
            status = f"OK kept installed pak (incoming refused: {refused})" if refused else "OK payload pak checked"
    else:
        status = f"FALLBACK incoming pak refused ({refused}); no installed pak" if refused else "SKIP no pak in the payload"
    write_pak_status(target, status)
    hashes = {rel: (row['sha256'], int(row['bytes']), None, stamps[rel]) for rel, row in rows.items()}
    if carried:
        hashes[PAK_REL] = (carried[0], carried[1], None, carried[2])
    retire_unowned(str(target), hashes, own_shaders=any(rel.startswith('shaders/vulkan/') and rel.endswith('.spv') for rel in rows))
    dst = target / '.deployed_manifest.csv'
    fd, tmp = tempfile.mkstemp(prefix=dst.name + '.payload-', dir=dst.parent)
    os.close(fd)
    tmp = Path(tmp)
    try:
        with manifest.open(encoding='utf-8-sig', newline='') as stream:
            header = next(csv.reader(stream))
        with tmp.open('w', newline='', encoding='utf-8') as stream:
            writer = csv.writer(stream)
            writer.writerow(header)
            writer.writerow(['relpath', 'sha256', 'bytes', 'src_commit', 'timestamp', 'mtime_ns', 'dest_mtime_ns'])
            for rel, row in rows.items():
                writer.writerow([rel, row['sha256'], row['bytes'], row.get('src_commit') or '', row.get('timestamp') or '',
                                 row.get('mtime_ns') or '', stamps[rel] if stamps[rel] is not None else ''])
            for rel in sorted(hashes.keys() - rows.keys()):  # carried claims (editor / shared-exe rows): their own stamp or none
                digest, size, _src, stamp = hashes[rel]
                writer.writerow([rel, digest, size, '', '', '', '' if stamp is None else stamp])
        replace_file(tmp, dst)
    finally:
        unlink_for_replace(tmp)
    log(f'payload published: {len(hashes)} manifest rows, exe sha256 {rows["mc2.exe"]["sha256"]}, fingerprint matches {head[:9]}')


def _target_preset(value):
    if value not in TARGET_PRESETS:
        hint = (f" '{value}' looks like a directory: pass it as the positional target_dir "
                f"(deploy_payload.py {value} --ninja ... [--allow-new-target]), not as --target." if re.search(r"[\\/:]", value) else "")
        raise argparse.ArgumentTypeError(f"--target takes a preset name ({', '.join(sorted(TARGET_PRESETS))}).{hint}")
    return value


def declared_assets(src_root):
    import asset_parity
    return asset_parity.intentional_changes(git_head(src_root), Path(src_root))


def seed_asset_store(src_root, install, store):
    """Archive only bytes matching HEAD's declarations; inputs are read-only."""
    from kit_payload import contained_path
    store = Path(store)
    store.mkdir(parents=True, exist_ok=True)
    seeded = 0
    for rel, expected in declared_assets(src_root).items():
        blob = contained_path(store, expected)
        if blob.is_file() and sha256_file(blob) == expected:
            continue
        source = next((p for root in (install, src_root)
                       if (p := contained_path(root, rel)).is_file()
                       and sha256_file(p) == expected), None)
        if source is None:
            log(f"asset store unavailable in seed: {rel} sha256={expected}")
            continue
        pending = None
        try:
            with tempfile.NamedTemporaryFile(dir=store, delete=False) as stream:
                pending = Path(stream.name)
                with source.open("rb") as incoming:
                    shutil.copyfileobj(incoming, stream)
            if sha256_file(pending) != expected:
                fail(f"asset store seed changed during copy: {rel}")
            os.replace(pending, blob)
        finally:
            if pending is not None:
                pending.unlink(missing_ok=True)
        seeded += 1
    log(f"asset store: seeded {seeded} blobs from {install}")


class IntentionalItem(tuple):
    """Keep the three-field payload interface with its declared hash attached."""
    def __new__(cls, source, rel, sha):
        item = super().__new__(cls, (str(source), rel, "intentional"))
        item.declared_sha = sha
        return item


def intentional_payload(src_root, target, store, dry_run=False):
    """Resolve all declarations before copying; never trust a blob's filename."""
    from kit_payload import HashReceipts, contained_path
    receipts = HashReceipts(Path(target) / ".deploy-receipts.json")
    def digest(path):
        return receipts.digest(path, force=dry_run, hasher=sha256_file)
    items, errors = [], []
    missing = mismatched = 0
    for rel, expected in declared_assets(src_root).items():
        dst = contained_path(target, rel)
        exists = dst.is_file()
        current = exists and digest(dst) == expected
        if not current:
            missing += not exists
            mismatched += exists
            log(f"intentional asset {'MISMATCH' if exists else 'MISSING'}: {rel} sha256={expected}")
        source = contained_path(src_root, rel)
        blob = contained_path(store, expected)
        resolved = next((p for p in (source, blob, dst) if p.is_file()
                         and digest(p) == expected), None)
        if resolved is None:
            errors.append(f"{rel} sha256={expected} (store miss or corrupt blob: {blob})")
        else:
            items.append(IntentionalItem(resolved, rel, expected))
    log(f"intentional assets: {len(items)} resolved, {missing} missing, {mismatched} mismatched, {len(errors)} unresolved"
        + (" (dry-run; no writes)" if dry_run else ""))
    if errors:
        fail("intentional asset resolution FAILED before payload copy:\n  " + "\n  ".join(errors))
    if not dry_run:
        receipts.save()
    return items


def deploy_shaders_only(src_root, build_dir, target, exe_name):
    """Rebake Vulkan shaders without relinking; preserve every unrelated manifest row."""
    target = Path(target)
    manifest = target / MANIFEST_NAME
    rows = read_manifest(target)
    if exe_name not in rows or sha256_file(target / exe_name) != rows[exe_name][0]:
        fail("--shaders-only needs an existing, manifest-verified executable")
    if not (Path(build_dir) / "build.ninja").is_file():
        fail("--shaders-only requires an existing Ninja build; pass --ninja or --build-dir")
    tree = Path(build_dir).resolve().relative_to(Path(src_root).resolve())
    subprocess.run([sys.executable, str(Path(src_root) / "scripts/ninja_build.py"), "mc2_vulkan_shaders"],
                   cwd=src_root, env=dict(os.environ, MC2_NINJA_TREE=str(tree)), check=True,
                   **console_child.child_kwargs(redirected=True))
    items = [(str(p), "shaders/vulkan/" + p.name, "shader") for p in baked_spv_files(build_dir)]
    if not items or {rel for _, rel, _ in items} != {
            rel for rel in rows if rel.startswith("shaders/vulkan/") and rel.endswith(".spv")}:
        fail("--shaders-only requires the existing shader inventory; full deploy first")
    # Only the shaders that differ are written (link-safe replace_file); unchanged seed links stay shared.
    check_hardlink_isolation([i for i in items if sha256_file(i[0]) != rows[i[1]][0]]
                             + [(None, MANIFEST_NAME, "manifest")], str(target))
    hashes = {}
    for src, rel, _ in items:
        dst = target / rel
        if sha256_file(dst) != rows[rel][0]:
            fail(f"shader differs from manifest: {rel}; full deploy first")
        digest = sha256_file(src)
        if digest == rows[rel][0]:
            continue
        pending = None
        try:
            with tempfile.NamedTemporaryFile(dir=dst.parent, delete=False) as f:
                pending = f.name
            shutil.copyfile(src, pending)
            if sha256_file(pending) != digest:
                fail(f"shader copy failed verification: {rel}")
            replace_file(pending, dst)
        finally:
            if pending and os.path.exists(pending):
                os.unlink(pending)
        got, size, stamp = hash_with_stamp(dst, sha256_file)
        if got != digest:
            fail(f"deployed shader failed verification: {rel}")
        hashes[rel] = (got, size, os.stat(src).st_mtime_ns, stamp)
    if not hashes:
        log("shaders unchanged; manifest retained")
        return
    with manifest.open(newline="", encoding="utf-8") as f:
        csv_rows = list(csv.reader(f))
    head = git_head(src_root) + "-shader-dirty"
    now = datetime.datetime.now().isoformat(timespec="seconds")
    for row in csv_rows[2:]:
        if row and row[0] in hashes:
            h, size, src_stamp, dst_stamp = hashes[row[0]]
            row[:] = [row[0], h, str(size), head, now, str(src_stamp or ""), str(dst_stamp or "")]
    pending = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", dir=target, newline="", delete=False) as f:
            pending = f.name
            csv.writer(f).writerows(csv_rows)
        replace_file(pending, manifest)
    finally:
        if pending and os.path.exists(pending):
            os.unlink(pending)
    log(f"[DEPLOY_FINGERPRINT] shader-dirty source={head} shaders={len(hashes)} exe=unchanged")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        epilog="Presets: --target game   = A:/Games/mc2-opengl/mc2-win64-v0.4 "
               "(mc2.exe, build64/RelWithDebInfo); "
               "--target editor = A:/Games/mc2-opengl/mc2-win64-v0.4 "
               "(\"Mission Editor.exe\", build64/out/editor/RelWithDebInfo). "
               "Explicit target dir / --build-dir / --exe-name override preset "
               "defaults.")
    ap.add_argument("target_dir", nargs="?", default=None,
                    help="deploy target dir (e.g. "
                    "A:/Games/mc2-opengl/mc2-win64-v0.4); optional when "
                    "--target preset is given (and then overrides the preset dir)")
    ap.add_argument("--target", type=_target_preset,
                    help="canned target preset: 'game' (v0.4, mc2.exe) or "
                    "'editor' (dev v0.4, Mission Editor.exe, EditRel build dir). "
                    "Sets defaults only; explicit flags still override.")
    ap.add_argument("--classic-assets", action="store_true",
                    help="deploy without the Remastered building kit (also honors RemasteredBuildings=FALSE)")
    ap.add_argument('--seed-owner-test', action='store_true',
                    help='seed target_dir from --seed-source after verifying against --source-root HEAD; no build or cook')
    ap.add_argument('--seed-source', type=Path,
                    help='published install to verify and copy with --seed-owner-test')
    ap.add_argument("--source-root", default=repo_root(),
                    help="source worktree root (default: this script's repo)")
    ap.add_argument("--build-dir", default=None,
                    help="build output dir (default: <source-root>/build64/RelWithDebInfo, "
                    "or the preset's build dir with --target)")
    ap.add_argument("--exe-name", default=None,
                    help="exe filename (default mc2.exe, or the preset's exe "
                    "with --target)")
    ap.add_argument("--wait", type=float, default=None, metavar="SECONDS",
                    help="how long to wait for a game running from the target folder to exit before refusing (default %d, or MC2_DEPLOY_WAIT_S; "
                         "0 refuses at once)" % DEFAULT_DEPLOY_WAIT_S)
    ap.add_argument("--ninja", action="store_true",
                    help="use the Ninja single-config dev tree (build64-ninja/, "
                    "configured by scripts/configure_ninja.py) instead of the VS "
                    "tree. Equivalent to --build-dir <src>/build64-ninja for "
                    "--target game. An explicit --build-dir wins.")
    ap.add_argument("--shaders-only", action="store_true",
                    help="compile/deploy Vulkan shaders only; tag captures shader-dirty (no commit or exe build)")
    ap.add_argument("--build64-root", default=None,
                    help="explicit build64-family root that holds engine outputs "
                    "and mc2.dir (e.g. an absolute path to build64_island). Default: "
                    "walk up from --build-dir for an ancestor whose name starts with "
                    "'build64' (so build64_island/build64_editor are auto-found).")
    ap.add_argument("--allow-stale-pdb", action="store_true",
                    help="downgrade PDB staleness to a warning")
    ap.add_argument("--allow-stale-exe", action="store_true",
                    help="downgrade stale-exe (exe older than its .obj files) to a warning")
    ap.add_argument("--full-verify", action="store_true",
                    help="ignore the size+mtime_ns fast path: copy every payload "
                    "file and hash source and target as earlier versions did")
    ap.add_argument("--verify-only", action="store_true",
                    help="re-check existing target against its manifest; no copy")
    ap.add_argument("--write-manifest-only", action="store_true",
                    help="re-hash payload files ALREADY in the target and "
                    "rewrite .deployed_manifest.csv; no copy, no lock check. "
                    "Use as the final step of any OTHER deploy path "
                    "(deploy-editor.sh, manual recipe) so the CSV manifest "
                    "that --verify-only checks does not go stale.")
    ap.add_argument("--check-only", action="store_true",
                    help="only check the target has every runtime DLL the exe imports plus assets/ data/ shaders/; no copy; exit 7 if not")
    ap.add_argument("--strict", action="store_true",
                    help="with --verify-only: drift = nonzero exit")
    ap.add_argument("--require-clean", action="store_true",
                    help="refuse an executable whose embedded fingerprint says "
                         "dirty=1 (built from an uncommitted tree)")
    ap.add_argument("--no-example-mods", action="store_true",
                    help="omit bundled example mods from the payload")
    ap.add_argument("--skip-kit", action="store_true",
                    help="lane installs: leave the install's mods/ kit untouched (no pinned-kit payload, no kit reconcile)")
    ap.add_argument("--no-asset-sync", action="store_true",
                    help="lane installs: FAIL instead of syncing data/assets/*.fst files that differ from v0.4 (asset parity)")
    ap.add_argument("--allow-unverified-exe", action="store_true",
                    help="--target game: publish an exe whose sha256 is not the one in the newest PASS gate verdict for HEAD")
    ap.add_argument("--verdict", metavar="PATH",
                    help="--target game: gate-verdict.json from a stage/lane tree; accepted only if PASS, its head is this tree's HEAD "
                         "and its exe sha256 is the exe being deployed (default build64-ninja/gate-verdict.json)")
    ap.add_argument("--allow-stock-trees", action="store_true",
                    help="a failed deploy cook (concrete detail, cliff rock, detail normals) only warns and ships stock data (default: exit non-zero)")
    ap.add_argument("--allow-new-target", action="store_true",
                    help="DELIBERATELY deploy to a NON-canonical / new ~5GB install "
                         "(release cut, fresh install). Off by default -- deploys are "
                         "forced to the canonical DEPLOY_ALLOWLIST to stop per-lane bloat.")
    ap.add_argument("--bootstrap-from", metavar="INSTALL",
                    help="with --allow-new-target: when the target is missing or empty, first copy the base files (data, assets, "
                         "archives, DLLs) from this existing install once; the deploy then adds the exe, shaders and manifest.")
    ap.add_argument("--preserve", action="append", default=[],
                    metavar="RELPATH",
                    help="payload file (repeatable) whose EXISTING target copy "
                         "is kept instead of overwritten (e.g. a hand-edited "
                         "run-with-log.bat). Only skips if the target copy "
                         "exists; fresh targets still get the source file.")
    ap.add_argument("--from-payload", metavar="DIR", default=None,
                    help="publish a payload gated elsewhere (tools/laptop_gate.py pull-back: DIR/files + DIR/.deployed_manifest.csv) "
                         "instead of enumerating a local build; sha256-verified, no build needed. Pair with --payload-head.")
    ap.add_argument("--payload-head", metavar="SHA", default=None,
                    help="with --from-payload: the candidate head the exe must fingerprint as (default: --source-root HEAD)")
    ap.add_argument("--quiet", action="store_true",
                    help="print only verdicts, failures/warnings, artifact paths and the DEPLOY_FINGERPRINT lines; "
                         "the full output goes to a log (path on the last line)")
    ap.add_argument("--asset-store", default=os.environ.get("MC2_ASSET_STORE", ASSET_STORE_ROOT),
                    help="sha256-addressed intentional asset store (or MC2_ASSET_STORE)")
    asset_mode = ap.add_mutually_exclusive_group()
    asset_mode.add_argument("--from-install", metavar="INSTALL",
                           help="seed declared assets into the store from a read-only install and source; no deploy")
    asset_mode.add_argument("--dry-run", action="store_true",
                           help="audit intentional assets only; resolve missing/stale declarations without writes or build")
    args = ap.parse_args()
    if args.shaders_only and (args.target or args.from_payload or args.bootstrap_from or args.require_clean
                             or args.verify_only or args.write_manifest_only or args.check_only
                             or args.seed_owner_test or args.from_install or args.dry_run):
        ap.error("--shaders-only requires an existing lane install and cannot combine with other deploy modes")
    preset_target = TARGET_PRESETS.get(args.target, {}).get("target_dir")
    refuse_golden_target(args.target_dir or preset_target)
    import quiet_output
    if quiet_output.wanted(args):
        quiet_output.install_quiet(name="deploy_payload")
    global ALLOW_STOCK_TREES
    ALLOW_STOCK_TREES = bool(args.allow_stock_trees)

    if args.from_install:
        seed_asset_store(args.source_root, args.from_install, args.asset_store)
        return

    if args.seed_owner_test:
        preset = TARGET_PRESETS.get(args.target) if args.target else None
        destination = args.target_dir or (preset and preset['target_dir'])
        if not args.seed_source or not destination:
            ap.error('--seed-owner-test requires --seed-source INSTALL and target_dir (or --target)')
        source, target = args.seed_source, Path(destination)
        head = subprocess.check_output(['git', '-C', args.source_root, 'rev-parse', 'HEAD'],
                                       text=True, **console_child.child_kwargs(redirected=True)).strip()
        verify_published_identity(source, args.source_root, head)
        check_locks(str(target), 'mc2.exe', 0)
        held = take_folder_lease(str(target), 0)
        try:
            import kit_payload
            kit_payload.seed_install(source, target)
        finally:
            if held:
                release_folder_lease(str(target))
        return

    preset = TARGET_PRESETS.get(args.target) if args.target else None
    target_dir = args.target_dir or (preset and preset["target_dir"])
    if not target_dir:
        ap.error("a target dir is required: pass it positionally or via "
                 "--target game|editor")
    exe_name = args.exe_name or (preset["exe_name"] if preset else "mc2.exe")
    if preset:
        log(f"preset '{args.target}': target {preset['target_dir']}, "
            f"exe {preset['exe_name']}, build {preset['build_subdir']}")

    target = os.path.abspath(target_dir)
    if args.verify_only:
        sys.exit(verify_only(target, args.strict))

    src_root = os.path.abspath(args.source_root)
    if args.dry_run:
        intentional_payload(src_root, target, args.asset_store, dry_run=True)
        return
    refusal = implicit_target_refusal(src_root, args.target_dir)
    if refusal:
        fail(refusal)
    default_subdir = (preset["build_subdir"] if preset
                      else os.path.join("build64", "RelWithDebInfo"))
    if args.ninja:
        default_subdir = (preset["ninja_subdir"] if preset else "build64-ninja")
    build_dir = args.build_dir or os.path.join(src_root, default_subdir)
    pdb_name = os.path.splitext(exe_name)[0] + ".pdb"

    if args.check_only:
        gaps = check_install_complete(target, exe_name, build_dir, fix=False)
        if gaps:
            fail("INSTALL INCOMPLETE: " + ", ".join(gaps), 7)
        log("install complete")
        return

    if args.write_manifest_only:
        write_manifest_only(src_root, build_dir, exe_name, pdb_name, target,
                            build64_root=args.build64_root, no_example_mods=args.no_example_mods)
        return

    if os.path.isfile(os.path.join(build_dir, ".ninja_deps")):   # objects whose deps name another checkout ship stale code
        import deps_guard
        try:
            refusal = deps_guard.refusal_after(build_dir)
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            refusal = None
            log(f"[DEPS_GUARD] scan skipped: {exc}")
        if refusal:
            fail(refusal)

    # DEPLOY-TARGET-GUARD: force deploys to the canonical deploy folders; never create
    # or deploy into per-lane ~5GB installs that bloat A:/Games/mc2-opengl.
    if not args.allow_new_target:
        base = os.path.basename(os.path.normpath(target))
        if base not in DEPLOY_ALLOWLIST:
            fail(f"deploy target '{target}'\n"
                 f"  basename '{base}' is NOT a canonical deploy folder.\n"
                 f"  Deploy ONLY to: {sorted(DEPLOY_ALLOWLIST)}\n"
                 f"  (under A:/Games/mc2-opengl). Each full install is ~5GB -- do NOT spin "
                 f"up per-lane/per-experiment deploy folders; pick a FREE canonical folder "
                 f"and md5-verify after.\n"
                 f"  If you REALLY need a new install (release cut), pass --allow-new-target.")

    if args.bootstrap_from:
        if not args.allow_new_target:
            fail("--bootstrap-from creates a new install: pass --allow-new-target too")
        with deploy_phase("bootstrap seed", 600):
            bootstrap_install(args.bootstrap_from, target, exe_name, pdb_name)

    if not os.path.isdir(target):
        fail(f"target dir does not exist: {target} (refusing to create deploy "
             "targets -- wrong-target trap, Mistake C). Pass --allow-new-target only "
             "for a deliberate new install.")

    log(f"source root: {src_root}")
    log(f"build dir:   {build_dir}")
    log(f"target:      {target}")

    wait_s = args.wait if args.wait is not None else float(os.environ.get("MC2_DEPLOY_WAIT_S") or DEFAULT_DEPLOY_WAIT_S)
    with deploy_phase("locks and folder lease", max(120, wait_s + 30)):
        check_locks(target, exe_name, wait_s)
        lease_held = take_folder_lease(target, wait_s)
        _ASIDE_TARGET[0] = target
        sweep_asides(target)
    if getattr(args, "verdict", None) and (args.from_payload or not is_v04_game_publish(target, exe_name)):
        print("WARNING: --verdict is ignored (it applies only to a v0.4 game build deploy)", file=sys.stderr)
    try:
        if args.from_payload:
            head = args.payload_head or subprocess.check_output(['git', '-C', src_root, 'rev-parse', 'HEAD'],
                                                                text=True, **console_child.child_kwargs(redirected=True)).strip()
            with deploy_phase("published payload", 600):
                deploy_from_payload(args.from_payload, target, src_root, head)
        elif args.shaders_only:
            deploy_shaders_only(src_root, build_dir, target, exe_name)
        else:
            run_deploy(args, src_root, build_dir, target, exe_name, pdb_name)
        import seed_parity  # a v0.4 publish moves the parity reference: refresh the lane seed and its snapshot, or print the command
        seed_parity.after_v04_deploy(target)
    finally:
        if lease_held:
            sweep_asides(target)
            _ASIDE_TARGET[0] = None
            release_folder_lease(target)


def rebuild_command():
    """The exact command that rebuilds this worktree's dev exe (what a stale/dirty refusal tells the lane to run)."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
    tree = os.environ.get("MC2_NINJA_TREE")
    return f"cd {root} && " + (f"MC2_NINJA_TREE={tree} " if tree else "") + "py -3 scripts/ninja_build.py"


def require_head_exe(exe, head, root=None):
    """Reject stale/missing provenance from the executable bytes, never a header. With root, a clean
    ancestor build is accepted when no build input changed since (data/test-only commits)."""
    from exe_fingerprint import extract, sha_agrees, build_current
    fp = extract(exe)
    current = (sha_agrees(fp.get("sha"), head) is True
               or (root is not None and build_current(fp.get("sha"), head, root, bool(fp.get("dirty")))))
    if not fp.get("present") or not current:
        fail(f"REFUSING stale/unattributable executable: {exe}\n"
             f"  embedded build sha={fp.get('sha', 'absent')} != HEAD {head} "
             f"({fp.get('reason', 'SHA mismatch')}).\n"
             f"  Commit first, rebuild this HEAD with: {rebuild_command()}\n"
             "  then deploy --ninja (or name that same tree with --build-dir). "
             "--allow-stale-exe only relaxes object timestamps, never provenance.")
    return fp


def viewer_payload(build_dir, build64_root, head, root=None):
    """Ship the rebuilt viewer and its complete runtime closure, never an install's old exe."""
    b64 = find_build_root(build_dir, build64_root)
    cfg = "" if is_single_config(build_dir, b64) else os.path.basename(build_dir)
    directory = os.path.join(b64, "out", "tools", "asset_viewer", cfg)
    exe = os.path.join(directory, "mc2_asset_viewer.exe")
    if require_head_exe(exe, head, root)["dirty"]:
        fail(f"REFUSING dirty viewer build: {exe}; commit first and rebuild")
    # Include loader-library objects too: a partial build must not ship an old link.
    for root, _dirs, files in os.walk(os.path.join(b64, "out", "tools", "asset_viewer")):
        for name in files:
            obj = os.path.join(root, name)
            if name.endswith((".obj", ".o")) and os.path.getmtime(obj) > os.path.getmtime(exe) + 1:
                fail(f"STALE viewer: {exe} is older than {obj}; rebuild mc2_asset_viewer")
    items = [(exe, "mc2_asset_viewer.exe", "exe")]
    todo, seen = [exe], set()
    while todo:
        for name in _pe_imports(todo.pop()):
            if name in seen:
                continue
            seen.add(name)
            system = os.path.join(os.environ.get("SystemRoot", "C:/Windows"), "System32", name)
            crt = name.startswith(("msvcp140", "vcruntime140", "concrt140"))
            if name.startswith(("api-ms-", "ext-ms-")) or (os.path.isfile(system) and not crt):
                continue
            search = (directory, build_dir, os.path.dirname(system)) if crt else (directory, build_dir)
            src = next((os.path.join(d, name) for d in search
                        if os.path.isfile(os.path.join(d, name))), None)
            if not src:
                fail(f"viewer runtime DLL missing from build: {name}")
            items.append((src, name, "dll"))
            todo.append(src)
    for name in ("mesh_lit.vert.spv", "mesh_lit.frag.spv", "pbr_sphere.vert.spv", "pbr_sphere.frag.spv"):
        src = os.path.join(directory, "shaders_vk", name)
        if not os.path.isfile(src):
            fail(f"viewer shader missing from build: {src}")
        items.append((src, "shaders_vk/" + name, "shader"))
    return items


def dll_source_dirs(src_root):
    """Real homes of the runtime DLLs a fresh Ninja tree lacks: the repo's 3rdparty/lib/x64 (SDL2, zlib1), the ffmpeg
    bin dir, and the Visual Studio redist CRT (msvcp140*, vcruntime140*). No other worktree is consulted."""
    dirs = [os.path.join(src_root, "3rdparty", "lib", "x64"),
            os.path.join(src_root, "3rdparty", "ffmpeg-lgpl-win64", "bin"),
            os.path.join(src_root, "3rdparty", "fidelityfx", "bin")]
    pf = os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")
    for edition in ("BuildTools", "Community", "Professional", "Enterprise"):
        base = os.path.join(pf, "Microsoft Visual Studio", "2022", edition, "VC", "Redist", "MSVC")
        for ver in sorted(glob.glob(os.path.join(base, "*")), reverse=True):
            dirs.extend(glob.glob(os.path.join(ver, "x64", "Microsoft.VC*.CRT")))
    return [d for d in dirs if os.path.isdir(d)]


def seed_build_dlls(build_dir, src_root, names=None):
    """Copy any runtime DLL missing from build_dir out of dll_source_dirs (a fresh Ninja tree has none of them); returns the copied names."""
    if sys.platform != "win32":
        return []
    copied = []
    for name in (RUNTIME_DLLS if names is None else names):
        dst = os.path.join(build_dir, name)
        if os.path.isfile(dst):
            continue
        src = next((os.path.join(d, name) for d in dll_source_dirs(src_root) if os.path.isfile(os.path.join(d, name))), None)
        if src:
            shutil.copy2(src, dst)
            copied.append(name)
    if copied:
        log(f"seeded build dir {build_dir} with {', '.join(copied)} from 3rdparty/redist")
    return copied


def verdict_exe_refusal(src_root, exe_src, head, verdict_path=None):
    """Why the v0.4 publish must stop: the exe on disk is not the one the gate PASSed for HEAD (verdict artifacts.exe_sha256), else None.

    verdict_path (--verdict) names a gate-verdict.json from another tree; it must satisfy the same PASS/head/exe-sha checks.
    """
    path = verdict_path or os.path.join(src_root, "build64-ninja", "gate-verdict.json")
    try:
        with open(path, encoding="utf-8") as f:
            v = json.load(f)
        want = (v.get("artifacts") or {}).get("exe_sha256")
    except (OSError, ValueError, AttributeError):
        return f"no readable gate verdict at {path}"
    if v.get("verdict") != "PASS" or v.get("head") != head:
        return f"{'gate verdict ' + path if verdict_path else 'newest gate verdict'} is {v.get('verdict')} for {str(v.get('head'))[:9]}, not PASS for HEAD {head[:9]}"
    have = sha256_file(exe_src) if os.path.isfile(exe_src) else None
    if not want or have != want:
        return f"exe sha256 {str(have)[:12]} is not the gated {str(want)[:12]} (rebuilt after the gate?)"
    return None


def seed_receipt_rows():
    """The seed's own verification rows ({casefolded rel: {stamp, sha256}}); {} when the seed is absent."""
    seed = Path(os.environ.get('MC2_LEAN_SEED', LEAN_SEED_ROOT))
    rows = {}
    for name in ('.lane-seed-verification.json', '.lane-kit-verification.json'):
        try:
            rows.update(json.loads((seed / name).read_text(encoding='utf-8')))
        except (OSError, ValueError):
            pass
    return rows


def prepare_seed_delta(items, target, full_verify=False, allow_legacy=False):
    """Keep verified shared lane bytes; detach changed writes before the guard.

    Legacy lane installs have no receipt: detach their exact write set when
    explicitly allowed. Published v0.4 behavior is unchanged. Replacement never
    writes through a shared inode; the isolation guard still runs afterwards.
    """
    receipt = Path(target) / '.lane-seed.json'
    if not receipt.is_file() and not allow_legacy:
        return {}
    spec = importlib.util.spec_from_file_location('lane_seed', Path(__file__).with_name('make-isolated-deploy.py'))
    iso = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(iso)
    shared = json.loads(receipt.read_text(encoding='utf-8'))['shared'] if receipt.is_file() else {}
    verified = {}
    import kit_payload
    from concurrent.futures import ThreadPoolExecutor
    source_rx = kit_payload.HashReceipts(kit_payload.cook_cache_root() / 'source-receipts.json')
    counts = {'hashed': 0, 'privatized': 0, 'seed_confirmed': 0}
    seed_rows = seed_receipt_rows()
    restamped = {}

    def settle(item):
        src, rel, _kind = item
        dst = Path(target) / rel
        if not dst.is_file() or dst.stat().st_nlink <= 1:
            return None
        peer = seed_rows.get(rel.casefold())
        row = shared.get(rel) or peer  # a legacy install (no receipt) is proven by the seed's row when the stamp is identical
        # Full stamp incl. NTFS ChangeTime (pinned by the restored-mtime corruption test); the source side uses receipts.
        if not full_verify and src is not None and row and Path(src).is_file() and source_rx.digest(src) == row['sha256']:
            stamp = iso.content_stamp(dst)
            if stamp == row['stamp']:
                return rel, row['sha256']
            # Other installs linking/unlinking the shared inode bump its ChangeTime on every lane; the seed's own receipt is
            # re-stamped after each of those. The same (dev, ino, size, mtime, ChangeTime) as the seed's verified entry means
            # no write reached the inode since the seed bytes were hashed, so the cold re-read of the file is not needed.
            if peer and peer.get('sha256') == row['sha256'] and peer.get('stamp') == stamp:
                counts['seed_confirmed'] += 1
                restamped[rel] = stamp
                return rel, row['sha256']
            # Any link or unlink of the shared inode by another install bumps its ChangeTime without touching the bytes;
            # re-reading is far cheaper than copying every shared file. Equal bytes stay shared (deploy then writes nothing).
            counts['hashed'] += 1
            if kit_payload.sha(dst) == row['sha256']:
                restamped[rel] = stamp
                return rel, row['sha256']
        # The normal deploy still checks the source and copied hash. Privatize
        # first so the isolation guard sees no shared write destinations.
        counts['privatized'] += 1
        fd, tmp = tempfile.mkstemp(prefix=dst.name + '.private-', dir=dst.parent)
        os.close(fd)
        tmp = Path(tmp)
        backup = tmp.with_name(tmp.name + '.shared')
        try:
            shutil.copy2(dst, tmp)
            os.link(dst, backup)
            try:
                unlink_for_replace(dst)
                os.replace(tmp, dst)
            except OSError:
                if not dst.exists():
                    os.rename(backup, dst)
                raise
        finally:
            unlink_for_replace(tmp)
            unlink_for_replace(backup)
        return None

    # Per-file stat/hash/copy is IO latency bound; four workers (JOBS-sized) overlap it. Distinct files, distinct temps.
    with ThreadPoolExecutor(max_workers=4) as pool:
        for done in pool.map(settle, items):
            if done:
                verified[done[0]] = done[1]
    source_rx.save(merge=True)
    restamped = {r: st for r, st in restamped.items() if r in shared}
    if restamped and receipt.is_file():
        # Remember the proven stamps so this lane's next deploy takes the exact-stamp path (receipt is private: replace, never write through).
        for rel, stamp in restamped.items():
            shared[rel]['stamp'] = stamp
        tmp = receipt.with_name(receipt.name + f'.tmp-{os.getpid()}')
        try:
            tmp.write_text(json.dumps({'shared': shared}), encoding='utf-8')
            os.replace(tmp, receipt)
        except OSError as exc:
            log(f'[DEPLOY] WARN: could not refresh {receipt.name}: {exc}')
        finally:
            tmp.unlink(missing_ok=True)
    log(f'lane seed delta: {len(verified)} unchanged shared payload files retained '
        f'({counts["seed_confirmed"]} confirmed by the seed receipt, '
        f'{counts["hashed"]} re-hashed after a link-count change, {counts["privatized"]} detached)')
    return verified


PAK_CACHE_ROOT = Path(os.environ.get("MC2_PAK_CACHE", (INSTALL_ROOT + '/_pak-cache')))
PAK_REL = "paks/stock.mc2pak"


def _mc2pak_python():
    """The interpreter that can run tools/mc2pak.py (ZIP Zstandard needs Python 3.14), or None."""
    if sys.version_info >= (3, 14):
        return [sys.executable]
    try:
        r = subprocess.run(["py", "-3.14", "-c", "import sys"], capture_output=True, timeout=30,
                           **console_child.child_kwargs(redirected=True))
        return ["py", "-3.14"] if r.returncode == 0 else None
    except (OSError, subprocess.SubprocessError):
        return None


def registered_fsts(target):
    """[(name, path)] of the system.cfg [FastFiles] archives in registration order; None when there is no list or one is missing."""
    cfg = Path(target) / "system.cfg"
    if not cfg.is_file():
        return None
    section = re.search(r"(?ims)^\[FastFiles\](.*?)(?=^\[|\Z)", cfg.read_text(encoding="latin-1"))
    names = re.findall(r'(?im)^\s*st\s+File(\d+)\s*=\s*"([^"]+)"', section[1]) if section else []
    if not names:
        return None
    found = []
    for _, name in sorted(names, key=lambda item: int(item[0])):
        path = next((p for p in (Path(target) / name, Path(target) / "data/assets" / name) if p.is_file()), None)
        if path is None:
            return None
        found.append((name, path))
    return found


def _toc_hash(data):
    """tools/mc2pak.fast_hash (that module refuses to import below Python 3.14, so it is mirrored here)."""
    h, mask = 14695981039346656037, (1 << 64) - 1
    end = len(data) // 8 * 8
    for (w,) in struct.iter_unpack('<Q', data[:end]):
        h = ((h ^ w) * 0x9E3779B97F4A7C15) & mask
        h ^= h >> 29
    for b in data[end:]:
        h = ((h ^ b) * 1099511628211) & mask
    return f'{h:016x}'


def fst_snapshot(target):
    """{name: (size, mtime_ns)} of the registered FSTs, or None: taken before and after a pack to catch a source that changed."""
    fsts = registered_fsts(target)
    return None if fsts is None else {name: (st.st_size, st.st_mtime_ns) for name, path in fsts for st in [path.stat()]}


def pak_defect(pak, target):
    """Why `pak` cannot be the stock pak of `target`, or None. Cheap (central directory + 8 header bytes per FST, no decoding):
    the pak's own source list must equal the installed FSTs (name, size, entry count), every FST must be a whole archive with
    entries (a robocopy-preallocated textures.fst read as 0 entries and produced a 73 MB pak instead of 214 MB), and the pak
    must hold at least as many names as its largest source."""
    fsts = registered_fsts(target)
    if fsts is None:
        return "no registered FST set to compare against"
    try:
        with zipfile.ZipFile(pak) as z:
            items = z.infolist()
            if not items or items[0].filename != "mc2pak.json":
                return "pak has no leading mc2pak.json"
            sources = {s["name"]: s for s in json.loads(z.read(items[0]))["sources"]}
    except (OSError, zipfile.BadZipFile, KeyError, ValueError, TypeError) as exc:
        return f"pak unreadable ({type(exc).__name__}: {exc})"
    largest = 0
    for name, path in fsts:
        with open(path, "rb") as f:
            head = f.read(8)
            magic, count = struct.unpack("<II", head) if len(head) == 8 else (0, 0)
            whole = magic in (0xFADDECAF, 0xCADDECAF) and count > 0 and 8 + count * 266 <= path.stat().st_size
            toc = head + f.read(count * 266) if whole else b""
        if not whole:
            return f"{name} is not a whole FST (magic {magic:#x}, {count} entries, {path.stat().st_size} bytes): unfinished copy?"
        src = sources.get(Path(name.replace("\\", "/")).name.lower())  # mc2pak keys sources by basename
        if src is None or src.get("size") != path.stat().st_size or src.get("entries") != count:
            return f"pak source {name} ({src and src.get('entries')} entries, {src and src.get('size')} bytes) != installed FST ({count} entries, {path.stat().st_size} bytes)"
        if src.get("toc_hash") and src["toc_hash"] != _toc_hash(toc):  # same size and count but different content: packed from another FST
            return f"pak source {name} toc_hash {src['toc_hash']} != installed FST {_toc_hash(toc)}"
        largest = max(largest, count)
    if len(items) - 1 < largest:
        return f"pak holds {len(items) - 1} entries, fewer than its largest source ({largest})"
    return None


def stock_pak_key(src_root, target):
    """Cache key: every registered FST's name+size+sha256 plus the packer's own sha256; None without a system.cfg."""
    fsts = registered_fsts(target)
    if fsts is None:
        return None
    h = hashlib.sha256()
    for name, path in fsts:
        h.update(f"{name.lower()}\0{path.stat().st_size}\0{sha256_file(path)}\n".encode())
    for tool in ("tools/mc2pak.py", "tools/mc2x_import/fst.py"):
        h.update(f"{tool}\0{sha256_file(Path(src_root) / tool)}\n".encode())
    return h.hexdigest()[:32]


PAK_PACK_TIMEOUT_S = 1500
PAK_STATUS_NAME = ".pak_deploy_status"  # read by scripts/smoke_lib/pak_gate.py; not in the manifest, never retired


def ensure_stock_pak(src_root, target, hashes):
    """Default pak deploy: pack once per FST set into the shared cache, copy it (never hardlink: the engine maps it) to
    <target>/paks/stock.mc2pak (replace by rename) and record it in `hashes`. Any failure is a loud log line; the FSTs still ship and rc stays 0.
    The outcome (OK / FALLBACK / SKIP) is recorded in <target>/.pak_deploy_status so run_smoke can refuse a silent FST fallback."""
    status = _ensure_stock_pak(src_root, target, hashes)
    kept = Path(target) / PAK_REL
    if not status.startswith("OK") and kept.is_file():
        # retire_unowned never prunes the pak, so a failed or refused repack keeps the previous one (also when a code-only deploy
        # carried its row): judge it, then record it as it is on disk, unless it is itself defective (then the game must read the
        # FSTs, not a pak that disagrees with them). A good kept pak is OK; FALLBACK is only for a refused or missing pak.
        defect = quarantine_bad_pak(target) if status.startswith("FALLBACK") else None
        if defect:
            hashes.pop(PAK_REL, None)
            status += f"; kept pak refused and moved aside ({defect})"
        else:
            digest, size, stamp = hash_with_stamp(kept, sha256_file)
            hashes[PAK_REL] = (digest, size, None, stamp)
            log(f"WARN [PAK_DEPLOY] kept the previous {PAK_REL} ({size} bytes)")
            if status.startswith("FALLBACK") and registered_fsts(target) is not None:
                status = "OK kept previous pak (" + status[len("FALLBACK "):] + ")"
    write_pak_status(target, status)


def write_pak_status(target, status):
    path = Path(target) / PAK_STATUS_NAME
    try:
        path.write_text(status + "\n", encoding="utf-8")
    except OSError as exc:
        log(f"WARN [PAK_DEPLOY] could not write {PAK_STATUS_NAME}: {exc}")
        try:
            path.unlink(missing_ok=True)  # never leave an older OK behind: require_pak_ok then fails closed
        except OSError:
            pass


def quarantine_bad_pak(target):
    """pak_defect of the installed pak; when defective, move it to paks/stock.mc2pak.defect-<ts> and return the reason, else None.
    Not judged (None) without a registered FST set to compare against."""
    kept = Path(target) / PAK_REL
    if not kept.is_file() or registered_fsts(target) is None:
        return None
    defect = pak_defect(kept, target)
    if defect:
        aside = kept.with_name(f"{kept.name}.defect-{time.strftime('%Y%m%d-%H%M%S')}")
        replace_file(str(kept), str(aside))
        log(f"WARN [PAK_DEPLOY] installed {PAK_REL} is defective ({defect}); moved to {aside.name}, the game reads the FSTs")
        for old in sorted(kept.parent.glob(f"{kept.name}.defect-*"))[:-1]:  # keep only the newest
            old.unlink(missing_ok=True)
    return defect


def fail_status(reason):
    log(f"WARN [PAK_DEPLOY] {reason}; the pak was not replaced (the FSTs still ship)")
    return f"FALLBACK {reason}"


def _ensure_stock_pak(src_root, target, hashes):
    try:
        key = stock_pak_key(src_root, target)
        if key is None:
            log("WARN [PAK_DEPLOY] no system.cfg [FastFiles] / FST set to pack; FST only")
            return "SKIP no FST set"
        py = _mc2pak_python()
        if py is None:
            return fail_status("Python 3.14 is not available (tools/mc2pak.py needs ZIP Zstandard)")
        cache = PAK_CACHE_ROOT / key
        pak = cache / "stock.mc2pak"
        digest_file = cache / "stock.mc2pak.sha256"
        hit = pak.is_file() and digest_file.is_file()
        if hit and sha256_file(pak) != digest_file.read_text().strip():
            log(f"WARN [PAK_DEPLOY] cached pak digest mismatch key={key}; repacking")
            hit = False
        if not hit:
            cache.mkdir(parents=True, exist_ok=True)
            partial = cache / f"stock.{os.getpid()}.partial"
            started = time.perf_counter()
            fsts_before = fst_snapshot(target)
            log(f"[PAK_DEPLOY] cache miss key={key}: packing (about 9 minutes)")
            # Outside the bounded deploy clock: a timeout here must fall back to FST (rc 0, manifest written), never os._exit(8).
            try:
                with phase_paused():
                    r = subprocess.run([*py, str(Path(src_root) / "tools/mc2pak.py"), "pack", "--install", str(target),
                                        "--out", str(partial)], capture_output=True, text=True, timeout=PAK_PACK_TIMEOUT_S,
                                       **console_child.child_kwargs(redirected=True))
            except subprocess.TimeoutExpired:
                partial.unlink(missing_ok=True)
                return fail_status(f"mc2pak pack timed out after {PAK_PACK_TIMEOUT_S}s")
            if r.returncode != 0 or not partial.is_file():
                partial.unlink(missing_ok=True)
                return fail_status(f"mc2pak pack failed rc={r.returncode}: {(r.stderr or r.stdout).strip()[-300:]}")
            defect = ("a source FST changed while packing (size or mtime differs from before the pack)"
                      if fst_snapshot(target) != fsts_before else pak_defect(partial, target))
            if defect:
                partial.unlink(missing_ok=True)
                return fail_status(f"packed pak refused, not cached ({defect})")
            digest_file.write_text(sha256_file(partial))
            os.replace(partial, pak)
            log(f"[PAK_DEPLOY] packed {pak.stat().st_size} bytes in {time.perf_counter() - started:.1f}s")
        else:
            defect = pak_defect(pak, target)
            if defect:  # a cache entry packed from an incomplete FST set (older tool) must not be copied or reused
                pak.unlink(missing_ok=True)
                digest_file.unlink(missing_ok=True)
                return fail_status(f"cached pak refused and dropped ({defect})")
            log(f"[PAK_DEPLOY] cache hit key={key}")
        dst = Path(target) / PAK_REL
        dst.parent.mkdir(parents=True, exist_ok=True)
        want = digest_file.read_text().strip()
        row = pak_copy_current(dst, pak, want)  # (digest, size, stamp) when the installed pak is already the private copy: hashed once
        if row is None:
            # Always a private copy: the engine maps the pak, and a hardlinked mapped pak makes every install sharing it
            # undeletable (WinError 5) while any one game runs.
            tmp = dst.with_name(f".stock.mc2pak.new-{os.getpid()}")
            tmp.unlink(missing_ok=True)
            shutil.copy2(pak, tmp)
            if sha256_file(tmp) != want:  # a bad copy never replaces the pak that is there
                tmp.unlink(missing_ok=True)
                return fail_status("copy of the cached pak does not match its digest")
            replace_file(str(tmp), str(dst))
            row = hash_with_stamp(dst, sha256_file)
        # The row's hash is of the destination itself (the stamp is taken with it), never the cache's digest unverified.
        got, size, stamp = row
        if got != want:
            return fail_status(f"installed pak sha256 {got[:16]} != cached {want[:16]}")
        hashes[PAK_REL] = (got, size, None, stamp)
        log(f"[PAK_DEPLOY] OK {PAK_REL} key={key} sha256={hashes[PAK_REL][0][:16]}")
        return f"OK key={key}"
    except Exception as exc:  # noqa: BLE001 -- the FSTs still ship; a pak failure never fails the deploy
        return fail_status(f"pak step failed ({type(exc).__name__}: {exc})")


def pak_copy_current(dst, pak, digest):
    """(digest, size, dest stamp) when dst is already a private (st_nlink == 1) copy of the cached pak (same size, recorded
    digest; hashed once, with its stamp), else None."""
    try:
        st = dst.stat()
        if st.st_nlink != 1 or st.st_size != pak.stat().st_size:
            return None
        row = hash_with_stamp(dst, sha256_file)
        return row if row[0] == digest else None
    except OSError:
        return None


def run_deploy(args, src_root, build_dir, target, exe_name, pdb_name):
    with deploy_phase("deploy", 1800):
        return _run_deploy(args, src_root, build_dir, target, exe_name, pdb_name)


def _run_deploy(args, src_root, build_dir, target, exe_name, pdb_name):
    v04 = is_v04_game_publish(target, exe_name)
    unverified_marker = os.path.join(target, ".deployed_manifest.unverified.json")
    if v04 and getattr(args, "allow_unverified_exe", False):
        log("WARNING: --allow-unverified-exe on a v0.4 publish: the exe is not checked against the gate verdict (recorded in .deployed_manifest.unverified.json)")
        try:
            with open(unverified_marker, "w", encoding="utf-8") as f:
                json.dump({"allow_unverified_exe": True, "head": git_head(src_root),
                           "time": datetime.datetime.now().isoformat(timespec="seconds")}, f)
        except OSError as e:
            log(f"WARNING: could not record the unverified publish: {e}")
    elif v04:
        why = verdict_exe_refusal(src_root, os.path.join(build_dir, exe_name), git_head(src_root),
                                  getattr(args, "verdict", None))
        if why:
            fail(f"v0.4 publish refused: {why}. Deploy the exe the gate tested: the gated tree's build dir, or "
                 f"--verdict <gated tree or proof dir>/gate-verdict.json. Rebuilt after the gate: re-run the gate.")
        try:
            os.unlink(unverified_marker)
        except OSError:
            pass
    if v04:
        args.require_clean = True
    seed_build_dlls(build_dir, src_root)
    check_exe_fresh(os.path.join(build_dir, exe_name), build_dir, args.allow_stale_exe,
                    build64_root=args.build64_root)

    # DIRTY-BUILD GUARD: record what the exe was actually BUILT from, not just
    # git HEAD at deploy time. A dirty-tree build is unattributable to any
    # exact source state (2026-07-12 grey-regression P0 mis-diagnosis class).
    source_head = git_head(src_root)
    src_commit = source_head
    fp = require_head_exe(os.path.join(build_dir, exe_name), src_commit, src_root)
    if fp["dirty"]:
        msg = (f"embedded fingerprint says dirty build at {fp['sha']}. "
               f"COMMIT FIRST, rebuild ({rebuild_command()}), then deploy.")
        if args.require_clean:
            fail("--require-clean refused deploy: " + msg +
                 " lane_merge verification must build the committed HEAD before the gate deploy stage.")
        log(f"WARNING (dirty build): {msg}")
        src_commit += "-dirty"

    with deploy_phase("input comparison", 120):
        inputs = None
        reuse, reason = False, "explicit full verification or asset override"
        if not args.classic_assets and not getattr(args, "skip_kit", False):
            inputs = deploy_inputs(src_root)
            if getattr(args, "no_example_mods", False):
                inputs["no_example_mods"] = True
            reuse, reason = reusable_payload(src_root, target, inputs)
            if getattr(args, "full_verify", False):
                reuse, reason = False, "explicit full verification"
        log(f"kit {'skipped automatically' if reuse else 'full deploy'}: {reason}")
        previous = read_manifest(target)
        old_reuse = json.loads((Path(target) / REUSE_NAME).read_text()) if reuse else None
        # A failed copy/cook must not leave valid-looking evidence for a retry.
        (Path(target) / REUSE_NAME).unlink(missing_ok=True)
    with deploy_phase("payload enumeration", 300):
        items = enumerate_payload(src_root, build_dir, exe_name, pdb_name,
                                  build64_root=args.build64_root, code_only=reuse,
                                  no_example_mods=getattr(args, "no_example_mods", False))
        items.extend(magic_brain_payload(src_root, target))
    if exe_name != "mc2":
        with deploy_phase("viewer payload", 120):
            items.extend(viewer_payload(build_dir, args.build64_root, source_head, src_root))
    import kit_payload
    from smoke_lib import render_options_pin
    kit_manifest = None
    vehicle_manifest = None
    kit_tool = None
    remastered = render_options_pin.record(Path(target), os.environ)["values"]["RemasteredBuildings"]
    kit_prepare_started = time.perf_counter()
    if not args.classic_assets and not getattr(args, "skip_kit", False) and not reuse and remastered:
        try:
            with deploy_phase("kit preparation and importer", 600):
                # The kit stores are read-only reference trees: their stamped hashes are shared by every install.
                kit_receipts = kit_payload.HashReceipts(
                    Path(target) / '.deploy-receipts.json',
                    shared=kit_payload.HashReceipts(kit_payload.cook_cache_root() / 'store-receipts.json'))
                kit_manifest, kit_items = kit_payload.payload(src_root, receipts=kit_receipts)
                items.extend(kit_items)
                vehicle_manifest, vehicle_items = kit_payload.payload(src_root,
                    Path("data/kit/vehicle-kit.json"), Path("mods/mc2-vehicle-kit"), receipts=kit_receipts)
                items.extend(vehicle_items)
                kit_receipts.save()
                kit_tool = kit_payload.build_importer(src_root)
        except (OSError, ValueError, KeyError) as exc:
            fail(f"RemasteredBuildings requires the pinned kit: {exc}; use --classic-assets for Classic")
    declared_items = intentional_payload(src_root, target, getattr(args, "asset_store", None)
                                         or os.environ.get("MC2_ASSET_STORE", ASSET_STORE_ROOT))
    declared_paths = {rel for _, rel, _ in declared_items}
    items = [item for item in items if item[1] not in declared_paths] + declared_items
    kit_prepare_seconds = time.perf_counter() - kit_prepare_started
    log(f"payload: {len(items)} files "
        f"({sum(1 for *_x, k in items if k == 'shader')} shaders, "
        f"{sum(1 for *_x, k in items if k == 'defs')} defs)")

    # DISK-GUARD-1 (2026-07-29 ENOSPC incident) -- checked HERE, immediately
    # after enumeration, because this is the first moment the required byte
    # count is actually KNOWN. Every byte below is measured from the real
    # payload; nothing is guessed. Still before deploy() copies anything.
    with deploy_phase("disk space guard", 120):
        check_free_space_for_payload(items, target)

    # LINK-GUARD-1 (2026-07-30 shared-install incident) -- checked HERE for the
    # same reason as the disk guard: `items` is the first moment the exact set
    # of relpaths about to be overwritten is known, and it is still before
    # deploy() writes a single byte. Order relative to the disk guard is not
    # load-bearing (neither writes anything); both must pass.
    # write_manifest() below writes .deployed_manifest.csv, which
    # enumerate_payload() does not list (it is an output, not payload) -- and
    # that file was itself hardlinked four ways in the incident. Append it so
    # the guard covers every file this run will write, not just the copied
    # ones.
    write_set = items + [(None, MANIFEST_NAME, "manifest")]
    legacy_lane = (getattr(args, "allow_new_target", False) and
                   Path(target).resolve().is_relative_to(Path(INSTALL_ROOT).resolve()))
    with deploy_phase("hardlink isolation", 300):
        verified = prepare_seed_delta(write_set, target, args.full_verify, allow_legacy=legacy_lane)
        check_hardlink_isolation([item for item in items if item[1] not in verified] + [(None, MANIFEST_NAME, "manifest")],
                                 target)

    cache = {} if args.full_verify else read_manifest(target)
    preserve = list(args.preserve)
    if getattr(args, "skip_kit", False):
        preserve.extend(rel for rel in previous if rel.startswith("mods/"))
    with deploy_phase("payload hash copy and verification", 600):
        hashes = deploy(items, target, args.allow_stale_pdb, preserve=preserve,
                        cache=cache, full_verify=args.full_verify, code_only=reuse, verified=verified,
                        deploying_target="editor" if exe_name.casefold() == "mission editor.exe" else "game")
    if getattr(args, "skip_kit", False):
        hashes = carry_rows({rel: row for rel, row in previous.items() if rel.startswith("mods/")}, target) | hashes
    if kit_manifest:
        try:
            phase_started = time.perf_counter()
            with deploy_phase("stock texture receipts", 300):
                kit_payload.reconcile_stock_textures(target,
                    jobs=kit_payload.jobs_limit())
            log(f"phase stock receipts seconds={time.perf_counter() - phase_started:.3f}")
            phase_started = time.perf_counter()
            with deploy_phase("kit cook", 900):
                importer_key = kit_payload.importer_source_key(src_root, Path(kit_tool).parent)
                kit_payload.finish(target, kit_manifest, kit_tool, importer_key=importer_key)
                if vehicle_manifest:
                    kit_payload.finish(target, vehicle_manifest, kit_tool, kit_rel=Path("mods/mc2-vehicle-kit"),
                                       importer_key=importer_key)
            log(f"phase kit cook seconds={kit_prepare_seconds + time.perf_counter() - phase_started:.3f}")
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
            fail(f"pinned industrial kit deploy failed: {exc}")
    else:
        log("phase stock receipts seconds=0.000 (skipped)")
        log("phase kit cook seconds=0.000 (skipped)")
    # ASSET-PARITY-1 (owner 2026-11-11): a lane install must carry v0.4's data/, assets/ and *.fst. Sync what
    # differs from v0.4 (read only) or, with --no-asset-sync, fail with the counts; the verdict line is
    # [ASSET_PARITY], read by run_smoke and refused by the capture tools.
    import asset_parity
    # The manifest written below and captures use the same exact, hash-verified payload inventory.
    # Code-only deploy() retains the prior manifest's data rows in hashes.
    drift = asset_parity.enforce(target, sync_missing=not args.no_asset_sync, jobs=asset_parity.jobs(),
                                 payload={rel: row[0] for rel, row in hashes.items()},
                                 declared=asset_parity.intentional_changes(source_head, Path(src_root)))
    if drift is None:
        log(f"{asset_parity.PREFIX} SKIPPED ref={asset_parity.default_ref().as_posix()} (exempt target or no reference on this host)")
    elif drift.ok:
        log(asset_parity.verdict_line(drift, asset_parity.default_ref()))
    else:
        for _ln in asset_parity.detail_lines(drift, target, asset_parity.default_ref()):
            log(_ln)
        fail(f"asset parity: {len(drift.missing)} missing, {len(drift.size)} size-different, "
             f"{len(drift.content)} content-different, {len(drift.extra)} extra file(s) vs {asset_parity.default_ref()} "
             f"(missing e.g. {', '.join(drift.missing[:5])}); rerun without --no-asset-sync to copy them from v0.4", 8)
    with deploy_phase("mod visual height cook", 900):
        cook_mod_visual_heights(src_root, target)
    if not reuse and (Path(target) / "data/model_overrides/models.json").is_file():
        try:
            with deploy_phase("tree cook", 900):
                kit_tool = kit_tool or kit_payload.build_importer(src_root)
                kit_payload.finish_trees(target, kit_tool,
                    importer_key=kit_payload.importer_source_key(src_root, Path(kit_tool).parent))
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
            if args.target == "game":  # v0.4 must ship precooked trees; lane installs keep the soft warning
                fail(f"tree model precook failed: {exc}")
            log(f"WARN: tree model precook failed: {exc}; runtime will cook missing models")
    installed = os.path.join(target, exe_name)
    require_head_exe(installed, source_head, src_root)
    if sha256_file(installed) != sha256_file(os.path.join(build_dir, exe_name)):
        fail(f"REFUSING installed executable differs from validated source: {installed}")
    # Completeness BEFORE the manifest: a failure must not leave a manifest that says the deploy is complete.
    gaps = check_install_complete(target, exe_name, build_dir)
    if gaps:
        fail("INSTALL INCOMPLETE (game would die with 0xC0000135 / missing assets): " + ", ".join(gaps) +
             f"; copy them from a full install (e.g. {GOLDEN_MATERIAL_ROOT})", 7)
    if os.path.isdir(os.path.join(build_dir, "shaders", "vulkan")) and os.path.isdir(os.path.join(target, "shaders", "vulkan")):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
        import check_deployed_spv
        spv_fails, spv_warns, spv_n = check_deployed_spv.audit(
            target, build_dir, baked={p.name for p in baked_spv_files(build_dir)})
        for m in spv_warns:
            log(f"WARNING deployed SPIR-V: {m}")
        if spv_fails:
            fail("deployed SPIR-V is not the build's SPIR-V:\n  " + "\n  ".join(spv_fails), 4)
        log(f"deployed SPIR-V audit OK: {spv_n} .spv identical to the build, descriptor/push ABI matches")
    hashes = {rel: row for rel, row in hashes.items() if not asset_parity.is_deploy_artifact(rel)}
    restamp_rewritten(target, hashes)  # kit cook, asset sync, tree cook and DLL fixes ran after the hashes were taken
    ensure_stock_pak(src_root, target, hashes)  # editor deploys too: retire_unowned would prune the pak otherwise
    write_manifest(target, hashes, src_commit)
    if reuse:
        # Preserve the verified external source inventory across code-only deploys.
        record_deploy_inputs(target, inputs, hashes, [])
        value = json.loads((Path(target) / REUSE_NAME).read_text())
        value["sources"] = old_reuse["sources"]
        (Path(target) / REUSE_NAME).write_text(json.dumps(value, sort_keys=True), encoding="utf-8")
    elif kit_manifest and inputs is not None:
        record_deploy_inputs(target, inputs, hashes, items)
    log("deploy COMPLETE â€” payload verified, manifest written")

    # Post-deploy hygiene scan (Truth-First arc P1 #5/#7): the payload manifest
    # is a COMPLETENESS gate and never walks data/missions/*.beauty/, so a
    # generated-diagnostic sidecar that leaked into the target by MANUAL copy is
    # invisible to it. Run the standalone hygiene gate as a non-fatal warning so
    # every deploy surfaces leaked dev artifacts. Fully skippable / advisory.
    try:
        hygiene = Path(__file__).resolve().parent / "check-deploy-hygiene.py"
        if os.path.isfile(hygiene):
            r = subprocess.run([sys.executable, hygiene, target],
                               capture_output=True, text=True, timeout=120, **console_child.child_kwargs(redirected=True))
            out = (r.stdout or "").strip()
            if out:
                for line in out.splitlines():
                    log(f"hygiene: {line}")
    except Exception as e:  # noqa: BLE001 -- advisory only, never block a deploy
        log(f"hygiene scan skipped ({e})")


if __name__ == "__main__":
    main()
