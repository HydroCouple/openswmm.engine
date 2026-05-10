"""Pytest fixtures for openswmm.engine tests.

These fixtures provide pre-configured Solver instances at various lifecycle
stages so individual test modules don't duplicate boilerplate.
"""

import os
import pytest

from openswmm.engine import Solver, Nodes, Links, Subcatchments, Gages, MassBalance, EngineState


# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
_DATA_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), "data", "solver")
SITE_DRAINAGE_INP = os.path.join(_DATA_DIR, "site_drainage_example.inp")
NON_EXISTENT_INP = os.path.join(_DATA_DIR, "non_existent_input_file.inp")


# ---------------------------------------------------------------------------
# Solver fixtures
# ---------------------------------------------------------------------------
@pytest.fixture
def solver_files(tmp_path):
    """Return (inp, rpt, out) with temp outputs so tests don't collide."""
    rpt = str(tmp_path / "site_drainage.rpt")
    out = str(tmp_path / "site_drainage.out")
    return SITE_DRAINAGE_INP, rpt, out


@pytest.fixture
def opened_solver(solver_files):
    """A Solver that has been opened but not yet started.

    Yields the solver and cleans up on teardown.
    """
    inp, rpt, out = solver_files
    s = Solver(inp, rpt, out)
    s.open()
    yield s
    try:
        s.close()
    except Exception:
        pass
    s.destroy()


@pytest.fixture
def initialized_solver(solver_files):
    """A Solver that has been opened and initialized (ready for start)."""
    inp, rpt, out = solver_files
    s = Solver(inp, rpt, out)
    s.open()
    s.initialize()
    yield s
    try:
        s.close()
    except Exception:
        pass
    s.destroy()


@pytest.fixture
def running_solver(solver_files):
    """A Solver in STARTED/RUNNING state (ready for step()).

    The full lifecycle is cleaned up on teardown.
    """
    inp, rpt, out = solver_files
    s = Solver(inp, rpt, out)
    s.open()
    s.initialize()
    s.start()
    yield s
    try:
        s.end()
        s.report()
    except Exception:
        pass
    try:
        s.close()
    except Exception:
        pass
    s.destroy()


@pytest.fixture
def stepped_solver(running_solver):
    """A running solver that has been advanced 12 timesteps."""
    for _ in range(12):
        running_solver.step()
    return running_solver


@pytest.fixture
def completed_solver(solver_files):
    """A Solver that has finished the entire simulation (ENDED state)."""
    inp, rpt, out = solver_files
    s = Solver(inp, rpt, out)
    s.open()
    s.initialize()
    s.start()
    while s.state == EngineState.RUNNING:
        if s.step() != 0:
            break
        pass
    s.end()
    yield s
    try:
        s.report()
    except Exception:
        pass
    try:
        s.close()
    except Exception:
        pass
    s.destroy()


# ---------------------------------------------------------------------------
# Domain-object fixtures
# ---------------------------------------------------------------------------
@pytest.fixture
def nodes(running_solver) -> Nodes:
    """A Nodes instance bound to a running solver."""
    return Nodes(running_solver)


@pytest.fixture
def stepped_nodes(stepped_solver) -> Nodes:
    """A Nodes instance bound to a solver that has been stepped 12 times."""
    return Nodes(stepped_solver)


@pytest.fixture
def links(running_solver) -> Links:
    """A Links instance bound to a running solver."""
    return Links(running_solver)


@pytest.fixture
def stepped_links(stepped_solver) -> Links:
    """A Links instance bound to a solver that has been stepped 12 times."""
    return Links(stepped_solver)


@pytest.fixture
def subcatchments(running_solver) -> Subcatchments:
    """A Subcatchments instance bound to a running solver."""
    return Subcatchments(running_solver)


@pytest.fixture
def stepped_subcatchments(stepped_solver) -> Subcatchments:
    """A Subcatchments instance bound to a solver that has been stepped 12 times."""
    return Subcatchments(stepped_solver)


@pytest.fixture
def gages(running_solver) -> Gages:
    """A Gages instance bound to a running solver."""
    return Gages(running_solver)


@pytest.fixture
def stepped_gages(stepped_solver) -> Gages:
    """A Gages instance bound to a solver that has been stepped 12 times."""
    return Gages(stepped_solver)


@pytest.fixture
def mass_balance(completed_solver) -> MassBalance:
    """A MassBalance instance bound to a completed solver."""
    return MassBalance(completed_solver)
