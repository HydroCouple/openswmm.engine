"""Shared path constants and helpers for the OpenSWMM unittest suite.

Replaces the path fixtures formerly defined in ``tests/conftest.py``.
Test outputs are written to a reviewable ``tests/_artifacts/`` tree
(instead of pytest's hidden ``tmp_path``) so a user can inspect the
.rpt/.out files produced by any test after a run.
"""

import os

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(TESTS_DIR, "data")
SOLVER_DATA_DIR = os.path.join(DATA_DIR, "solver")
OUTPUT_DATA_DIR = os.path.join(DATA_DIR, "output")

SITE_DRAINAGE_INP = os.path.join(SOLVER_DATA_DIR, "site_drainage_example.inp")
SITE_DRAINAGE_RPT = SITE_DRAINAGE_INP.replace(".inp", ".rpt")
SITE_DRAINAGE_OUT = SITE_DRAINAGE_INP.replace(".inp", ".out")

NON_EXISTENT_INP = os.path.join(SOLVER_DATA_DIR, "non_existent_input_file.inp")

EXAMPLE_OUTPUT_FILE = os.path.join(OUTPUT_DATA_DIR, "example_output_1.out")
NON_EXISTENT_OUTPUT = os.path.join(OUTPUT_DATA_DIR, "non_existent_output_file.out")
JSON_TIMESERIES_PICKLE = os.path.join(OUTPUT_DATA_DIR, "json_time_series.pickle")

ARTIFACTS_DIR = os.path.join(TESTS_DIR, "_artifacts")


def artifact_dir(test_case):
    """Per-test reviewable output directory (replacement for ``tmp_path``).

    Uses the unittest test id (``module.Class.method``) so parallel tests
    never collide and each test's files are easy to locate afterwards.
    """
    parts = test_case.id().split(".")
    d = os.path.join(ARTIFACTS_DIR, *parts[-3:])
    os.makedirs(d, exist_ok=True)
    return d
