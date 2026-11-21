"""ASSET-PARITY-1: lane installs carry v0.4's data/, assets/ and *.fst, and captures refuse drift."""
import itertools
import csv
import hashlib
import importlib.util
import json
import os
import shutil
import stat
import sys
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import asset_parity as ap  # noqa: E402
import console_child  # noqa: E402

_COUNTER = itertools.count()


@pytest.fixture
def work():
    """A plain scratch dir (work's mkdtemp mode 0700 is unenumerable under the lane sandbox)."""
    base = Path(os.environ.get("MC2_ASSET_PARITY_TEST_ROOT") or tempfile.gettempdir())
    d = base / f"asset-parity-{os.getpid()}-{next(_COUNTER)}"
    d.mkdir(parents=True, exist_ok=True)
    try:
        yield d
    finally:
        shutil.rmtree(d, ignore_errors=True)

SHARED = {
    "data/textures/1.2.txm": b"transition",
    "data/textures/256/cement_1.tga": b"tga",
    "data/art/mechicons/anubis.png": b"png",
    "assets/graphics/agencyfb11.bmp": b"bmp",
    "textures.fst": b"fst-archive",
    "tgl.fst": b"tgl-archive",
}
EXCLUDED = {
    "data/textures/_txm_cache_bak/1.2.txm": b"bak",
    "data/textures/txm_backup/1.txm": b"bak",
}
NOT_PARITY = {
    "mc2.exe": b"exe", "shaders/vulkan/a.spv": b"spv", "mods/x/mod.json": b"m",
    "cache/content-model/a.mci": b"c", "options.cfg": b"cfg",
}


def tree(root, files):
    for rel, data in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return root


@pytest.fixture
def ref(work):
    return tree(work / "v04", {**SHARED, **EXCLUDED, **NOT_PARITY})


def test_index_covers_data_assets_and_root_fst_but_no_backups(ref):
    index = ap.reference_index(ref)
    assert set(index) == {rel.casefold() for rel in SHARED}
    assert ap.is_excluded("DATA\\Textures\\_txm_cache_bak\\1.2.txm")
    assert not ap.is_excluded("data/textures/1.2.txm")


def test_check_names_missing_and_size_differences(ref, work):
    inst = tree(work / "lane", SHARED)
    assert ap.compare(inst, ref).ok
    (inst / "data/textures/1.2.txm").unlink()
    (inst / "textures.fst").write_bytes(b"fst-archivE")          # same size, other byte (sampled)
    (inst / "assets/graphics/agencyfb11.bmp").write_bytes(b"bmp-but-longer")   # size differs
    result = ap.compare(inst, ref)
    assert result.missing == ["data/textures/1.2.txm"]
    assert result.size == ["assets/graphics/agencyfb11.bmp"]
    assert result.content == ["textures.fst"]
    assert result.total == len(SHARED)
    line = ap.verdict_line(result, ref)
    assert line.startswith("[ASSET_PARITY] FAIL ") and "missing=1" in line and "size=1" in line and "content=1" in line


def test_sync_copies_only_what_differs_and_replaces_shared_inodes(ref, work):
    inst = tree(work / "lane", SHARED)
    (inst / "data/textures/1.2.txm").unlink()
    (inst / "tgl.fst").write_bytes(b"old")
    before = (ref / "data/textures/1.2.txm").stat().st_mtime_ns
    result = ap.enforce(inst, ref, sync_missing=True)
    assert result.ok and result.synced == 2
    assert (inst / "data/textures/1.2.txm").read_bytes() == b"transition"
    assert (inst / "tgl.fst").read_bytes() == b"tgl-archive"
    assert (ref / "data/textures/1.2.txm").stat().st_mtime_ns == before      # reference untouched
    assert ap.verdict_line(result, ref).startswith("[ASSET_PARITY] SYNCED ")
    assert ap.compare(inst, ref).ok


