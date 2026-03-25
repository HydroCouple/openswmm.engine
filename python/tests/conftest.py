"""Shared pytest fixtures for the OpenSWMM test suite."""

import os
import pytest

# ---------------------------------------------------------------------------
# Paths to test data bundled with the repository
# ---------------------------------------------------------------------------
DATA_DIR = os.path.join(os.path.dirname(__file__), "data")
SOLVER_DATA_DIR = os.path.join(DATA_DIR, "solver")
OUTPUT_DATA_DIR = os.path.join(DATA_DIR, "output")

SITE_DRAINAGE_INP = os.path.join(SOLVER_DATA_DIR, "site_drainage_example.inp")
SITE_DRAINAGE_RPT = SITE_DRAINAGE_INP.replace(".inp", ".rpt")
SITE_DRAINAGE_OUT = SITE_DRAINAGE_INP.replace(".inp", ".out")

NON_EXISTENT_INP = os.path.join(SOLVER_DATA_DIR, "non_existent_input_file.inp")

EXAMPLE_OUTPUT_FILE = os.path.join(OUTPUT_DATA_DIR, "example_output_1.out")
NON_EXISTENT_OUTPUT = os.path.join(OUTPUT_DATA_DIR, "non_existent_output_file.out")
JSON_TIMESERIES_PICKLE = os.path.join(OUTPUT_DATA_DIR, "json_time_series.pickle")


@pytest.fixture
def site_drainage_inp():
    """Path to the site-drainage example .inp file."""
    return SITE_DRAINAGE_INP


@pytest.fixture
def site_drainage_files(tmp_path):
    """Return (inp, rpt, out) paths using a temporary directory for outputs.

    The .rpt and .out files are placed in *tmp_path* so parallel test runs
    don't collide and cleanup is automatic.
    """
    rpt = str(tmp_path / "site_drainage_example.rpt")
    out = str(tmp_path / "site_drainage_example.out")
    return SITE_DRAINAGE_INP, rpt, out


@pytest.fixture
def non_existent_inp():
    """Path to a non-existent .inp file (for error-handling tests)."""
    return NON_EXISTENT_INP


@pytest.fixture
def example_output_file():
    """Path to the bundled example .out file."""
    return EXAMPLE_OUTPUT_FILE


@pytest.fixture
def non_existent_output():
    """Path to a non-existent .out file."""
    return NON_EXISTENT_OUTPUT


@pytest.fixture
def timeseries_pickle():
    """Path to the pickled time-series comparison data."""
    return JSON_TIMESERIES_PICKLE
