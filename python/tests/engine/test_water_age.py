"""Round-trips and documented refusals for ``solver.water_age``.

Covers:
  * ``water_age.enabled`` — the ``[OPTIONS] WATER_AGE`` toggle.
  * ``water_age.globals`` — the ``[WATER_AGE_SOURCES]`` GLOBAL ages (HOURS)
    as a mapping keyed by :class:`WaterAgeSource`.
  * ``water_age.node_overrides`` — the NODE-scope override rows, the A1a
    scope rule, and pair-keyed ``set`` / ``remove``.
  * ``water_age.save`` — the component-file write path.
  * The **signed** contract: a negative age is legal here (it extracts
    age-volume, D-NS1), so the positive test that it is *accepted* matters
    as much as the negative tests elsewhere.

The model is authored inline because ``site_drainage_model.inp`` carries no
``[WATER_AGE_SOURCES]`` section. Both the ``.inp`` and its ``.age``
component config, and every report/output file, land in
``tests/engine/output`` so a user can review them after a run.

@author: Caleb Buahin
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import (
    BadIndexError,
    BadParamError,
    WaterAgeOverride,
    WaterAgeSource,
)

from tests.engine._solver_cases import EngineSolverCase

_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")

#: Component id the engine registers the water-age coordinator under
#: (``kAgeId`` in ``src/engine/transport/components/WaterAgeModule/
#: WaterAgeComponent.cpp``).
_AGE_COMPONENT = "org.hydrocouple.openswmm.waterage"


def _water_age_model(tag):
    """Write a water-age-enabled ``.inp`` plus its ``.age`` config.

    ``[WATER_AGE_SOURCES]`` rows are ``<source> GLOBAL <hours>`` and
    ``<source> NODE <node> <hours>`` — the spelling ``WaterAgeComponent.cpp``
    parses. The component is bound through ``[PROCESS_COMPONENTS]``.
    """
    os.makedirs(_OUT_DIR, exist_ok=True)
    age_name = f"water_age_{tag}.age"

    with open(os.path.join(_OUT_DIR, age_name), "w") as f:
        f.write(
            "[WATER_AGE_SOURCES]\n"
            "DWF              GLOBAL  4.0\n"
            "EXTERNAL_INFLOW  NODE    J0  6.0\n"
        )

    text = (
        "[TITLE]\n"
        "Python water-age-binding round-trip deck\n"
        "\n[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "QUALITY_SOLVER       EULERIAN_ARD\n"
        "WATER_AGE            ON\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             01:00:00\n"
        "ROUTING_STEP         5\n"
        "REPORT_STEP          00:05:00\n"
        "\n[JUNCTIONS]\n"
        ";;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
        "J0  10.0  10  1.5  0  0\n"
        "J1   9.4  10  1.5  0  0\n"
        "J2   8.8  10  1.5  0  0\n"
        "\n[OUTFALLS]\n"
        "OUT  7.0  FREE  NO\n"
        "\n[CONDUITS]\n"
        "C1  J0  J1   500  0.013  0  0  0\n"
        "C2  J1  J2   500  0.013  0  0  0\n"
        "C3  J2  OUT  500  0.013  0  0  0\n"
        "\n[XSECTIONS]\n"
        "C1  CIRCULAR  2.0  0  0  0\n"
        "C2  CIRCULAR  2.0  0  0  0\n"
        "C3  CIRCULAR  2.0  0  0  0\n"
        "\n[INFLOWS]\n"
        'J0  FLOW  ""  FLOW  1.0  1.0  5\n'
        "\n[PROCESS_COMPONENTS]\n"
        f'{_AGE_COMPONENT} config="{age_name}"\n'
        "\n[REPORT]\n"
        "INPUT NO\n"
    )
    path = os.path.join(_OUT_DIR, f"water_age_{tag}.inp")
    with open(path, "w") as f:
        f.write(text)
    return path


class _WaterAgeCase(EngineSolverCase):
    """``EngineSolverCase`` bound to the inline water-age deck."""

    def solver_files(self):
        # Class AND method: two classes share ``test_container_protocol``,
        # and the method name alone would put both on the same deck file.
        tag = "_".join(self.id().split(".")[-2:])
        inp = _water_age_model(tag)
        base = os.path.splitext(inp)[0]
        return inp, base + ".rpt", base + ".out"


# ---------------------------------------------------------------------------
# Toggle
# ---------------------------------------------------------------------------
class TestWaterAgeEnabled(_WaterAgeCase):

    def test_enabled_reflects_option(self):
        s = self.opened_solver()
        self.assertTrue(s.water_age.enabled)

    def test_repr_does_not_raise(self):
        s = self.opened_solver()
        for view in (s.water_age, s.water_age.globals,
                     s.water_age.node_overrides):
            with self.subTest(view=type(view).__name__):
                self.assertIsInstance(repr(view), str)


# ---------------------------------------------------------------------------
# GLOBAL source ages
# ---------------------------------------------------------------------------
class TestWaterAgeGlobals(_WaterAgeCase):

    def test_container_protocol(self):
        s = self.opened_solver()
        g = s.water_age.globals
        # The C enum's trailing COUNT sentinel is deliberately not mirrored
        # into WaterAgeSource, so len() is the pathway count.
        self.assertEqual(len(g), len(WaterAgeSource))
        self.assertEqual(len(g), 7)
        self.assertEqual(list(g), list(WaterAgeSource))
        self.assertIn(WaterAgeSource.DWF, g)
        self.assertIn("EXTERNAL_INFLOW", g)
        self.assertIn(0, g)
        self.assertNotIn("NOT_A_SOURCE", g)
        self.assertIsInstance(repr(g), str)

    def test_parsed_global_is_visible(self):
        s = self.opened_solver()
        self.assertAlmostEqual(s.water_age.globals[WaterAgeSource.DWF], 4.0)

    def test_roundtrip_every_pathway(self):
        s = self.opened_solver()
        g = s.water_age.globals
        for i, source in enumerate(WaterAgeSource):
            hours = 1.5 * (i + 1)
            with self.subTest(source=source):
                g[source] = hours
                self.assertAlmostEqual(g[source], hours)
                self.assertAlmostEqual(g[source.name], hours)
                self.assertAlmostEqual(g[int(source)], hours)

    def test_negative_global_age_is_accepted(self):
        """Negative is LEGAL here (D-NS1 age-volume extraction), not refused."""
        s = self.opened_solver()
        g = s.water_age.globals
        g[WaterAgeSource.GW] = -2.5
        self.assertAlmostEqual(g[WaterAgeSource.GW], -2.5)
        g[WaterAgeSource.RAINFALL] = -0.001
        self.assertAlmostEqual(g[WaterAgeSource.RAINFALL], -0.001)

    def test_zero_is_accepted(self):
        s = self.opened_solver()
        g = s.water_age.globals
        g[WaterAgeSource.IFACE] = 0.0
        self.assertAlmostEqual(g[WaterAgeSource.IFACE], 0.0)

    def test_unknown_source_key_raises_keyerror(self):
        s = self.opened_solver()
        with self.assertRaises(KeyError):
            s.water_age.globals["NOT_A_SOURCE"]
        with self.assertRaises(KeyError):
            s.water_age.globals[42] = 1.0


# ---------------------------------------------------------------------------
# NODE-scope overrides
# ---------------------------------------------------------------------------
class TestWaterAgeNodeOverrides(_WaterAgeCase):

    def test_container_protocol(self):
        s = self.opened_solver()
        rows = s.water_age.node_overrides
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertIsInstance(row, WaterAgeOverride)
        self.assertEqual(row.source, WaterAgeSource.EXTERNAL_INFLOW)
        self.assertEqual(row.node_index, s.nodes["J0"].index)
        self.assertAlmostEqual(row.hours, 6.0)
        self.assertEqual(rows[-1], row)
        self.assertEqual(list(rows), [row])
        self.assertIsInstance(repr(rows), str)

    def test_index_out_of_range(self):
        s = self.opened_solver()
        with self.assertRaises(IndexError):
            s.water_age.node_overrides[7]

    def test_set_updates_rather_than_duplicates(self):
        s = self.opened_solver()
        rows = s.water_age.node_overrides
        rows.set(WaterAgeSource.EXTERNAL_INFLOW, "J0", 9.25)
        self.assertEqual(len(rows), 1)
        self.assertAlmostEqual(rows[0].hours, 9.25)

    def test_set_appends_a_new_pair(self):
        s = self.opened_solver()
        rows = s.water_age.node_overrides
        rows.set(WaterAgeSource.DWF, "J1", 3.0)
        self.assertEqual(len(rows), 2)
        added = rows[1]
        self.assertEqual(added.source, WaterAgeSource.DWF)
        self.assertEqual(added.node_index, s.nodes["J1"].index)
        self.assertAlmostEqual(added.hours, 3.0)

    def test_negative_override_age_is_accepted(self):
        """Negative is LEGAL for an override too — a naive guard would refuse."""
        s = self.opened_solver()
        rows = s.water_age.node_overrides
        rows.set(WaterAgeSource.DWF, "J1", -1.5)
        by_node = {(r.source, r.node_index): r.hours for r in rows}
        self.assertAlmostEqual(
            by_node[(WaterAgeSource.DWF, s.nodes["J1"].index)], -1.5)

    def test_remove_is_keyed_on_the_pair(self):
        s = self.opened_solver()
        rows = s.water_age.node_overrides
        rows.set(WaterAgeSource.DWF, "J1", 3.0)
        self.assertEqual(len(rows), 2)
        rows.remove(WaterAgeSource.DWF, "J1")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].source, WaterAgeSource.EXTERNAL_INFLOW)

    def test_remove_unknown_pair_raises(self):
        s = self.opened_solver()
        with self.assertRaises(BadIndexError):
            s.water_age.node_overrides.remove(WaterAgeSource.DWF, "J2")

    def test_scope_rule_refuses_non_dwf_non_external(self):
        """Only DWF and EXTERNAL_INFLOW take NODE scope (the A1a rule)."""
        s = self.opened_solver()
        rows = s.water_age.node_overrides
        before = len(rows)
        for source in (WaterAgeSource.RAINFALL, WaterAgeSource.GW,
                       WaterAgeSource.RDII, WaterAgeSource.IFACE,
                       WaterAgeSource.INITIAL_STATE):
            with self.subTest(source=source):
                with self.assertRaises(BadParamError):
                    rows.set(source, "J1", 5.0)
                # Refused, not silently ignored: no row was written.
                self.assertEqual(len(rows), before)

    def test_unknown_node_raises_keyerror(self):
        s = self.opened_solver()
        with self.assertRaises(KeyError):
            s.water_age.node_overrides.set(
                WaterAgeSource.DWF, "NOPE", 1.0)


# ---------------------------------------------------------------------------
# save()
# ---------------------------------------------------------------------------
class TestWaterAgeSave(_WaterAgeCase):

    def test_save_writes_a_component_file(self):
        s = self.opened_solver()
        s.water_age.globals[WaterAgeSource.RDII] = 7.5
        out = os.path.join(_OUT_DIR, "water_age_saved.age")
        s.water_age.save(out)
        self.assertTrue(os.path.exists(out))
        with open(out) as f:
            text = f.read()
        self.assertIn("[WATER_AGE_SOURCES]", text)


if __name__ == "__main__":
    unittest.main()
