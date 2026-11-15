#!/usr/bin/env python3
"""BT2018-MECH-MATERIAL-PACKAGE-SCAFFOLD-1 checker — the consistency rung of the
material-package feature ladder (package -> featureBits -> handles -> shader -> CHECK).

For each deployed <chassis>.package.json, verify package/data agreement:
  - albedo declared  -> its .ktx2 (or .tga) exists in deploy data/tgl
  - HAS_AO in featureBits  IFF  materials.ao declared
  - if materials.ao declared -> its dump source (-Base-amb.png) exists
  - AO logicalFormat is linear-grayscale (never sRGB/color) and container documented
  - HAS_NORMAL in featureBits  IFF  materials.normal declared
  - if materials.normal declared -> dump source (-Base-nrm.png) exists, cooked .ktx2
    exists, logicalFormat is linear-normal-xyz (engine derives HAS_NORMAL at runtime
    from whether the .ktx2 loads; this checker validates the bit anyway)

Exit 0 = consistent. Non-zero = packages and data disagree. Read-only.

Usage:
  py -3 check_mech_packages.py --deploy <release>/data/tgl \
      --dump A:/Games/mc2-opengl/BattleTech_2018_Dump
"""
import argparse, glob, json, os, sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--deploy", required=True)
    ap.add_argument("--dump", default="A:/Games/mc2-opengl/BattleTech_2018_Dump")
    args = ap.parse_args()
    tex2d = os.path.join(args.dump, "Texture2D")

    pkgs = sorted(glob.glob(os.path.join(args.deploy, "**", "*.package.json"), recursive=True))
    if not pkgs:
        sys.exit(f"FAIL: no *.package.json in {args.deploy}")

    errors, warns, n_ao, n_nrm = [], [], 0, 0
    for p in pkgs:
        name = os.path.basename(p)[:-len(".package.json")]
        d = json.load(open(p, encoding="utf-8"))
        if "clips" in d and (not isinstance(d["clips"], dict) or
                             not isinstance(d["clips"].get("showcase"), str) or not d["clips"]["showcase"].strip()):
            errors.append(f"{name}: clips.showcase must be a nonempty clip name")
        mats = d.get("materials", {})
        feats = set(d.get("featureBits", []))

        if "albedo" not in mats:
            errors.append(f"{name}: no albedo declared")
        else:
            a = mats["albedo"]
            if not (os.path.isfile(os.path.join(args.deploy, a.get("ktx2", "")))
                    or os.path.isfile(os.path.join(args.deploy, a.get("tga", "")))):
                errors.append(f"{name}: albedo artifact missing ({a.get('ktx2')})")

        has_ao_bit = "HAS_AO" in feats
        has_ao_mat = "ao" in mats
        if has_ao_bit != has_ao_mat:
            errors.append(f"{name}: HAS_AO bit({has_ao_bit}) != materials.ao({has_ao_mat})")
        if has_ao_mat:
            n_ao += 1
            ao = mats["ao"]
            if ao.get("logicalFormat") != "linear-grayscale":
                errors.append(f"{name}: AO logicalFormat must be linear-grayscale, got {ao.get('logicalFormat')!r}")
            if "container" not in ao:
                warns.append(f"{name}: AO container not documented")
            src = ao.get("source", "")
            if src.startswith("dump:Texture2D/"):
                if not os.path.isfile(os.path.join(tex2d, src.split("/", 1)[1])):
                    errors.append(f"{name}: AO dump source missing ({src})")
            else:
                errors.append(f"{name}: AO source not a dump ref ({src!r})")
        has_nrm_bit = "HAS_NORMAL" in feats
        has_nrm_mat = "normal" in mats
        if has_nrm_bit != has_nrm_mat:
            errors.append(f"{name}: HAS_NORMAL bit({has_nrm_bit}) != materials.normal({has_nrm_mat})")
        if has_nrm_mat:
            n_nrm += 1
            nm = mats["normal"]
            if nm.get("logicalFormat") != "linear-normal-xyz":
                errors.append(f"{name}: normal logicalFormat must be linear-normal-xyz, got {nm.get('logicalFormat')!r}")
            if "container" not in nm:
                warns.append(f"{name}: normal container not documented")
            nsrc = nm.get("source", "")
            if nsrc.startswith("dump:Texture2D/"):
                if not os.path.isfile(os.path.join(tex2d, nsrc.split("/", 1)[1])):
                    errors.append(f"{name}: normal dump source missing ({nsrc})")
            else:
                errors.append(f"{name}: normal source not a dump ref ({nsrc!r})")
            # cooked artifact (HAS_NORMAL derives at runtime from the .ktx2 loading)
            if not os.path.isfile(os.path.join(args.deploy, nm.get("ktx2", ""))):
                errors.append(f"{name}: normal artifact missing ({nm.get('ktx2')})")

    print(f"[check] packages={len(pkgs)} with-AO={n_ao} with-NORMAL={n_nrm} errors={len(errors)} warns={len(warns)}")
    for w in warns[:10]:
        print("  WARN  " + w)
    if len(warns) > 10:
        print(f"  ... +{len(warns)-10} more warns")
    for e in errors:
        print("  ERROR " + e)
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
