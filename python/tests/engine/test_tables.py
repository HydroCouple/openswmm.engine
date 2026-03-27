"""Tests for :class:`openswmm.engine.Tables` (time series, curves, patterns)."""

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