def test_no_sync_leaves_the_install_alone(ref, work):
    inst = tree(work / "lane", SHARED)
    (inst / "textures.fst").unlink()
    result = ap.enforce(inst, ref, sync_missing=False)
    assert not result.ok and result.missing == ["textures.fst"]
    assert not (inst / "textures.fst").exists()


def test_reference_and_golden_are_never_targets(ref, work):
    assert ap.is_exempt(ref, ref)
    golden = work / "mc2-win64-v0.4c"
    golden.mkdir()
    assert ap.is_exempt(golden, ref)
    assert ap.enforce(ref, ref) is None
    assert ap.enforce(golden, ref) is None
    assert ap.capture_drift(ref, ref) is None


def test_no_reference_on_this_host_is_a_skip_not_a_failure(ref, work):
    missing_ref = work / "no-v04"
    assert ap.enforce(tree(work / "lane", SHARED), missing_ref) is None
    assert ap.capture_drift(tree(work / "lane2", SHARED), missing_ref) is None


def test_payload_owned_and_per_install_files_are_not_synced_from_the_reference(work):
    ref = tree(work / "v04", {"data/defs/ui/page.fit": b"v04-page", "data/tgl/x.ini": b"v04",
                                  "data/tgl/x.ini.cookstamp": b"1"})
    inst = tree(work / "inst", {"data/defs/ui/page.fit": b"lane-page", "data/tgl/x.ini": b"lane",
                                    "data/tgl/x.ini.cookstamp": b"2"})
    result = ap.enforce(inst, ref, skip={"data/defs/ui/page.fit", "data/tgl/x.ini"})
    assert result.ok and result.synced == 0 and result.total == 0
    assert (inst / "data/defs/ui/page.fit").read_bytes() == b"lane-page"
    assert (inst / "data/tgl/x.ini").read_bytes() == b"lane"
    # A file the deploy does NOT ship is still synced from the reference.
    (inst / "data/tgl/new.ini").unlink(missing_ok=True)
    ref2 = tree(work / "v04b", {"data/tgl/y.ini": b"v04-y"})
    assert ap.enforce(inst, ref2).synced == 1
    assert (inst / "data/tgl/y.ini").read_bytes() == b"v04-y"


def test_sample_is_deterministic_and_spread_over_directories(work):
    files = {f"data/textures/{i:04d}.tga": b"x" for i in range(50)}
    ref = tree(work / "v04", files)
    rels = [rel for rel, _ in ap.reference_index(ref).values()]
    picks = ap.sample_rels(rels, 10)
    assert picks == ap.sample_rels(rels, 10) and len(picks) == 10
    assert len({rel.split("/")[2] for rel in picks[:5]}) > 1
    assert ap.sample_rels(rels, 100) == sorted(rels)


def test_cli_check_reports_counts_and_never_copies(ref, work, capsys):
    inst = tree(work / "lane", SHARED)
    (inst / "tgl.fst").unlink()
    assert ap.main([str(inst), "--check", "--ref", str(ref)]) == 1
    out = capsys.readouterr().out
    assert "[ASSET_PARITY] FAIL" in out and "missing=1" in out
    assert not (inst / "tgl.fst").exists()
    assert ap.main([str(inst), "--sync", "--ref", str(ref)]) == 0
    assert (inst / "tgl.fst").read_bytes() == b"tgl-archive"


def test_capture_drift_reports_but_does_not_repair(ref, work):
    inst = tree(work / "lane", SHARED)
    (inst / "assets/graphics/agencyfb11.bmp").write_bytes(b"different")
    drift = ap.capture_drift(inst, ref)
    assert not drift.ok and drift.size == ["assets/graphics/agencyfb11.bmp"]
    assert (inst / "assets/graphics/agencyfb11.bmp").read_bytes() == b"different"


