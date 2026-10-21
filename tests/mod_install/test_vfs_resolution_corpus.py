"""Resolver conformance corpus: tools/mod_install/resolver.py must agree with
tests/fixtures/vfs_resolution.json on provider AND bytes.

The same fixture drives the engine ladder (tests/unit/test_file_family_contract.cpp)
and the editor resolver (editor/tests/test_catalog.cpp); this file is the static-
resolver arm.  Keeping the three on one specification is the point: a resolver
that merely agrees on "exists" would pass none of them.

Sandbox note: pytest's tmp dir is not writable here (tempfile.mkdtemp creates a
directory that the sandbox then refuses to let us populate), so the deploy tree
is built under a CWD-relative scratch root and removed in a finally block.
"""

import json
import os
import shutil
import struct
import sys
import unittest
import zlib

_TOOLS = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "tools", "mod_install"))
if _TOOLS not in sys.path:
    sys.path.insert(0, _TOOLS)

from resolver import ResolverConfig, load_config, normalize_key, parse_mod_deps, resolve  # noqa: E402

_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_FIXTURE = os.path.join(_REPO, "tests", "fixtures", "vfs_resolution.json")

FASTFILE_VERSION = 0xCADDECAF
ENTRY_TABLE_START = 8
_ENTRY = struct.Struct("<IIII250s")


def load_fixture(path=_FIXTURE):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def write_bytes(root, rel, data):
    target = os.path.join(root, rel.replace("/", os.sep))
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode("utf-8"))
    return target


def write_fst(path, entries):
    """Author a stock-shaped FastFile archive: zlib entries in a fixed table."""
    encoded = []
    for entry in entries:
        plain = entry["bytes"].encode("utf-8")
        compressed = zlib.compress(plain, 6)
        encoded.append((entry["name"], compressed, plain))
    offset = ENTRY_TABLE_START + _ENTRY.size * len(encoded)
    with open(path, "wb") as f:
        f.write(struct.pack("<II", FASTFILE_VERSION, len(encoded)))
        for name, compressed, plain in encoded:
            f.write(_ENTRY.pack(offset, len(compressed), len(plain), 0, name.encode("ascii")))
            offset += len(compressed)
        for _, compressed, _ in encoded:
            f.write(compressed)


def read_fst_member(path, key):
    with open(path, "rb") as f:
        data = f.read()
    _, count = struct.unpack_from("<II", data, 0)
    for i in range(count):
        off, size, real, _, raw_name = _ENTRY.unpack_from(data, ENTRY_TABLE_START + i * _ENTRY.size)
        name = raw_name.split(b"\x00", 1)[0].decode("ascii")
        if normalize_key(name) == key:
            payload = data[off:off + size]
            return payload if size == real else zlib.decompress(payload)
    raise AssertionError(f"{path}: no member {key!r}")


class Deploy:
    """Materialize the fixture into an empty directory (no tempfile: see module doc)."""

    def __init__(self, fixture):
        self.root = os.path.abspath(".scratch/vfs-resolution-corpus")
        shutil.rmtree(self.root, ignore_errors=True)
        os.makedirs(self.root)
        for item in fixture["files"]:
            write_bytes(self.root, item["path"], item["bytes"])
        for archive in fixture["fst_files"]:
            path = os.path.join(self.root, archive["path"])
            write_fst(path, archive["entries"])
            # resolver.py reads the text sidecar (fst_listing.py's <archive>.fst.txt)
            listing = "\n".join(normalize_key(entry["name"]) for entry in archive["entries"]) + "\n"
            write_bytes(self.root, archive["path"] + ".txt", listing)

    def config(self, fixture, **overrides):
        settings = fixture["config"]
        options = dict(
            game_dir=self.root,
            active_mod=settings["active_mod"],
            dependencies=list(settings["dependencies"]),
            # resolver.py consumes the text sidecar of each archive.
            fst_files=[os.path.join(self.root, name + ".txt") for name in settings["fst"]],
        )
        options.update(overrides)
        return ResolverConfig(**options)

    def provider(self, record):
        """(root-relative path, bytes) for a resolver record."""
        if record["layer"] == "fastfile":
            for archive in ("first.fst", "second.fst"):
                try:
                    return "", read_fst_member(os.path.join(self.root, archive), record["key"])
                except AssertionError:
                    continue
            raise AssertionError(f"fastfile membership with no archive member: {record!r}")
        if not record["path"]:
            return "", b""
        path = os.path.join(self.root, record["path"])
        return os.path.relpath(path, self.root).replace(os.sep, "/").lower(), open(path, "rb").read()

    def cleanup(self):
        shutil.rmtree(self.root, ignore_errors=True)


