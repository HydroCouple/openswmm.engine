"""P0.1 — Solver.flow_units / Solver.unit_system unit-context accessors.

Because the OpenSWMM C API returns every quantity in the units declared in
the ``.inp`` file (project units), consumers need a first-class way to
discover those units. These tests verify that ``Solver.flow_units`` resolves
the ``[OPTIONS] FLOW_UNITS`` setting to a :class:`FlowUnits` enum member and
that ``Solver.unit_system`` derives ``'US'`` / ``'SI'`` consistently — for
both a US-customary (CFS) model and a metric (CMS) model.

All assertions run against the real handle-based ``Solver`` over the shared
``site_drainage_example.inp`` model (no engine mocks).
"""

from __future__ import annotations

import os
import unittest

try:
    import openswmm.engine._solver  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import FlowUnits, Solver

from tests._paths import artifact_dir
from tests.engine._solver_cases import EngineSolverCase


def _write_inp_with_flow_units(src_inp: str, dst_dir, token: str) -> str:
    """Copy C{src_inp} replacing the FLOW_UNITS value with C{token}.

    @param src_inp: Path to the source ``.inp`` file.
    @param dst_dir: Artifact directory (a ``str``) for the rewritten copy.
    @param token: New FLOW_UNITS token, e.g. ``"CMS"``.
    @return: Path to the rewritten ``.inp`` file.
    @rtype: str
    """
    out_lines = []
    for line in open(src_inp, encoding="utf-8"):
        if line.strip().upper().startswith("FLOW_UNITS"):
            out_lines.append(f"FLOW_UNITS           {token}\n")
        else:
            out_lines.append(line)
    dst = os.path.join(dst_dir, f"flow_units_{token.lower()}.inp")
    with open(dst, "w", encoding="utf-8") as f:
        f.write("".join(out_lines))
    return dst


class TestFlowUnitsDefault(EngineSolverCase):
    """The shipped example model is CFS / US."""

    def test_flow_units_is_enum(self):
        opened_solver = self.opened_solver()
        self.assertIs(opened_solver.flow_units, FlowUnits.CFS)

    def test_unit_system_is_us(self):
        opened_solver = self.opened_solver()
        self.assertEqual(opened_solver.unit_system, "US")

    def test_agrees_with_options(self):
        opened_solver = self.opened_solver()
        # The enum accessor must agree with the raw option string.
        token = opened_solver.options["FLOW_UNITS"].strip().upper()
        self.assertIs(opened_solver.flow_units, FlowUnits[token])


class TestFlowUnitsMetric(EngineSolverCase):
    """A CMS variant must resolve to SI."""

    def test_cms_model_is_si(self):
        inp, _rpt, _out = self.solver_files()
        d = artifact_dir(self)
        cms_inp = _write_inp_with_flow_units(inp, d, "CMS")
        s = Solver(cms_inp, os.path.join(d, "cms.rpt"), os.path.join(d, "cms.out"))
        s.open()
        try:
            self.assertIs(s.flow_units, FlowUnits.CMS)
            self.assertEqual(s.unit_system, "SI")
        finally:
            try:
                s.close()
            except Exception:
                pass
            s.destroy()


class TestUnitSystemPartition(EngineSolverCase):
    """US-vs-SI split is exactly the CFS/GPM/MGD | CMS/LPS/MLD boundary."""

    def test_partition(self):
        for token, expected in [
            ("CFS", "US"),
            ("GPM", "US"),
            ("MGD", "US"),
            ("CMS", "SI"),
            ("LPS", "SI"),
            ("MLD", "SI"),
        ]:
            with self.subTest(token=token, expected=expected):
                inp, _rpt, _out = self.solver_files()
                d = artifact_dir(self)
                variant = _write_inp_with_flow_units(inp, d, token)
                s = Solver(
                    variant,
                    os.path.join(d, f"{token}.rpt"),
                    os.path.join(d, f"{token}.out"),
                )
                s.open()
                try:
                    self.assertIs(s.flow_units, FlowUnits[token])
                    self.assertEqual(s.unit_system, expected)
                finally:
                    try:
                        s.close()
                    except Exception:
                        pass
                    s.destroy()