def test_capture_tools_refuse_drift_unless_allowed():
    """run_visual_capture and visual_verify/capture.py both parse --allow-asset-drift and call capture_drift."""
    src = (ROOT / "scripts/run_visual_capture.py").read_text(encoding="utf-8")
    assert '"--allow-asset-drift"' in src and "asset_parity.capture_drift(exe.parent)" in src
    cap = (ROOT / "tools/visual_verify/capture.py").read_text(encoding="utf-8")
    assert '"--allow-asset-drift"' in cap and "asset_parity.capture_drift(install)" in cap
    ab = (ROOT / "tools/visual_verify/ab.py").read_text(encoding="utf-8")
    assert "--allow-asset-drift" in ab and "allow_asset_drift=a.allow_asset_drift" in ab


def test_deploy_calls_parity_with_sync_and_no_sync_fails():
    src = (ROOT / "scripts/deploy_payload.py").read_text(encoding="utf-8")
    assert "--no-asset-sync" in src
    assert "asset_parity.enforce(target, sync_missing=not args.no_asset_sync" in src
    assert "asset_parity.PREFIX" in src
    smoke = (ROOT / "scripts/run_smoke.py").read_text(encoding="utf-8")
    assert "asset_parity.capture_drift" in smoke and "verdict_line" in smoke


def test_extras_are_reported_and_sync_preserves_unowned_assets(ref, work, capsys):
    extras = {f"data/vegetation/fern_{i}.ktx2": b"fern" for i in range(8)}
    keep = {"data/new-owned.ktx2": b"owned", "data/tgl/a.tga.cookstamp": b"receipt"}
    inst = tree(work / "lane", {**SHARED, **extras, **keep, **EXCLUDED, **NOT_PARITY})
    assert ap.main([str(inst), "--check", "--ref", str(ref)]) == 0
    assert "extra=9" in capsys.readouterr().out
    assert all((inst / rel).exists() for rel in extras)
    skip = {"data/new-owned.ktx2"}
    result = ap.compare(inst, ref, skip=skip)
    assert result.extra == sorted(extras)
    (inst / next(iter(extras))).chmod(stat.S_IREAD)
    result = ap.enforce(inst, ref, skip=skip)
    assert result.extra == sorted(extras)
    assert result.ok and result.deleted == 0
    assert all((inst / rel).read_bytes() == b"fern" for rel in extras)
    assert all((inst / rel).is_file() for rel in {**keep, **EXCLUDED, **NOT_PARITY})
    assert ap.compare(inst, ref, skip=skip).ok


def test_atomic_sync_replaces_readonly_hardlink_without_writing_peer(ref, work):
    peer = tree(work / "peer", {"tgl.fst": b"old"}) / "tgl.fst"
    inst = tree(work / "lane", SHARED)
    dst = inst / "tgl.fst"
    dst.unlink()
    os.link(peer, dst)
    dst.chmod(stat.S_IREAD)
    try:
        result = ap.enforce(inst, ref)
        assert result.ok and result.synced == 1
        assert dst.read_bytes() == b"tgl-archive"
        assert peer.read_bytes() == b"old"
        assert not peer.samefile(dst)
    finally:
        peer.chmod(stat.S_IWRITE | stat.S_IREAD)


def test_deploy_replaces_readonly_payload(work, monkeypatch):
    import deploy_payload as dp
    monkeypatch.setattr(dp, "ensure_terrain_materials", lambda target: {})
    monkeypatch.setattr(dp, "ensure_engine_normal_cooks", lambda target: None)
    src = tree(work / "src", {"data/a.fit": b"new"}) / "data/a.fit"
    inst = tree(work / "lane", {"data/a.fit": b"old"})
    dst = inst / "data/a.fit"
    dst.chmod(stat.S_IREAD)
    dp.deploy([(str(src), "data/a.fit", "defs")], str(inst), False)
    assert dst.read_bytes() == b"new"


