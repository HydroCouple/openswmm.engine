"""Tests for :class:`openswmm.engine.OutputReader` binary output file reader."""

import os
import numpy as np
import pytest

from openswmm.engine import Solver, OutputReader, EngineState


@pytest.fixture
def output_file(solver_files, tmp_path):
    """Run a complete simulation and return the .out file path."""
    inp, rpt, out = solver_files
    s = Solver(inp, rpt, out)
    s.open()
    s.initialize()
    s.start(save_results=True)
    while s.state == EngineState.RUNNING:
        if s.step() != 0:
            break
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


class TestOutputReaderNodeStatistics:
    """Post-run node statistics aggregated from the .out file.

    These four accessors (``get_node_stat_max_depth`` /
    ``get_node_stat_max_overflow`` / ``get_node_stat_vol_flooded`` /
    ``get_node_stat_time_flooded``) were added to the C API after the
    initial binding sweep and surfaced through the drift test in
    ``python/tests/test_api_coverage.py``.

    Tests assert: (1) the methods exist; (2) they return ``float``;
    (3) they return non-negative values (these are cumulative
    quantities — a negative value would indicate a wrong-column read);
    (4) the per-period series and the aggregate are mutually consistent
    (peak depth aggregate must equal the max of the
    SWMM_OUT_NODE_DEPTH=0 series for the same node).
    """

    def test_methods_exist(self, output_file):
        with OutputReader(output_file) as r:
            assert hasattr(r, "get_node_stat_max_depth")
            assert hasattr(r, "get_node_stat_max_overflow")
            assert hasattr(r, "get_node_stat_vol_flooded")
            assert hasattr(r, "get_node_stat_time_flooded")

    def test_max_depth_returns_float(self, output_file):
        with OutputReader(output_file) as r:
            v = r.get_node_stat_max_depth(0)
            assert isinstance(v, float)
            assert v >= 0.0

    def test_max_overflow_returns_float(self, output_file):
        with OutputReader(output_file) as r:
            v = r.get_node_stat_max_overflow(0)
            assert isinstance(v, float)
            assert v >= 0.0

    def test_vol_flooded_returns_float(self, output_file):
        with OutputReader(output_file) as r:
            v = r.get_node_stat_vol_flooded(0)
            assert isinstance(v, float)
            assert v >= 0.0

    def test_time_flooded_returns_float(self, output_file):
        with OutputReader(output_file) as r:
            v = r.get_node_stat_time_flooded(0)
            assert isinstance(v, float)
            assert v >= 0.0

    def test_max_depth_matches_series_aggregate(self, output_file):
        """Sanity: max(depth_series) == get_node_stat_max_depth.

        Catches the regression where the aggregator reads the wrong
        SWMM_Out_NodeAttribute column or accidentally walks links."""
        with OutputReader(output_file) as r:
            n_periods = r.get_period_count()
            assert n_periods > 0
            # SWMM_OUT_NODE_DEPTH = 0 (see openswmm_output.h).
            series = r.get_node_series(0, 0, 0, n_periods - 1)
            agg = r.get_node_stat_max_depth(0)
            # series is float32, agg is float64; compare with tolerance.
            assert agg == pytest.approx(float(series.max()), rel=1e-5,
                                          abs=1e-9)

    def test_stats_consistent_across_all_nodes(self, output_file):
        """All nodes should yield non-negative stats; a wrong-column
        read or off-by-one would produce a negative somewhere."""
        with OutputReader(output_file) as r:
            n = r.get_node_count()
            for i in range(n):
                assert r.get_node_stat_max_depth(i) >= 0.0
                assert r.get_node_stat_max_overflow(i) >= 0.0
                assert r.get_node_stat_vol_flooded(i) >= 0.0
                assert r.get_node_stat_time_flooded(i) >= 0.0

    def test_time_flooded_units_are_seconds(self, output_file):
        """Per the header doc the unit is seconds (not hours).  Bound
        as a sanity-check: total flooded seconds must be <= simulation
        duration."""
        with OutputReader(output_file) as r:
            n_periods = r.get_period_count()
            report_step = r.get_report_step()
            sim_duration_sec = n_periods * report_step
            n = r.get_node_count()
            for i in range(n):
                t = r.get_node_stat_time_flooded(i)
                assert 0.0 <= t <= sim_duration_sec + 1e-6, (
                    f"node {i}: flooded {t}s > sim duration "
                    f"{sim_duration_sec}s")
