"""Engine catalog contract.

``openswmm/engine/catalog.json`` is the machine-readable map of the Python API
that the MCP server and gymnasium consume. Two guarantees are checked here:

* **Source contract** (no compiled engine needed): the committed catalog equals
  what ``python/scripts/gen_catalog.py`` generates from the current stubs,
  Cython sources and headers; every public class is reachable or deliberately
  excluded; unit coverage has not regressed.
* **Runtime contract** (compiled engine): every catalogued property and method
  exists on its runtime class, and every Solver-rooted target resolves on an
  opened model.
"""

from __future__ import annotations

import importlib
import importlib.util
import json
import os
import sys
import unittest
from pathlib import Path

_PYTHON_DIR = Path(__file__).resolve().parents[1]
_GENERATOR = _PYTHON_DIR / "scripts" / "gen_catalog.py"
_CATALOG = _PYTHON_DIR / "openswmm" / "engine" / "catalog.json"

try:
    import openswmm.engine as _engine
    from openswmm.engine import catalog as _catalog
except ImportError:  # pragma: no cover - source-only environments
    _engine = None
    _catalog = None


class CatalogSourceContract(unittest.TestCase):
    def test_catalog_is_fresh_and_complete(self):
        spec = importlib.util.spec_from_file_location("gen_catalog", _GENERATOR)
        gen = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = gen
        spec.loader.exec_module(gen)
        self.assertEqual(gen.main(["--check"]), 0,
                         "catalog.json is stale or incomplete; run python/scripts/gen_catalog.py")

    def test_every_element_target_names_its_collection(self):
        targets = json.loads(_CATALOG.read_text(encoding="utf-8"))["targets"]
        for name, entry in targets.items():
            if entry["path"].endswith("[]"):
                self.assertIn("collection", entry, name)
                self.assertIn(entry["collection"], targets, name)


@unittest.skipIf(_engine is None, "compiled openswmm.engine not installed")
class CatalogRuntimeContract(unittest.TestCase):
    def test_members_exist_on_runtime_classes(self):
        missing = []
        for name, entry in _catalog.targets().items():
            module = importlib.import_module(f"openswmm.engine.{entry['module']}")
            cls = getattr(module, entry["class"])
            for m in _catalog.members(name):
                if m["form"] == "item" or "variant" in m:
                    continue
                if not hasattr(cls, m["name"]):
                    missing.append(m["path"])
        self.assertEqual(missing, [], "catalog members absent at runtime")

    def test_solver_targets_resolve_on_an_opened_model(self):
        from tests._paths import SITE_DRAINAGE_INP, artifact_dir

        out_dir = artifact_dir(self)
        solver = _engine.Solver(SITE_DRAINAGE_INP, os.path.join(out_dir, "catalog.rpt"),
                                os.path.join(out_dir, "catalog.out"))
        solver.open()
        try:
            for name, entry in _catalog.targets().items():
                if "construct" in entry or entry.get("subtypes"):
                    continue
                if name.startswith("surface2d") and not solver.surface2d.is_active:
                    continue
                key = None
                if entry["path"].endswith("[]") or "[]." in entry["path"]:
                    collection = _catalog.resolve(solver, _collection_of(entry))
                    if len(collection) == 0:
                        continue
                    key = 0
                with self.subTest(target=name):
                    self.assertIsNotNone(_catalog.resolve(solver, name, key))
        finally:
            solver.close()

    def test_lookup_reports_units_and_bulk_path(self):
        entry = _catalog.lookup("node.depth")
        self.assertEqual(entry["units"], "length")
        self.assertEqual(entry["bulk"], "nodes.depths")
        with self.assertRaises(KeyError):
            _catalog.lookup("node.not_a_field")


def _collection_of(entry: dict) -> str:
    """Name of the collection target that owns an element (or sub-view) target."""
    targets = _catalog.targets()
    while "collection" not in entry:
        entry = targets[entry["parent"]]
    return entry["collection"]


if __name__ == "__main__":
    unittest.main()