@pytest.mark.parametrize("seed_valid", [True, False])
def test_tree_cook_reuses_only_hash_pinned_published_assets(work, monkeypatch, seed_valid):
    import deploy_payload as dp
    content = b"pinned tree"
    rel = "data/tgl/128/palm.ktx2"
    root = tree(work / "src", {"data/kit/tree-upscale.json": json.dumps(
        {"palm": {"ktx2_sha256": hashlib.sha256(content).hexdigest()}}).encode()})
    seed = tree(work / "seed", {rel: content if seed_valid else b"wrong tree!"})
    ref = tree(work / "published", {rel: content})
    monkeypatch.setenv("MC2_LEAN_SEED", str(seed))
    monkeypatch.setenv("MC2_ASSET_PARITY_REF", str(ref))
    def no_cook(*args, **kwargs):
        pytest.fail("verified published tree should avoid the multi-minute cook")
    dp.ensure_tree_upscale(str(root), cooker=no_cook)
    assert (root / rel).read_bytes() == content
    assert not (root / rel).samefile(ref / rel)
    assert (seed / rel).read_bytes() == (content if seed_valid else b"wrong tree!")


def test_tree_cook_rejects_unpinned_cache_and_runs_cooker(work, monkeypatch):
    import deploy_payload as dp
    root = tree(work / "src", {"data/kit/tree-upscale.json": json.dumps(
        {"palm": {"ktx2_sha256": hashlib.sha256(b"pinned").hexdigest()}}).encode()})
    seed = tree(work / "seed", {"data/tgl/128/palm.ktx2": b"corrupt"})
    monkeypatch.setenv("MC2_LEAN_SEED", str(seed))
    monkeypatch.setenv("MC2_ASSET_PARITY_REF", str(work / "missing"))
    called = []
    def cook(*args, **kwargs):
        called.append(args)
        return type("Result", (), {"returncode": 1, "stdout": "", "stderr": ""})()
    dp.ensure_tree_upscale(str(root), cooker=cook)
    assert len(called) == 1
    assert not (root / "data/tgl/128/palm.ktx2").exists()


