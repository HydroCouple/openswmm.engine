"""Option enumeration and build capabilities (2026-10-24).

* ``solver.options`` iterates every standard key the engine accepts
  (``swmm_options_key_count`` / ``swmm_options_key_at``), and each key reads
  back a value that ``swmm_options_set`` accepts.
* ``solver.options.ext`` iterates the keys ``swmm_options_get_ext`` accepts:
  the 2D keys on a model with a 2D surface, then stored extension keys.
* ``capabilities()`` describes the build as JSON data.

Outputs go to ``tests/_artifacts/`` for review.
"""

from __future__ import annotations

import json
import os
import re
import unittest

try:
    from openswmm.engine import HAS_2D, BadParamError, Solver, capabilities, catalog
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from tests._paths import SITE_DRAINAGE_INP, artifact_dir

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TWOD_EXAMPLE_INP = os.path.join(os.path.dirname(_REPO_ROOT), "examples", "2d_complete_example.inp")
_CHECKPOINT_H = os.path.join(os.path.dirname(_REPO_ROOT), "include", "openswmm", "engine",
                             "openswmm_checkpoint.h")


def _opened(test_case, inp: str) -> Solver:
    out = artifact_dir(test_case)
    solver = Solver(inp, os.path.join(out, "model.rpt"), os.path.join(out, "model.out"))
    solver.open()
    test_case.addCleanup(solver.close)
    return solver


class TestOptionEnumeration(unittest.TestCase):
    def test_every_standard_key_reads_and_round_trips(self):
        solver = _opened(self, SITE_DRAINAGE_INP)
        keys = list(solver.options)
        self.assertEqual(len(keys), len(solver.options))
        self.assertGreater(len(keys), 90)
        self.assertEqual(len(keys), len(set(keys)))
        for key in ("FLOW_UNITS", "CRS", "RPT_NODES", "THREADS", "FV_LTS"):
            self.assertIn(key, keys)
        self.assertNotIn("ROUTING_MODEL", keys)  # alias of FLOW_ROUTING
        refused = {}
        for key in keys:
            value = solver.options[key]
            try:
                solver.options[key] = value
            except BadParamError as error:
                refused[key] = f"{value!r}: {error}"
            self.assertEqual(solver.options[key], value, key)
        # Record the snapshot for review.
        with open(os.path.join(artifact_dir(self), "options.json"), "w", encoding="utf-8") as f:
            json.dump({k: solver.options[k] for k in keys}, f, indent=1)
        self.assertEqual(refused, {}, "options whose read value is refused on write")

    def test_dict_snapshot_matches_item_access(self):
        solver = _opened(self, SITE_DRAINAGE_INP)
        snapshot = dict(solver.options)
        self.assertEqual(snapshot["FLOW_UNITS"], solver.options["FLOW_UNITS"])

    def test_ext_keys_list_stored_extension_options(self):
        solver = _opened(self, SITE_DRAINAGE_INP)
        before = list(solver.options.ext)
        solver.options.ext["MY_TOOL_SETTING"] = "on"
        after = list(solver.options.ext)
        self.assertEqual(len(after), len(solver.options.ext))
        self.assertIn("MY_TOOL_SETTING", after)
        self.assertEqual(len(after), len(before) + 1)
        self.assertEqual(solver.options.ext["MY_TOOL_SETTING"], "on")

    @unittest.skipUnless(HAS_2D and os.path.exists(TWOD_EXAMPLE_INP), "needs the 2D build")
    def test_ext_keys_include_2d_options_on_a_2d_model(self):
        solver = _opened(self, TWOD_EXAMPLE_INP)
        keys = list(solver.options.ext)
        self.assertEqual(keys[0], "MESH_FILE")
        for key in ("MAX_TIMESTEP", "DRY_DEPTH", "DISPERSION", "BACKEND"):
            self.assertIn(key, keys)
        for key in keys:
            solver.options.ext[key]  # every listed key reads


class TestCapabilities(unittest.TestCase):
    def test_capabilities_describe_the_build(self):
        caps = capabilities()
        json.dumps(caps)  # JSON-ready
        with open(os.path.join(artifact_dir(self), "capabilities.json"), "w",
                  encoding="utf-8") as f:
            json.dump(caps, f, indent=1)
        self.assertEqual(caps["catalog_revision"], catalog.revision())
        self.assertEqual(caps["features"]["2d"], HAS_2D)
        self.assertGreaterEqual(caps["threads"]["logical_cpus"], 1)
        self.assertIn("inspection", caps["checkpoints"])

    def test_checkpoint_feature_bits_match_the_header(self):
        from openswmm.engine._capabilities import CHECKPOINT_FEATURES

        with open(_CHECKPOINT_H, encoding="utf-8") as f:
            header = f.read()
        # The feature bits are the first block; session-status hints follow a blank line.
        block = header[header.index("#define SWMM_CHECKPOINT_INSPECTION"):]
        block = block[:block.index("\n\n")]
        defined = {name.lower(): 1 << int(bit) for name, bit in re.findall(
            r"#define SWMM_CHECKPOINT_([A-Z_]+) \(1UL << (\d+)\)", block)}
        self.assertEqual(CHECKPOINT_FEATURES, defined)


if __name__ == "__main__":
    unittest.main()
