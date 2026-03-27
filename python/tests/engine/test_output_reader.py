"""Tests for :class:`openswmm.engine.OutputReader` binary output file reader."""

import os
import numpy as np
import pytest

from openswmm.engine import Solver, OutputReader


@pytest.fixture
def output_file(solver_files, tmp_path):
    """Run a complete simulation and return the .out file path."""
    inp, rpt, out = solver_files
    s = Solver(inp, rpt, out)
    s.open()
    s.initialize()
    s.start(save_results=True)
    while s.step():
        pass
    s.end()
    s.report()
    s.close()
    s.destroy()
    return out


class TestOutputReaderLifecycle:
    """Open/close and context manager."""

    def test_open_close(self, output_file):
        reader = OutputReader(output_file)
        reader.close()

    def test_context_manager(self, output_file):
        with OutputReader(output_file) as r:
            assert r.get_period_count() > 0

    def test_open_nonexistent_raises(self, tmp_path):
        with pytest.raises(IOError):
            OutputReader(str(tmp_path / "nonexistent.out"))


class TestOutputReaderMetadata:
    """Metadata queries."""

    def test_get_version(self, output_file):
        with OutputReader(output_file) as r:
            v = r.get_version()
            assert isinstance(v, int)
            assert v > 0

    def test_get_flow_units(self, output_file):
        with OutputReader(output_file) as r:
            fu = r.get_flow_units()
            assert isinstance(fu, int)
            assert fu >= 0

    def test_get_counts(self, output_file):
        with OutputReader(output_file) as r:
            assert r.get_subcatch_count() > 0
            assert r.get_node_count() > 0
            assert r.get_link_count() > 0
            assert r.get_period_count() > 0

    def test_get_start_date(self, output_file):
        with OutputReader(output_file) as r:
            d = r.get_start_date()
            assert isinstance(d, float)
            assert d > 0.0

    def test_get_report_step(self, output_file):
        with OutputReader(output_file) as r:
            s = r.get_report_step()
            assert isinstance(s, int)
            assert s > 0

    def test_get_error_code(self, output_file):
        with OutputReader(output_file) as r:
            e = r.get_error_code()
            assert e == 0


class TestOutputReaderObjectIDs:
    """Object ID retrieval."""

    def test_get_node_id(self, output_file):
        with OutputReader(output_file) as r:
            name = r.get_node_id(0)
            assert isinstance(name, str)
            assert len(name) > 0

    def test_get_link_id(self, output_file):
        with OutputReader(output_file) as r:
            name = r.get_link_id(0)
            assert isinstance(name, str)
            assert len(name) > 0

    def test_get_subcatch_id(self, output_file):
        with OutputReader(output_file) as r:
            name = r.get_subcatch_id(0)
            assert isinstance(name, str)
            assert len(name) > 0


class TestOutputReaderResults:
    """Per-period result retrieval."""

    def test_get_node_result_depth(self, output_file):
        with OutputReader(output_file) as r:
            arr = r.get_node_result(0, 0)  # period 0, NODE_DEPTH
            assert isinstance(arr, np.ndarray)
            assert arr.shape == (r.get_node_count(),)

    def test_get_link_result_flow(self, output_file):
        with OutputReader(output_file) as r:
            arr = r.get_link_result(0, 0)  # period 0, LINK_FLOW
            assert isinstance(arr, np.ndarray)
            assert arr.shape == (r.get_link_count(),)

    def test_get_subcatch_result_runoff(self, output_file):
        with OutputReader(output_file) as r:
            arr = r.get_subcatch_result(0, 4)  # period 0, SUBCATCH_RUNOFF
            assert isinstance(arr, np.ndarray)
            assert arr.shape == (r.get_subcatch_count(),)

    def test_get_system_result(self, output_file):
        with OutputReader(output_file) as r:
            v = r.get_system_result(0, 0)  # period 0, SYS_TEMPERATURE
            assert isinstance(v, float)


class TestOutputReaderTimeSeries:
    """Time series retrieval."""

    def test_get_node_series(self, output_file):
        with OutputReader(output_file) as r:
            n_periods = r.get_period_count()
            if n_periods > 1:
                arr = r.get_node_series(0, 0, 0, n_periods - 1)
                assert isinstance(arr, np.ndarray)
                assert arr.shape == (n_periods,)

    def test_get_link_series(self, output_file):
        with OutputReader(output_file) as r:
            n_periods = r.get_period_count()
            if n_periods > 1:
                arr = r.get_link_series(0, 0, 0, n_periods - 1)
                assert arr.shape == (n_periods,)

    def test_get_period_time(self, output_file):
        with OutputReader(output_file) as r:
            t = r.get_period_time(0)
            assert isinstance(t, float)
            assert t > 0.0


class TestOutputReaderAttributes:
    """Per-object attribute retrieval."""

    def test_get_node_attribute(self, output_file):
        with OutputReader(output_file) as r:
            arr = r.get_node_attribute(0, 0)
            assert isinstance(arr, np.ndarray)
            assert len(arr) > 0

    def test_get_link_attribute(self, output_file):
        with OutputReader(output_file) as r:
            arr = r.get_link_attribute(0, 0)
            assert isinstance(arr, np.ndarray)
            assert len(arr) > 0

    def test_get_subcatch_attribute(self, output_file):
        with OutputReader(output_file) as r:
            arr = r.get_subcatch_attribute(0, 0)
            assert isinstance(arr, np.ndarray)
            assert len(arr) > 0