def test_seed_manifest_disagreement_still_requires_exact_byte_repair(work):
    spec = importlib.util.spec_from_file_location("iso_seed", ROOT / "scripts/make-isolated-deploy.py")
    iso = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(iso)
    seed = tree(work / "seed", {"mc2.exe": b"exe", "data/defs/a.fit": b"line\n"})
    manifest = seed / ".deployed_manifest.csv"
    def write_manifest(defs):
        with manifest.open("w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["manifest_version", "v1"])
            w.writerow(["relpath", "sha256", "bytes", "src_commit", "timestamp", "mtime_ns"])
            for rel, data in {"mc2.exe": b"exe", "data/defs/a.fit": defs}.items():
                w.writerow([rel, hashlib.sha256(data).hexdigest(), len(data), "abc", "", 0])
    write_manifest(b"line\r\n")
    with pytest.raises(SystemExit):
        iso.verified_seed(seed, use_cache=True)
    # Restore independently verified bytes; never bless drift.
    (seed / "data/defs/a.fit").write_bytes(b"line\r\n")
    assert len(iso.verified_seed(seed, use_cache=True)) == 2
    (seed / "data/defs/a.fit").write_bytes(b"evil\n")
    with pytest.raises(SystemExit):
        iso.verified_seed(seed, use_cache=True)


def payload_manifest(install, files, commit="a" * 40):
    import deploy_payload as dp
    dp.write_manifest(str(install), {rel: (hashlib.sha256(data).hexdigest(), len(data))
                                    for rel, data in {"mc2.exe": b"exe", **files}.items()}, commit)


def test_deploy_and_capture_share_exact_payload_ownership(work):
    ref = tree(work / "reference", {"data/defs/owned.fit": b"base", "data/stock.fit": b"stock"})
    files = {"data/defs/owned.fit": b"candidate", "data/new.fit": b"new"}
    install = tree(work / "candidate", {**files, "data/stock.fit": b"stock"})
    hashes = {rel: hashlib.sha256(data).hexdigest() for rel, data in files.items()}
    deployed = ap.enforce(install, ref, payload=hashes, declared={})
    payload_manifest(install, files, commit="unknown")
    captured = ap.capture_drift(install, ref)
    assert deployed.ok and captured.ok and deployed.total == captured.total == 1
    # Missing, extra and same-size corruption outside the payload still refuse.
    (install / "data/stock.fit").write_bytes(b"STOCK")
    assert ap.capture_drift(install, ref).content == ["data/stock.fit"]
    tree(install, {"data/unlisted.fit": b"extra"})
    assert ap.capture_drift(install, ref).extra == ["data/unlisted.fit"]
    (install / "data/stock.fit").unlink()
    assert ap.capture_drift(install, ref).missing == ["data/stock.fit"]
    # Owned bytes are pinned even if outside the sampled set.
    (install / "data/new.fit").write_bytes(b"bad")
    with pytest.raises(ValueError, match="hash mismatch"):
        ap.capture_drift(install, ref)


@pytest.mark.parametrize("state", ["missing", "content"])
def test_declared_cliff_cooks_sync_only_from_pinned_reference(work, state):
    # Trigger paths: scripts/asset_parity.py, intentional-asset-changes.json.
    rels = [f"data/terrain_layers/{name}.ktx2" for name in
            ("cliff_rock_ah", "cliff_rock_nra", "concrete_detail_pad", "concrete_detail_road")]
    pins = json.loads((ROOT / ap.INTENTIONAL).read_text())
    assert all(rel in pins for rel in rels)
    files = dict.fromkeys(rels, b"published cook")
    ref = tree(work / "reference", files)
    install = tree(work / "candidate", {} if state == "missing" else dict.fromkeys(rels, b"incorrect cook"))
    hashes = {rel: hashlib.sha256(data).hexdigest() for rel, data in files.items()}
    with pytest.raises(ValueError, match="INTENTIONAL ASSET hash mismatch"):
        ap.enforce(install, ref, sync_missing=False, payload={}, declared=hashes)
    assert ap.enforce(install, ref, payload={}, declared=hashes).ok
    assert ap.compare(install, ref, payload={}, declared=hashes).ok
    (install / rels[0]).unlink()
    (ref / rels[0]).write_bytes(b"unpublished cook")
    with pytest.raises(ValueError, match="reference hash mismatch"):
        ap.enforce(install, ref, payload={}, declared=hashes)
    assert not (install / rels[0]).exists()


@pytest.fixture
def declaration_repo(work, monkeypatch):
    import subprocess
    import console_child
    root = work / "repo"
    root.mkdir()
    def git(*args):
        return subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True,
                              text=True, **console_child.child_kwargs(redirected=True)).stdout.strip()
    git("init", "-q")
    git("config", "user.email", "test@example.invalid")
    git("config", "user.name", "Parity Test")
    git("commit", "--allow-empty", "-qm", "baseline")
    baseline = git("rev-parse", "HEAD")
    def declare(entries):
        (root / ap.INTENTIONAL).write_text(json.dumps(entries), encoding="utf-8")
        git("add", ap.INTENTIONAL)
        git("commit", "-qm", "declare candidate assets")
        return git("rev-parse", "HEAD")
    return root, baseline, declare


def test_tracked_grass_declarations_are_candidate_only_and_exact(work, declaration_repo, monkeypatch, capsys):
    root, baseline, declare = declaration_repo
    grass = {f"data/vegetation/cards/grass_{n:02d}.png": b"new grass" for n in range(1, 5)}
    expected = {rel: hashlib.sha256(data).hexdigest() for rel, data in grass.items()}
    candidate = declare(expected)
    assert ap.intentional_changes(baseline, root) == {}
    assert ap.intentional_changes(candidate, root) == expected
    # An uncommitted edit cannot broaden the pinned list.
    (root / ap.INTENTIONAL).write_text('{"data/unlisted.png": "bad"}')
    assert ap.intentional_changes(candidate, root) == expected
    install = tree(work / "candidate", grass)
    ref = tree(work / "ref", {rel: b"old grass" for rel in grass})
    payload_manifest(install, {}, candidate)
    original = ap.intentional_changes
    monkeypatch.setattr(ap, "intentional_changes", lambda commit, _root=None: original(commit, root))
    assert ap.capture_drift(install, ref).ok
    assert "INTENTIONAL ASSET ACCEPTED 4 files" in capsys.readouterr().out
    payload_manifest(install, {}, baseline)
    assert not ap.capture_drift(install, ref).ok
    payload_manifest(install, {}, candidate)
    (install / next(iter(grass))).write_bytes(b"bad grass")
    with pytest.raises(ValueError, match="INTENTIONAL ASSET hash mismatch"):
        ap.capture_drift(install, ref)


