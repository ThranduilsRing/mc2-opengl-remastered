"""Pins the invariant behind the walk-key reuse in TG_MultiShape::TransformMultiShape_BuildRecipe (mclib/msl_node_walk.h):
no code other than the walk (msl_transform.cpp), the sim-pose publish on private sim shapes (sim_pose.cpp, guarded by
gosASSERT(!walkKey.valid)) and shape construction (msl.cpp, identity) assigns a TG_ShapeRec's *ShapeToWorld / worldToShape.
A new writer would leave the keys claiming the matrices are current when they are not.

Run:
  py -3 -B -m pytest scripts/tests/test_msl_matrix_writers.py -q -p no:cacheprovider
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ALLOWED = {"mclib/msl.cpp", "mclib/msl_transform.cpp", "mclib/sim_pose.cpp"}
SKIP_DIRS = {"gosfx", "mlr", "build", "build64", "build64-ninja", ".git"}
# rec.shapeToWorld = / p->localShapeToWorld = / [i].worldToShape = (not ==), plus memcpy/std::copy into them
WRITE = re.compile(r"(?:\.|->)\s*(?:localShapeToWorld|shapeToWorld|worldToShape)\s*(?:=(?!=)|\[)|"
                   r"(?:memcpy|memmove|copy)\s*\(\s*&?[\w\.\->\[\]]*(?:localShapeToWorld|shapeToWorld|worldToShape)")


def sources():
    for sub in ("mclib", "code", "GameOS"):
        for d, dirs, files in os.walk(os.path.join(ROOT, sub)):
            dirs[:] = [x for x in dirs if x not in SKIP_DIRS and not x.startswith("build")]
            for f in files:
                if f.endswith((".cpp", ".h", ".inl")):
                    yield os.path.join(d, f)


def test_only_known_writers_touch_shape_matrices():
    offenders = []
    for path in sources():
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        if rel in ALLOWED:
            continue
        with open(path, encoding="utf-8", errors="replace") as fh:
            for n, line in enumerate(fh, 1):
                code = line.split("//", 1)[0]
                if WRITE.search(code):
                    offenders.append(f"{rel}:{n}: {line.strip()}")
    # gosfx/mlr use unrelated structs with the same member names and are skipped by directory.
    assert not offenders, "new writer of TG_ShapeRec shape matrices; keep the walk-key invariant:\n" + "\n".join(offenders)


def test_allowed_writers_are_still_the_ones_we_reviewed():
    sim = open(os.path.join(ROOT, "mclib", "sim_pose.cpp"), encoding="utf-8").read()
    assert "gosASSERT(!rec.walkKey.valid)" in sim, "PublishSimPose lost its private-sim-shape guard"
    walk = open(os.path.join(ROOT, "mclib", "msl_transform.cpp"), encoding="utf-8").read()
    assert "evalNode(" in walk and "refreshWalkOrder(" in walk
