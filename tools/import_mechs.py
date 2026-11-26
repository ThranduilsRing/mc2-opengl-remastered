#!/usr/bin/env python3
"""Build personal-use mech mods from installed games; never distribute the output.

Python dependencies: numpy scipy Pillow pygltflib UnityPy (BT only).
Native tools are discovered on PATH, in their environment variables, or common
tool directories. Extracted game bytes and logs stay in the system temp folder.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))
import console_child

LAYOUTS = {"mw5": "MW5Mercs/Content/Paks", "clans": "MW5Clans/Content/Paks",
           "bt": "BattleTech_Data/StreamingAssets/data/assetbundles"}


def allowed(path):
    return path.drive.upper() not in {"I:", "J:", "K:"}


def detect(steam_roots, epic_dir, common_roots):
    """Inspect launcher metadata and one level of common game directories only."""
    candidates = []
    for steam in steam_roots:
        if not allowed(steam):
            continue
        libraries = [steam]
        vdf = steam / "steamapps/libraryfolders.vdf"
        if vdf.is_file():
            libraries += [Path(s.replace("\\\\", "\\")) for s in
                          re.findall(r'"path"\s*"([^"\r\n]+)"', vdf.read_text(errors="replace"))]
        for library in libraries:
            if not allowed(library):
                continue
            for manifest in (library / "steamapps").glob("appmanifest_*.acf"):
                match = re.search(r'"installdir"\s*"([^"\r\n]+)"', manifest.read_text(errors="replace"))
                if match:
                    candidates.append(library / "steamapps/common" / match[1])
    if allowed(epic_dir):
        for manifest in epic_dir.glob("*.item"):
            try:
                candidates.append(Path(json.loads(manifest.read_text(encoding="utf-8-sig"))["InstallLocation"]))
            except (ValueError, KeyError):
                continue
    for root in common_roots:
        if allowed(root) and root.is_dir():
            candidates.extend(p for p in root.iterdir() if p.is_dir())
    found = {}
    for candidate in candidates:
        if allowed(candidate):
            for kind, layout in LAYOUTS.items():
                if kind not in found and (candidate / layout).is_dir():
                    found[kind] = candidate.resolve()
    return found


def tool(name, env, candidates):
    for value in [os.environ.get(env, ""), shutil.which(name) or "", *map(str, candidates)]:
        if value and allowed(Path(value)) and Path(value).is_file():
            return str(Path(value).resolve())
    return None


def run(cmd, log, env=None, ok=(0,)):
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run(list(map(str, cmd)), stdout=stream, stderr=subprocess.STDOUT,
                                env=env, **console_child.child_kwargs(redirected=True))
    if result.returncode not in ok:
        raise RuntimeError(f"exit {result.returncode}; see {log}")


def preflight(games, work):
    """Report actionable prerequisites by game so one failure cannot mask another."""
    errors = {kind: [] for kind in games}
    env = os.environ.copy()
    env.update(TEMP=str(work), TMP=str(work), DOTNET_CLI_HOME=str(work / "dotnet-home"),
               NUGET_PACKAGES=str(work / "nuget"), PYTHONDONTWRITEBYTECODE="1")
    ktx = tool("ktx", "KTX", [Path("A:/Games/mc2-tools/ktx/ktx.exe")])  # drive-path: ok optional workstation fallback; PATH and KTX take precedence
    umodel = tool("umodel_64", "UMODEL", [Path.home() / "Downloads/umodel_win32/umodel_64.exe"])
    for module, pip_name in [("numpy", "numpy"), ("scipy", "scipy"), ("PIL", "Pillow"), ("pygltflib", "pygltflib")]:
        if importlib.util.find_spec(module) is None:
            for issues in errors.values():
                issues.append(f"Install Python dependency: {sys.executable} -m pip install {pip_name}")
    if ktx:
        try:
            run([ktx, "--version"], work / "ktx.log", env)
            env["KTX"] = ktx
        except (OSError, RuntimeError) as exc:
            ktx = None
            print(f"ktx: {exc}")
    if not ktx:
        for issues in errors.values():
            issues.append("Install KTX-Software from https://github.com/KhronosGroup/KTX-Software/releases; put ktx on PATH or set KTX to ktx.exe")
    if "mw5" in games:
        if umodel:
            env["UMODEL"] = umodel
        else:
            errors["mw5"].append("Attachments still need UE Viewer (MIT): https://www.gildor.org/en/projects/umodel ; set UMODEL to umodel_64.exe (not bundled)")
    if any(kind in games for kind in ("mw5", "clans")):
        if not shutil.which("dotnet"):
            for kind in ("mw5", "clans"):
                if kind in errors:
                    errors[kind].append("Install .NET 8 SDK: https://dotnet.microsoft.com/download/dotnet/8.0 ; restart the terminal")
        else:
            try:
                run(["dotnet", "build", ROOT / "tools/mw5_kit/extract/Mw5Extract.csproj", "-c", "Release",
                     "--artifacts-path", work / "dotnet"], work / "dotnet.log", env)
                env["MW5_EXTRACT_DLL"] = str(work / "dotnet/bin/Mw5Extract/release/Mw5Extract.dll")
            except (OSError, RuntimeError) as exc:
                for kind in ("mw5", "clans"):
                    if kind in errors:
                        errors[kind].append(f"Mw5Extract build failed ({exc}); install .NET 8 SDK and allow NuGet restore, then rerun")
    if "bt" in games and importlib.util.find_spec("UnityPy") is None:
        errors["bt"].append(f"Install bundle reader: {sys.executable} -m pip install -r tools/bt2018_import/requirements.txt")
    return env, errors


def extract(env, game, kind, request, out, work):
    req = work / "request.json"
    req.write_text(json.dumps(request), encoding="utf-8")
    cmd = ["dotnet", env["MW5_EXTRACT_DLL"], "--install", game, "--paks", game / LAYOUTS[kind],
           "--ue", "5.5" if kind == "clans" else "4.26", "--request", req, "--out", out]
    if kind == "clans" and env.get("MW5_USMAP"):
        cmd += ["--usmap", env["MW5_USMAP"]]
    if env.get("MW5_AES"):
        cmd += ["--aes", env["MW5_AES"]]
    run(cmd, work / "extract.log", env, ok=(0, 2))


def material_defaults(env, game, work):
    if env.get("MW5_MATERIAL_DEFAULTS"):
        # Accept an existing player-local extraction, never a developer install.
        params = json.loads(Path(env["MW5_MATERIAL_DEFAULTS"]).read_text())
        check_material_defaults(params)
        return
    out = work / "defaults"
    extract(env, game, "mw5", {"mesh": "", "packages": [
        "/Game/Objects/_common/Materials/PaintedMetals/PaintedMetal_MTL"]}, out, work)
    params = json.loads((out / "PaintedMetal_MTL.exports.json").read_text())[0]["Properties"]["CachedExpressionData"]["Parameters"]
    scalars = dict(zip((p["Name"] for p in params["RuntimeEntries"]["ParameterInfos"]), params["ScalarValues"]))
    vectors = dict(zip((p["Name"] for p in params["RuntimeEntries[1]"]["ParameterInfos"]),
                       ([v[c] for c in "RGBA"] for v in params["VectorValues"])))
    defaults = {"S": scalars, "V": vectors}
    check_material_defaults(defaults)
    path = work / "material-defaults.json"
    path.write_text(json.dumps(defaults))
    env["MW5_MATERIAL_DEFAULTS"] = str(path)


def check_material_defaults(defaults):
    metals = ("Base Metal", "Gun Metal", "Black Metal", "Steel", "Aluminum", "Matte", "Anodized", "Sensor")
    missing = [m + " " + field for m in metals for field in ("Roughness", "Metallic")
               if m + " " + field not in defaults["S"]]
    if missing:
        raise RuntimeError("Installed PaintedMetal_MTL is incompatible with the existing eight-metal bake; missing "
                           + ", ".join(missing) + ". The cooker needs support for this game's material, or set "
                           "MW5_MATERIAL_DEFAULTS to matching player-extracted S/V defaults. Values will not be guessed")


def base_install(install, stage):
    """Builders read base data here and write only mods; hardlinks avoid copying bytes."""
    shutil.copytree(install / "data", stage / "data", copy_function=os.link)
    for path in install.glob("*.fst"):
        os.link(path, stage / path.name)


def validate(pack, dump=None):
    from tools.bt2018_mechs.glb_guard import validate_pack_glbs
    broken = validate_pack_glbs(pack)
    if not list(pack.rglob("*.glb")) or not (pack / "mod.json").is_file():
        raise RuntimeError("builder produced no complete mod")
    if broken:
        raise RuntimeError(f"pack GLB check: {broken}")
    run([sys.executable, ROOT / "tools/bt2018_import/check_mech_packages.py", "--deploy", pack / "data/tgl",
         "--dump", dump or pack], pack.parent / "validate.log")


def mercs(game, install, work, env, recipes, report):
    env = dict(env, MW5_INSTALL=str(game))
    material_defaults(env, game, work)
    stage = work / "base"
    base_install(install, stage)
    for recipe_path, recipe in recipes:
        name = recipe["family"]
        dest = work / name
        dest.mkdir()
        pack = stage / "mods/mw5_mechs"
        try:
            run([sys.executable, ROOT / "tools/mw5_mechs/make_mech.py", "--recipe", recipe_path,
                 "--work", dest, "--install", stage], dest / "build.log", env)
            validate(pack)
            shutil.copytree(pack, install / "mods/mw5_mechs", dirs_exist_ok=True)
            report.append(("mw5", name, "PASS", str(dest / "build.log")))
        except (OSError, ValueError, RuntimeError) as exc:
            report.append(("mw5", name, "FAIL", str(exc)))
        finally:
            if pack.exists() and pack.resolve().is_relative_to(stage.resolve()):
                shutil.rmtree(pack)


def clans(game, work, env, recipes, report):
    # Clans paths need verification against the player's build; never turn an
    # unverified bare model into a playable unit by guessing a recipe.
    name, recipe = next(((r["family"], r) for _, r in recipes if r["family"] == "timberwolf"),
                        (recipes[0][1]["family"], recipes[0][1]))
    try:
        extract(env, game, "clans", {"mesh": recipe["extraction"]["mesh"]}, work / "native", work)
        bridge = json.loads((work / "native/bridge.json").read_text())
        if bridge or not (work / "native/mesh.json").is_file():
            raise RuntimeError(f"Clans mesh not decoded: {bridge}; see {work / 'extract.log'}")
        raise RuntimeError("Clans mesh extracted; a verified Clans material/loadout/attachment recipe is still required before pack building")
    except (OSError, ValueError, RuntimeError) as exc:
        report.append(("clans", name, "FAIL", f"{exc}; UE5.5 requires matching MW5_USMAP; encrypted containers may also need MW5_AES (not derived or bundled)"))


def battletech(game, install, work, env, names, report):
    # Existing conversion starts at AssetStudio's FBX export, not Unity bundles.
    # Keep that prerequisite explicit rather than falling back to a dev install.
    dump = Path(env.get("BT2018_DUMP", str(work / "bt-dump")))
    fbx = tool("FBX2glTF", "FBX2GLTF", [Path.home() / "Downloads/FBX2glTF-windows-x64.exe"])
    if not fbx or not list(dump.glob("Animator/chrPrfMech_*Base-001/*.fbx")):
        raise RuntimeError(f"BT model exporter missing: export YOUR {game / LAYOUTS['bt']} with AssetStudio (https://github.com/Perfare/AssetStudio) as FBX + Texture2D PNG to {dump}; set BT2018_DUMP to that folder and FBX2GLTF to https://github.com/facebookincubator/FBX2glTF/releases executable. Bundle-to-skinned-GLB extraction is not implemented by the existing tools")
    conv, tex = work / "conversions", work / "textures"
    tex.mkdir()
    # The Atlas reference is generated into scratch, never into the source dump.
    atlas = next(dump.glob("Animator/chrPrfMech_atlasBase-001/*.fbx"))
    ref = work / "atlas"
    run([fbx, "-i", atlas, "-o", ref, "-b"], work / "atlas.log", env)
    from tools.bt2018_import.convert_all_bt2018 import convert_one, btname_of
    from types import SimpleNamespace
    args = SimpleNamespace(out=str(conv), deploy=str(tex), fbx2gltf=fbx, force=False, atlas_ref=str(ref.with_suffix(".glb")))
    conv.mkdir()
    rows = []
    for source in sorted(dump.glob("Animator/chrPrfMech_*Base-001/*.fbx")):
        name = btname_of(str(source)).lower()
        if name not in names:
            continue
        try:
            row = convert_one(str(source), name, args)
            if not row:
                raise RuntimeError("FBX conversion failed")
            rows.append(row)
        except Exception as exc:
            report.append(("bt", name, "FAIL", str(exc)))
    if not rows:
        raise RuntimeError("No BT models converted")
    # build_pack retargets from the Atlas reference; include it even for --only.
    shutil.copyfile(ref.with_suffix(".glb"), conv / "atlas.glb")
    manifest = conv / "conversions_manifest.json"
    manifest.write_text(json.dumps(rows))
    common = ["--manifest", manifest, "--conversions", conv, "--deploy", tex]
    for script, flags in [("gen_mech_package.py", ["--dump", dump]),
                          ("cook_ktx2_textures.py", ["--ktx", env["KTX"], "--keep-tga"])]:
        run([sys.executable, ROOT / "tools/bt2018_import" / script, *common, *flags], work / (script + ".log"), env)
    run([sys.executable, ROOT / "tools/bt2018_import/cook_ao_from_package.py", "--deploy", tex,
         "--dump", dump, "--ktx", env["KTX"]], work / "maps.log", env)
    pack = work / "bt2018_mechs"
    run([sys.executable, ROOT / "tools/bt2018_mechs/build_pack.py", "--install", install,
         "--conversions", conv, "--texture-source", tex, "--bundles", game / LAYOUTS["bt"],
         "--out", pack], work / "build.log", env)
    validate(pack, dump)
    shutil.copytree(pack, install / "mods/bt2018_mechs", dirs_exist_ok=True)
    built = {row["btname"] for row in rows}
    for name in names:
        if not any(r[:2] == ("bt", name) for r in report):
            report.append(("bt", name, "PASS" if name in built else "FAIL", str(work / "build.log") if name in built else "No exported FBX for authored roster entry"))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", required=True, type=Path)
    for kind in LAYOUTS:
        parser.add_argument("--" + kind, type=Path)
    parser.add_argument("--only", default="", help="comma-separated authored recipe names")
    args = parser.parse_args(argv)
    install = args.install.resolve()
    if not (install / "data").is_dir():
        parser.error("--install must be an existing MC2 install with data/")
    games = detect([Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Steam"],
                   Path(os.environ.get("ProgramData", "C:/ProgramData")) / "Epic/EpicGamesLauncher/Data/Manifests",
                   [Path(d + suffix) for d in ("A:", "D:", "E:", "G:") for suffix in ("/Games", "/Epic Games")])
    for kind, layout in LAYOUTS.items():
        explicit = getattr(args, kind)
        if explicit:
            if not allowed(explicit) or not (explicit / layout).is_dir():
                parser.error(f"--{kind} must contain {layout}")
            games[kind] = explicit.resolve()
    if not games:
        parser.error("No games detected; pass --mw5, --clans or --bt with your game root")
    work = Path(tempfile.mkdtemp(prefix="mc2-import-mechs-", dir=os.environ.get("TEMP")))
    for source in [ROOT, *games.values()]:
        if work.is_relative_to(source) or install.is_relative_to(source) or source.is_relative_to(install):
            parser.error("Output and temp folders must be separate from game installs and repository")
    print(f"Work/logs: {work}", flush=True)
    print("Detected: " + "; ".join(f"{k}={v}" for k, v in games.items()), flush=True)
    recipes = [(p, json.loads(p.read_text())) for p in sorted((ROOT / "tools/mw5_mechs/recipes").glob("*.json"))]
    recipes = [(p, r) for p, r in recipes if r.get("schema") == "mw5-mech-recipe/1"]
    bt_names = sorted(n for n in json.loads((ROOT / "tools/bt2018_mechs/data/roster.json").read_text()) if not n.startswith("_"))
    only = {s.strip().lower() for s in args.only.split(",") if s.strip()}
    unknown = only - {r["family"] for _, r in recipes} - set(bt_names)
    if unknown:
        parser.error(f"Unknown recipe names: {sorted(unknown)}")
    if only:
        recipes = [(p, r) for p, r in recipes if r["family"] in only]
        bt_names = [n for n in bt_names if n in only]
    env, errors = preflight(games, work)
    report = []
    for kind, game in games.items():
        names = bt_names if kind == "bt" else [r["family"] for _, r in recipes]
        if not names:
            continue
        dest = work / kind
        dest.mkdir()
        try:
            if errors[kind]:
                raise RuntimeError("; ".join(errors[kind]))
            if kind == "mw5":
                mercs(game, install, dest, env, recipes, report)
            elif kind == "clans":
                clans(game, dest, env, recipes, report)
            else:
                battletech(game, install, dest, env, names, report)
        except Exception as exc:
            for name in names:
                if not any(r[:2] == (kind, name) for r in report):
                    report.append((kind, name, "FAIL", str(exc)))
    print("GAME  MECH  RESULT  DETAIL")
    for row in report:
        print("  ".join(row), flush=True)
    return int(any(r[2] == "FAIL" for r in report))


if __name__ == "__main__":
    sys.exit(main())