@pytest.mark.parametrize("rel", ["../data/a.png", "/data/a.png", "data/../../a.png", "mc2.exe", "other.dll"])
def test_declarations_reject_paths_outside_assets_and_auxiliary(declaration_repo, rel):
    root, _, declare = declaration_repo
    commit = declare({rel: "a" * 64})
    with pytest.raises(ValueError, match="invalid path/hash"):
        ap.intentional_changes(commit, root)


def test_ab_auxiliary_requires_equality_or_declaration(work, declaration_repo, monkeypatch, capsys):
    sys.path.insert(0, str(ROOT / "tools/visual_verify"))
    import ab
    root, base, declare = declaration_repo
    monkeypatch.setattr(ab, "ROOT", root)
    ref = tree(work / "baseline", {rel: b"baseline binary" for rel in ap.AUXILIARY})
    install = tree(work / "candidate", {rel: b"baseline binary" for rel in ap.AUXILIARY})
    good = hashlib.sha256(b"baseline binary").hexdigest()
    ma = {rel: good for rel in ap.AUXILIARY}
    mb = {rel: good for rel in ap.AUXILIARY}
    assert ab.candidate_mismatches(ma, mb, base, install, ref) == []
    # A copied manifest cannot hide replaced auxiliary bytes.
    (install / "mc2res_64.dll").write_bytes(b"different")
    mb["mc2res_64.dll"] = hashlib.sha256(b"different").hexdigest()
    assert [p for p, _ in ab.candidate_mismatches(ma, mb, base, install, ref)] == ["mc2res_64.dll"]
    assert [p for p, _ in ab.candidate_mismatches(ma, mb, base, baseline=ref)] == ["mc2res_64.dll"]
    # Even a candidate equal to the baseline cannot excuse arbitrary A bytes.
    assert ab.candidate_mismatches({rel: "a" * 64 for rel in ma}, ma, base, baseline=ref)
    candidate = declare({"mc2res_64.dll": mb["mc2res_64.dll"]})
    assert ab.candidate_mismatches(ma, mb, candidate, install, ref) == []
    assert "sha256=" + mb["mc2res_64.dll"] in capsys.readouterr().out
    assert ab.candidate_mismatches(ma, {}, candidate, baseline=ref)  # missing B stays refused
    assert ab.candidate_mismatches({"render.dll": "a" * 64}, {"render.dll": good}, base, baseline=ref)


def test_ab_declared_grass_accepts_only_candidate_hash(work, declaration_repo, monkeypatch):
    sys.path.insert(0, str(ROOT / "tools/visual_verify"))
    import ab
    root, base, declare = declaration_repo
    monkeypatch.setattr(ab, "ROOT", root)
    rel = "data/vegetation/cards/grass_01.png"
    sha = hashlib.sha256(b"grass").hexdigest()
    candidate = declare({rel: sha})
    ma, mb = {rel: "a" * 64}, {rel: sha}
    assert ab.candidate_mismatches(ma, mb, candidate) == []
    assert ab.candidate_mismatches(ma, mb, base)
    assert ab.candidate_mismatches(ma, {rel: "b" * 64}, candidate)
    assert ab.candidate_mismatches({**ma, "data/unlisted.png": "a" * 64}, mb, candidate)
    install_a = tree(work / "a", {rel: b"old"})
    install_b = tree(work / "b", {rel: b"grass"})
    payload_manifest(install_b, {}, candidate)
    parity = ab.data_parity(install_a, install_b)
    assert parity["cross_data"] and parity["unlisted_differing_count"] == 0
    tree(install_b, {"data/unlisted.png": b"extra"})
    assert ab.data_parity(install_a, install_b)["unlisted_differing_count"] == 1


