"""Tests for :class:`openswmm.engine.Controls` and :class:`openswmm.engine.Inflows`."""

import pytest


class TestControlsBasic:
    """Control rule management."""

    def test_count_initial(self, running_solver):
        from openswmm.engine import Controls
        c = Controls(running_solver)
        n = c.count()
        assert isinstance(n, int)
        assert n >= 0


class TestInflowsCounts:
    """Inflow count queries."""

    def test_ext_inflow_count(self, running_solver):
        from openswmm.engine import Inflows
        inf = Inflows(running_solver)
        n = inf.ext_inflow_count()
        assert isinstance(n, int)
        assert n >= 0

    def test_dwf_count(self, running_solver):
        from openswmm.engine import Inflows
        inf = Inflows(running_solver)
        n = inf.dwf_count()
        assert isinstance(n, int)
        assert n >= 0

    def test_rdii_count(self, running_solver):
        from openswmm.engine import Inflows
        inf = Inflows(running_solver)
        n = inf.rdii_count()
        assert isinstance(n, int)
        assert n >= 0
