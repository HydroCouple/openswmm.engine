"""
Storage-shape bindings — ``node.storage.shape`` / ``node.storage.geometry``.

Covers the Python surface over the C API added by the storage-shapes work
(``swmm_node_get/set_storage_shape`` and ``swmm_node_get/set_storage_geometry``;
see plans/STORAGE_SHAPES_PLAN_2026-07-12.md and the C round-trip test
tests/unit/engine/test_storage_shape_roundtrip.cpp):

* :class:`StorageShape` enum ordinals mirror ``SWMM_StorageShape``.
* ``shape`` getter returns the enum; setter accepts enum or int.
* ``geometry`` round-trips ``(p1, p2, p3)`` for every geometric shape and
  returns zeros for non-geometric shapes.
* Error paths: invalid shape codes and invalid dimensions raise
  :class:`BadParamError`.

The fixture model is derived from site_drainage_example.inp by moving
junction ``J2`` into a ``[STORAGE]`` section (FUNCTIONAL relation), so the
network connectivity is unchanged. Derived .inp/.rpt/.out files land in the
reviewable ``tests/_artifacts/`` tree.

Requires the compiled engine; the entire module is skipped at collection
when ``openswmm.engine._nodes`` cannot be imported.
"""

from __future__ import annotations

import os
import re
import unittest

try:
    import openswmm.engine._nodes  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import (  # noqa: E402
    BadParamError,
    Solver,
    StorageShape,
)

from tests.engine._solver_cases import (  # noqa: E402
    SITE_DRAINAGE_INP,
    EngineSolverCase,
    artifact_dir,
)


def _derive_storage_model(out_dir):
    """Write a copy of the site-drainage model with J2 as a storage node.

    J2 keeps its name, so every conduit that references it stays valid; it
    starts as a FUNCTIONAL storage (a=1000, b=0, c=0 — constant area).
    """
    with open(SITE_DRAINAGE_INP) as f:
        text = f.read()
    junction_line = re.search(r"(?m)^J2\s+[^\n]*\n", text)
    assert junction_line is not None, "fixture model no longer has junction J2"
    text = text.replace(junction_line.group(0), "", 1)
    assert "[OUTFALLS]" in text
    storage_section = (
        "[STORAGE]\n"
        ";;Name           Elev       MaxDepth   InitDepth  Shape      Params\n"
        "J2               4969       20         0          FUNCTIONAL 1000 0 0\n"
        "\n"
    )
    text = text.replace("[OUTFALLS]", storage_section + "[OUTFALLS]", 1)
    inp = os.path.join(out_dir, "site_drainage_storage.inp")
    with open(inp, "w") as f:
        f.write(text)
    return inp


class StorageSolverCase(EngineSolverCase):
    """Provides an opened Solver whose node ``J2`` is a storage unit."""

    def storage_solver(self) -> Solver:
        d = artifact_dir(self)
        inp = _derive_storage_model(d)
        s = Solver(inp,
                   os.path.join(d, "site_drainage_storage.rpt"),
                   os.path.join(d, "site_drainage_storage.out"))
        s.open()
        self.addCleanup(self._close_destroy, s)
        return s


# ---------------------------------------------------------------------------
# Enum surface
# ---------------------------------------------------------------------------


class TestStorageShapeEnum(unittest.TestCase):
    def test_ordinals_mirror_c_api(self):
        """Values must match SWMM_StorageShape (and legacy StorageType)."""
        expected = {
            "TABULAR": 0,
            "FUNCTIONAL": 1,
            "CYLINDRICAL": 2,
            "CONICAL": 3,
            "PARABOLOID": 4,
            "PYRAMIDAL": 5,
        }
        self.assertEqual({m.name: m.value for m in StorageShape}, expected)


# ---------------------------------------------------------------------------
# shape / geometry properties
# ---------------------------------------------------------------------------


class TestStorageShapeProperty(StorageSolverCase):
    def test_shape_getter_returns_enum(self):
        solver = self.storage_solver()
        shape = solver.nodes["J2"].storage.shape
        self.assertIsInstance(shape, StorageShape)
        self.assertIs(shape, StorageShape.FUNCTIONAL)

    def test_shape_setter_accepts_enum_and_int(self):
        solver = self.storage_solver()
        st = solver.nodes["J2"].storage
        st.shape = StorageShape.CYLINDRICAL
        self.assertIs(st.shape, StorageShape.CYLINDRICAL)
        st.shape = int(StorageShape.CONICAL)
        self.assertIs(st.shape, StorageShape.CONICAL)

    def test_invalid_shape_code_raises(self):
        solver = self.storage_solver()
        with self.assertRaises(BadParamError):
            solver.nodes["J2"].storage.shape = 99


class TestStorageGeometryProperty(StorageSolverCase):
    def test_geometry_zeros_for_non_geometric_shape(self):
        solver = self.storage_solver()
        # FUNCTIONAL is not geometric — the C API documents zeros here.
        self.assertEqual(solver.nodes["J2"].storage.geometry, (0.0, 0.0, 0.0))

    def test_geometry_round_trip_per_shape(self):
        """Every geometric shape stores and returns its raw dimensions."""
        solver = self.storage_solver()
        st = solver.nodes["J2"].storage
        for shape, dims in [
            (StorageShape.CYLINDRICAL, (10.0, 5.0, 0.0)),
            (StorageShape.CONICAL, (10.0, 5.0, 2.0)),
            (StorageShape.PARABOLOID, (10.0, 5.0, 3.0)),  # p3 = height, != 0
            (StorageShape.PYRAMIDAL, (10.0, 5.0, 2.0)),
        ]:
            with self.subTest(shape=shape):
                st.shape = shape
                st.geometry = dims
                self.assertIs(st.shape, shape)
                got = st.geometry
                for g, d in zip(got, dims):
                    self.assertAlmostEqual(g, d, places=6)

    def test_geometry_on_non_geometric_shape_raises(self):
        solver = self.storage_solver()
        st = solver.nodes["J2"].storage
        self.assertIs(st.shape, StorageShape.FUNCTIONAL)
        with self.assertRaises(BadParamError):
            st.geometry = (10.0, 5.0, 2.0)

    def test_invalid_dimensions_raise(self):
        solver = self.storage_solver()
        st = solver.nodes["J2"].storage
        st.shape = StorageShape.CYLINDRICAL
        with self.assertRaises(BadParamError):
            st.geometry = (0.0, 5.0, 2.0)  # p1 must be > 0
        # PARABOLOID additionally requires p3 (height) != 0.
        st.shape = StorageShape.PARABOLOID
        with self.assertRaises(BadParamError):
            st.geometry = (10.0, 5.0, 0.0)

    def test_setter_is_atomic_on_failure(self):
        """Nothing is written when the new dimensions are rejected."""
        solver = self.storage_solver()
        st = solver.nodes["J2"].storage
        st.shape = StorageShape.CYLINDRICAL
        st.geometry = (10.0, 5.0, 0.0)
        with self.assertRaises(BadParamError):
            st.geometry = (-1.0, 5.0, 0.0)
        for g, d in zip(st.geometry, (10.0, 5.0, 0.0)):
            self.assertAlmostEqual(g, d, places=6)