def test_fail_detail_names_paths_and_reference_moved_cause(ref, work):
    inst = tree(work / "lane", SHARED)
    (inst / ".deployed_manifest.csv").write_text("x")
    os.utime(inst / ".deployed_manifest.csv", (1000, 1000))
    (inst / "data/art/mechicons/anubis.png").write_bytes(b"older")
    tree(inst, {f"data/extra_{i}.bin": b"e" for i in range(10)})
    result = ap.compare(inst, ref, skip=set())
    text = "\n".join(ap.detail_lines(result, inst, ref, limit=3))
    assert "size: data/art/mechicons/anubis.png" in text and "(+7 more)" in text
    assert "extra: data/extra_0.bin" in text and "the reference moved" in text
    # Extras are advisory and preserved for owner review.
    inst2 = tree(work / "lane2", {**SHARED, "data/stale.bin": b"s"})
    os.utime(ref / "data/textures/1.2.txm", (1, 1))
    text = "\n".join(ap.detail_lines(ap.compare(inst2, ref, skip=set()), inst2, ref))
    assert "preserved for owner review" in text and "stale.bin" in text
    assert ap.detail_lines(ap.Result(), inst, ref) == []


def test_pinned_mismatch_error_lists_every_failing_file(work):
    inst = tree(work / "lane", {"data/a.fit": b"a", "data/b.fit": b"b"})
    sha = lambda data: hashlib.sha256(data).hexdigest()
    declared = {"data/a.fit": sha(b"A"), "data/b.fit": sha(b"b"), "data/gone.fit": sha(b"g")}
    with pytest.raises(ValueError) as exc:
        ap.owned_paths(inst, {}, declared)
    msg = str(exc.value)
    assert "2 file(s)" in msg and "data/a.fit (sha256=" in msg and "data/gone.fit (absent" in msg
    assert "data/b.fit" not in msg


# Declared sha = v0.4 reference bytes (enforce() refuses a declaration the reference lacks); a declared AND tracked
# asset must also equal the tracked blob, so v0.4 and the repo agree.


def test_declared_sha_matches_tracked_blob():
    """A declared asset that is also git-tracked must equal the tracked blob, or deploy fails on it."""
    import subprocess
    declared = json.loads((ROOT / ap.INTENTIONAL).read_text(encoding="utf-8"))
    tracked = set(subprocess.run(["git", "-C", str(ROOT), "ls-files", "--", "data", "assets"],
                                 capture_output=True, text=True, check=True,
                                 **console_child.child_kwargs(redirected=True)).stdout.split("\n"))
    bad = []
    for rel, sha in declared.items():
        if rel not in tracked:
            continue
        blob = subprocess.run(["git", "-C", str(ROOT), "show", f"HEAD:{rel}"], capture_output=True, check=True,
                              **console_child.child_kwargs(redirected=True)).stdout
        if hashlib.sha256(blob).hexdigest() != sha:
            bad.append(rel)
    assert not bad, f"{ap.INTENTIONAL} sha differs from the tracked blob: {bad}"


def test_game_written_texture_lists_are_not_parity_files():
    # mclib/terrtxm.cpp rewrites data/textures/<mission>.lst at mission load (merge-1121d detsave drift)
    assert ap.is_excluded("data/textures/mc2_01.lst") and ap.is_excluded(r"Data\Textures\MC2_24.LST")
    assert not ap.is_excluded("data/textures/mc2_01.txm")
    assert not ap.is_excluded("data/textures/sub/other.lst")