class ResolverConformanceCorpus(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = load_fixture()
        cls.deploy = Deploy(cls.fixture)

    @classmethod
    def tearDownClass(cls):
        cls.deploy.cleanup()

    def test_fixture_covers_every_axis(self):
        """Guard the corpus itself: a pruned fixture must not silently shrink coverage."""
        keys = {normalize_key(row["req"]) for row in self.fixture["requests"]}
        layers = {row["layer"] for row in self.fixture["requests"]}
        # base-strip is engine-only (the editor resolver has no numeric-subdir
        # strip step), so it stays out of the shared corpus; test_resolver.py
        # and the engine tests cover it.
        self.assertEqual(layers, {"mod:active", "mod:first", "mod:second",
                                  "base-loose", "fastfile", "MISS"})
        self.assertTrue(keys & {"data/art/shared.tga", "data/art/first-only.tga", "data/art/second-only.tga"})
        self.assertTrue(any("\\" in row["req"] for row in self.fixture["requests"]))
        # Ordered dependencies: one key only "first" and "second" both carry, so
        # the winner proves dep[0] > dep[1] rather than "some mod hit".
        self.assertTrue(any(row["req"] == "data/art/dep-order.tga" and row["layer"] == "mod:first"
                            and row["shadowed"] == ["mod:second"]
                            for row in self.fixture["requests"]))
        self.assertTrue(any(row["req"] != normalize_key(row["req"]) for row in self.fixture["requests"]))
        self.assertTrue(any(".hidden" in row["req"] or ".dot" in row["req"] or "scratch" in row["req"]
                            for row in self.fixture["requests"]))
        self.assertTrue(any(row["bytes"] is None for row in self.fixture["requests"]))
        self.assertGreaterEqual(len(self.fixture["fst_files"]), 2)
        self.assertEqual([row["path"] for row in self.fixture["requests"] if row["layer"] == "fastfile"],
                         [""] * len([row for row in self.fixture["requests"] if row["layer"] == "fastfile"]))

    def test_every_request_matches_provider_bytes_and_shadowed(self):
        config = self.deploy.config(self.fixture)
        for row in self.fixture["requests"]:
            with self.subTest(req=row["req"]):
                record = resolve(row["req"], config)
                self.assertEqual(record["layer"], row["layer"])
                self.assertEqual(record["key"], normalize_key(row["req"]))
                relative, data = self.deploy.provider(record)
                if row["bytes"] is None:
                    self.assertEqual(relative, "")
                    self.assertEqual(data, b"")
                else:
                    self.assertEqual(relative, row["path"])
                    self.assertEqual(data, row["bytes"].encode("utf-8"))
                self.assertEqual(record["shadowed"], row["shadowed"])

    def test_requests_do_not_depend_on_order(self):
        """Resolve back to front: a shared index must not leak state between rows."""
        config = self.deploy.config(self.fixture)
        for row in reversed(self.fixture["requests"]):
            record = resolve(row["req"], config)
            self.assertEqual((record["layer"], record["shadowed"]), (row["layer"], row["shadowed"]))

    def test_env_dependencies_match_engine_parser(self):
        self.assertEqual(parse_mod_deps("first, second; first ;;"), ["first", "second"])
        self.assertEqual(parse_mod_deps(None), [])
        self.assertEqual(parse_mod_deps(""), [])

    def test_load_config_reads_mod_deps_like_the_engine(self):
        previous = os.environ.get("MC2_MOD_DEPS")
        try:
            os.environ["MC2_MOD_DEPS"] = " second ;first,second "
            config = load_config(self.deploy.root, active_mod="active", fst_files=[])
            self.assertEqual(config.dependencies, ["second", "first"])
            record = resolve("data/art/second-only.tga", config)
            self.assertEqual(record["layer"], "mod:second")
            os.environ["MC2_MOD_DEPS"] = ""
            config = load_config(self.deploy.root, active_mod="active", fst_files=[])
            self.assertEqual(config.dependencies, [])
            self.assertEqual(resolve("data/art/second-only.tga", config)["layer"], "MISS")
        finally:
            if previous is None:
                os.environ.pop("MC2_MOD_DEPS", None)
            else:
                os.environ["MC2_MOD_DEPS"] = previous


if __name__ == "__main__":
    unittest.main()
