"""Smoke tests for solver.controls and solver.inflows counts."""

import pytest


class TestControlsBasic:
    def test_count_initial(self, running_solver):
        assert isinstance(len(running_solver.controls), int)
        assert len(running_solver.controls) >= 0


class TestInflowsCounts:
    def test_ext_inflow_count(self, running_solver):
        v = running_solver.inflows.external_count
        assert isinstance(v, int) and v >= 0

    def test_dwf_count(self, running_solver):
        v = running_solver.inflows.dwf_count
        assert isinstance(v, int) and v >= 0

    def test_rdii_count(self, running_solver):
        v = running_solver.inflows.rdii_count
        assert isinstance(v, int) and v >= 0
