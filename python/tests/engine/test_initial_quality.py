"""Round-trips and documented refusals for ``solver.initial_quality``.

Covers:
  * The ``[INITIAL_QUALITY]`` row table as a sequence — ``len``, iteration,
    positional and negative indexing, ``repr``.
  * ``set`` upsert semantics keyed on ``(is_link, elem_index, constituent)``:
    setting the same key twice updates in place, it does not append.
  * The reserved constituents ``InitialQuality.WATER_AGE`` and
    ``InitialQuality.TEMPERATURE``, whose values are signed.
  * The documented refusals: a **negative pollutant** value, an unknown
    constituent, an unknown element, and passing neither/both of
    ``node=`` / ``link=``.
  * ``remove`` shifting every later row down by one.

The model is authored inline because ``site_drainage_model.inp`` carries no
``[INITIAL_QUALITY]`` section. The ``.inp`` and every report/output file
land in ``tests/engine/output`` so a user can review them after a run.

@author: Caleb Buahin
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import (
    BadIndexError,
    BadParamError,
    InitialQuality,
    InitialQualityEntry,
)

from tests.engine._solver_cases import EngineSolverCase

_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")


def _initial_quality_model(tag):
    """Write an ``.inp`` carrying ``[INITIAL_QUALITY]`` rows; return its path.

    The row spelling is ``NODE|LINK <element> <constituent> <value>`` — the
    form ``InpWriter.cpp`` emits and ``PostParseResolver.cpp`` resolves.
    ``HEAT_TRANSPORT`` is deliberately left OFF, so the ``__TEMPERATURE__``
    row exercises the engine's warn-and-keep path (stored, classified,
    warned, inert) exactly as the C++ gate deck does.
    """
    os.makedirs(_OUT_DIR, exist_ok=True)
    text = (
        "[TITLE]\n"
        "Python initial-quality binding round-trip deck\n"
        "\n[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "WATER_AGE            YES\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:30:00\n"
        "ROUTING_STEP         5\n"
        "REPORT_STEP          00:05:00\n"
        "\n[JUNCTIONS]\n"
        ";;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
        "J0  10.0  10  0.5  0  0\n"
        "J1   9.0  10  0.5  0  0\n"
        "J2   9.5  10  0.5  0  0\n"
        "\n[OUTFALLS]\n"
        "OUT  7.0  FREE  NO\n"
        "\n[CONDUITS]\n"
        "C1  J0  J1   400  0.013  0  0  0\n"
        "C2  J1  OUT  400  0.013  0  0  0\n"
        "C3  J2  J1   400  0.013  0  0  0\n"
        "\n[XSECTIONS]\n"
        "C1  CIRCULAR  1.5  0  0  0\n"
        "C2  CIRCULAR  1.5  0  0  0\n"
        "C3  CIRCULAR  1.5  0  0  0\n"
        "\n[POLLUTANTS]\n"
        ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit\n"
        "TSS   MG/L  0.0  0.0  0.0  0.0  NO  *  0.0  0.0  5.0\n"
        "\n[INITIAL_QUALITY]\n"
        ";;Scope Element Constituent Value\n"
        "NODE  J1  TSS              12.5\n"
        "NODE  J0  __WATER_AGE__     6.0\n"
        "LINK  C2  __TEMPERATURE__  18.5\n"
        "\n[REPORT]\n"
        "INPUT NO\n"
    )
    path = os.path.join(_OUT_DIR, f"initial_quality_{tag}.inp")
    with open(path, "w") as f:
        f.write(text)
    return path


class _InitialQualityCase(EngineSolverCase):
    """``EngineSolverCase`` bound to the inline [INITIAL_QUALITY] deck."""

    def solver_files(self):
        # Class AND method, so a method name reused by another class can
        # never put two tests on the same deck file.
        tag = "_".join(self.id().split(".")[-2:])
        inp = _initial_quality_model(tag)
        base = os.path.splitext(inp)[0]
        return inp, base + ".rpt", base + ".out"


# ---------------------------------------------------------------------------
# Container protocol
# ---------------------------------------------------------------------------
class TestInitialQualityContainer(_InitialQualityCase):

    def test_reserved_constituent_names(self):
        self.assertEqual(InitialQuality.WATER_AGE, "__WATER_AGE__")
        self.assertEqual(InitialQuality.TEMPERATURE, "__TEMPERATURE__")

    def test_parsed_rows(self):
        s = self.opened_solver()
        iq = s.initial_quality
        self.assertEqual(len(iq), 3)
        rows = list(iq)
        self.assertEqual(len(rows), 3)
        for row in rows:
            self.assertIsInstance(row, InitialQualityEntry)

        first = iq[0]
        self.assertFalse(first.is_link)
        self.assertEqual(first.elem_index, s.nodes["J1"].index)
        self.assertEqual(first.constituent, "TSS")
        self.assertAlmostEqual(first.value, 12.5)

        second = iq[1]
        self.assertEqual(second.constituent, InitialQuality.WATER_AGE)
        self.assertEqual(second.elem_index, s.nodes["J0"].index)
        self.assertAlmostEqual(second.value, 6.0)

        third = iq[2]
        self.assertTrue(third.is_link)
        self.assertEqual(third.elem_index, s.links["C2"].index)
        self.assertEqual(third.constituent, InitialQuality.TEMPERATURE)
        self.assertAlmostEqual(third.value, 18.5)

    def test_negative_index_and_range(self):
        s = self.opened_solver()
        iq = s.initial_quality
        self.assertEqual(iq[-1], iq[2])
        with self.assertRaises(IndexError):
            iq[3]
        with self.assertRaises(IndexError):
            iq[-4]

    def test_repr_does_not_raise(self):
        s = self.opened_solver()
        self.assertIsInstance(repr(s.initial_quality), str)


# ---------------------------------------------------------------------------
# set() round-trips and upsert semantics
# ---------------------------------------------------------------------------
class TestInitialQualitySet(_InitialQualityCase):

    def test_node_pollutant_roundtrip(self):
        s = self.opened_solver()
        iq = s.initial_quality
        iq.set("TSS", 20.0, node="J2")
        self.assertEqual(len(iq), 4)
        added = iq[-1]
        self.assertFalse(added.is_link)
        self.assertEqual(added.elem_index, s.nodes["J2"].index)
        self.assertEqual(added.constituent, "TSS")
        self.assertAlmostEqual(added.value, 20.0)

    def test_link_pollutant_roundtrip(self):
        s = self.opened_solver()
        iq = s.initial_quality
        iq.set("TSS", 0.25, link="C1")
        added = iq[-1]
        self.assertTrue(added.is_link)
        self.assertEqual(added.elem_index, s.links["C1"].index)
        self.assertAlmostEqual(added.value, 0.25)

    def test_element_may_be_named_or_indexed(self):
        s = self.opened_solver()
        iq = s.initial_quality
        iq.set("TSS", 7.0, node=s.nodes["J2"].index)
        self.assertAlmostEqual(iq[-1].value, 7.0)
        self.assertEqual(iq[-1].elem_index, s.nodes["J2"].index)

    def test_upsert_updates_in_place(self):
        """The same (scope, element, constituent) key is an EDIT, not a row."""
        s = self.opened_solver()
        iq = s.initial_quality
        before = len(iq)
        iq.set("TSS", 33.0, node="J1")
        self.assertEqual(len(iq), before)
        self.assertAlmostEqual(iq[0].value, 33.0)
        # And again, to prove it is not a one-shot.
        iq.set("TSS", 34.0, node="J1")
        self.assertEqual(len(iq), before)
        self.assertAlmostEqual(iq[0].value, 34.0)

    def test_negative_water_age_is_accepted(self):
        """__WATER_AGE__ is signed (hours) — a negative must NOT be refused."""
        s = self.opened_solver()
        iq = s.initial_quality
        iq.set(InitialQuality.WATER_AGE, -4.0, node="J0")
        self.assertAlmostEqual(iq[1].value, -4.0)
        iq.set(InitialQuality.WATER_AGE, -0.5, link="C1")
        self.assertAlmostEqual(iq[-1].value, -0.5)

    def test_negative_temperature_is_accepted(self):
        """__TEMPERATURE__ is degC — sub-zero is a real temperature."""
        s = self.opened_solver()
        iq = s.initial_quality
        iq.set(InitialQuality.TEMPERATURE, -3.5, link="C2")
        self.assertAlmostEqual(iq[2].value, -3.5)
        iq.set(InitialQuality.TEMPERATURE, -12.0, node="J2")
        self.assertAlmostEqual(iq[-1].value, -12.0)


# ---------------------------------------------------------------------------
# Refusals
# ---------------------------------------------------------------------------
class TestInitialQualityRefusals(_InitialQualityCase):

    def test_negative_pollutant_value_refused(self):
        """A concentration may not be negative — refused, and nothing stored."""
        s = self.opened_solver()
        iq = s.initial_quality
        before = len(iq)
        with self.assertRaises(BadParamError):
            iq.set("TSS", -1.0, node="J2")
        self.assertEqual(len(iq), before)

    def test_negative_pollutant_upsert_leaves_value_unchanged(self):
        s = self.opened_solver()
        iq = s.initial_quality
        with self.assertRaises(BadParamError):
            iq.set("TSS", -0.1, node="J1")
        # Refused, not clamped: the existing row keeps the deck's value.
        self.assertAlmostEqual(iq[0].value, 12.5)

    def test_unknown_constituent_refused(self):
        s = self.opened_solver()
        iq = s.initial_quality
        before = len(iq)
        with self.assertRaises(BadParamError):
            iq.set("NOT_A_POLLUTANT", 1.0, node="J1")
        self.assertEqual(len(iq), before)

    def test_unknown_element_raises_keyerror(self):
        s = self.opened_solver()
        with self.assertRaises(KeyError):
            s.initial_quality.set("TSS", 1.0, node="NOPE")
        with self.assertRaises(KeyError):
            s.initial_quality.set("TSS", 1.0, link="NOPE")

    def test_neither_or_both_scopes_is_a_value_error(self):
        s = self.opened_solver()
        iq = s.initial_quality
        with self.assertRaises(ValueError):
            iq.set("TSS", 1.0)
        with self.assertRaises(ValueError):
            iq.set("TSS", 1.0, node="J1", link="C1")

    def test_remove_out_of_range_refused(self):
        s = self.opened_solver()
        with self.assertRaises(BadIndexError):
            s.initial_quality.remove(99)


# ---------------------------------------------------------------------------
# remove()
# ---------------------------------------------------------------------------
class TestInitialQualityRemove(_InitialQualityCase):

    def test_remove_shifts_later_rows_down(self):
        s = self.opened_solver()
        iq = s.initial_quality
        before = list(iq)
        self.assertEqual(len(before), 3)
        iq.remove(0)
        after = list(iq)
        self.assertEqual(len(after), 2)
        self.assertEqual(after, before[1:])

    def test_remove_all_rows(self):
        s = self.opened_solver()
        iq = s.initial_quality
        while len(iq):
            iq.remove(0)
        self.assertEqual(len(iq), 0)
        self.assertEqual(list(iq), [])


if __name__ == "__main__":
    unittest.main()
