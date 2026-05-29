"""
Pytest fixtures for the Python regression test suite.

This is a placeholder suite — it currently exercises the same Example1.inp
that the C++ regression suite (tests/regression/) consumes, but only verifies
that the legacy CLI can run it end-to-end. Real per-timestep parity checks
between the new and legacy engines live in tests/regression/test_regression_suite.cpp;
the Python tests are kept here as a separate layer for higher-level scenarios
(workflow scripts, multi-run sweeps, etc.) and will grow over time.
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
CPP_REGRESSION_DATA = REPO_ROOT / "tests" / "regression" / "data"


@pytest.fixture(scope="session")
def repo_root() -> Path:
    return REPO_ROOT


@pytest.fixture(scope="session")
def example1_inp() -> Path:
    inp = CPP_REGRESSION_DATA / "Example1.inp"
    if not inp.is_file():
        pytest.skip(f"Reference model not found: {inp}")
    return inp


@pytest.fixture(scope="session")
def build_dir() -> Path:
    env = os.environ.get("OPENSWMM_BUILD_DIR")
    if env:
        p = Path(env)
        if p.is_dir():
            return p
        pytest.skip(f"OPENSWMM_BUILD_DIR points to a non-existent dir: {p}")

    candidates = sorted(REPO_ROOT.glob("build-*"))
    for c in candidates:
        if c.is_dir():
            return c
    pytest.skip(
        "Could not locate a build directory. Set OPENSWMM_BUILD_DIR or run "
        "from a checkout that has been configured with cmake."
    )


@pytest.fixture(scope="session")
def legacy_cli(build_dir: Path) -> Path:
    exe_name = "openswmm-legacy.exe" if sys.platform == "win32" else "openswmm-legacy"

    candidates = [
        build_dir / "bin" / "Debug" / exe_name,
        build_dir / "bin" / "Release" / exe_name,
        build_dir / "bin" / exe_name,
        build_dir / "src" / "legacy" / "cli" / "Debug" / exe_name,
        build_dir / "src" / "legacy" / "cli" / "Release" / exe_name,
        build_dir / "src" / "legacy" / "cli" / exe_name,
    ]
    for c in candidates:
        if c.is_file():
            return c

    found = shutil.which(exe_name, path=os.pathsep.join(str(p.parent) for p in candidates))
    if found:
        return Path(found)

    pytest.skip(f"Could not locate {exe_name} under {build_dir}")


@pytest.fixture(scope="session")
def results_dir() -> Path:
    """Where test artifacts land. The regression_testing workflow uploads
    everything under this directory."""
    d = Path(__file__).resolve().parent / "results"
    d.mkdir(parents=True, exist_ok=True)
    return d
