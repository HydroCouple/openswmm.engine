"""P0.1 — Solver.flow_units / Solver.unit_system unit-context accessors.

Because the OpenSWMM C API returns every quantity in the units declared in
the ``.inp`` file (project units), consumers need a first-class way to
discover those units. These tests verify that ``Solver.flow_units`` resolves
the ``[OPTIONS] FLOW_UNITS`` setting to a :class:`FlowUnits` enum member and
that ``Solver.unit_system`` derives ``'US'`` / ``'SI'`` consistently — for
both a US-customary (CFS) model and a metric (CMS) model.

All assertions run against the real handle-based ``Solver`` over the shared
``site_drainage_example.inp`` fixture (no engine mocks).
"""

from __future__ import annotations

import pytest

pytest.importorskip("openswmm.engine._solver")

from openswmm.engine import FlowUnits, Solver


def _write_inp_with_flow_units(src_inp: str, dst_dir, token: str) -> str:
    """Copy C{src_inp} replacing the FLOW_UNITS value with C{token}.

    @param src_inp: Path to the source ``.inp`` file.
    @param dst_dir: ``tmp_path`` (a ``pathlib.Path``) for the rewritten copy.
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
    dst = dst_dir / f"flow_units_{token.lower()}.inp"
    dst.write_text("".join(out_lines), encoding="utf-8")
    return str(dst)


class TestFlowUnitsDefault:
    """The shipped example model is CFS / US."""

    def test_flow_units_is_enum(self, opened_solver):
        assert opened_solver.flow_units is FlowUnits.CFS

    def test_unit_system_is_us(self, opened_solver):
        assert opened_solver.unit_system == "US"

    def test_agrees_with_options(self, opened_solver):
        # The enum accessor must agree with the raw option string.
        token = opened_solver.options["FLOW_UNITS"].strip().upper()
        assert opened_solver.flow_units is FlowUnits[token]


class TestFlowUnitsMetric:
    """A CMS variant must resolve to SI."""

    def test_cms_model_is_si(self, solver_files, tmp_path):
        inp, _rpt, _out = solver_files
        cms_inp = _write_inp_with_flow_units(inp, tmp_path, "CMS")
        s = Solver(cms_inp, str(tmp_path / "cms.rpt"), str(tmp_path / "cms.out"))
        s.open()
        try:
            assert s.flow_units is FlowUnits.CMS
            assert s.unit_system == "SI"
        finally:
            try:
                s.close()
            except Exception:
                pass
            s.destroy()


class TestUnitSystemPartition:
    """US-vs-SI split is exactly the CFS/GPM/MGD | CMS/LPS/MLD boundary."""

    @pytest.mark.parametrize(
        "token,expected",
        [
            ("CFS", "US"),
            ("GPM", "US"),
            ("MGD", "US"),
            ("CMS", "SI"),
            ("LPS", "SI"),
            ("MLD", "SI"),
        ],
    )
    def test_partition(self, solver_files, tmp_path, token, expected):
        inp, _rpt, _out = solver_files
        variant = _write_inp_with_flow_units(inp, tmp_path, token)
        s = Solver(variant, str(tmp_path / f"{token}.rpt"), str(tmp_path / f"{token}.out"))
        s.open()
        try:
            assert s.flow_units is FlowUnits[token]
            assert s.unit_system == expected
        finally:
            try:
                s.close()
            except Exception:
                pass
            s.destroy()
