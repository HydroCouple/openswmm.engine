"""Tests for :class:`openswmm.engine.Tables` (time series, curves, patterns)."""

import numpy as np
import pytest


class TestTablesCount:
    """Table count queries."""

    def test_count(self, running_solver):
        from openswmm.engine import Tables
        t = Tables(running_solver)
        n = t.count()
        assert isinstance(n, int)
        assert n >= 0

    def test_pattern_count(self, running_solver):
        from openswmm.engine import Tables
        t = Tables(running_solver)
        n = t.pattern_count()
        assert isinstance(n, int)
        assert n >= 0


class TestTablesLookup:
    """Table data point access."""

    def test_get_id_if_tables_exist(self, running_solver):
        from openswmm.engine import Tables
        t = Tables(running_solver)
        n = t.count()
        if n > 0:
            name = t.get_id(0)
            assert isinstance(name, str)
            assert len(name) > 0

    def test_get_point_count_if_tables_exist(self, running_solver):
        from openswmm.engine import Tables
        t = Tables(running_solver)
        n = t.count()
        if n > 0:
            pc = t.get_point_count(0)
            assert isinstance(pc, int)
            assert pc >= 0


# ---------------------------------------------------------------------------
# Creation methods (new additions)
# ---------------------------------------------------------------------------
class TestTablesAdd:
    """timeseries_add, curve_add, pattern_add, pattern_set_factors.

    These creation methods require BUILDING state (ModelBuilder).
    """

    def test_timeseries_add_returns_index(self):
        from openswmm.engine import ModelBuilder, Tables
        m = ModelBuilder()
        t = Tables(m)
        before = t.count()
        idx = t.timeseries_add("TS_TEST_NEW")
        assert isinstance(idx, int)
        assert idx >= 0
        assert t.count() == before + 1

    def test_curve_add_returns_index(self):
        from openswmm.engine import ModelBuilder, Tables
        m = ModelBuilder()
        t = Tables(m)
        before = t.count()
        idx = t.curve_add("CURVE_TEST_NEW", 1)
        assert isinstance(idx, int)
        assert idx >= 0
        assert t.count() == before + 1

    def test_pattern_add_returns_index(self):
        from openswmm.engine import ModelBuilder, Tables
        m = ModelBuilder()
        t = Tables(m)
        before = t.pattern_count()
        idx = t.pattern_add("PAT_TEST_NEW", 0)
        assert isinstance(idx, int)
        assert idx >= 0
        assert t.pattern_count() == before + 1

    def test_pattern_set_factors(self):
        from openswmm.engine import ModelBuilder, Tables
        m = ModelBuilder()
        t = Tables(m)
        idx = t.pattern_add("PAT_TEST_FACTORS", 0)
        factors = np.ones(12, dtype=np.float64) * 1.5
        t.pattern_set_factors(idx, factors)  # should not raise

