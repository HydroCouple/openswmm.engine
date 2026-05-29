"""Deployment smoke test for the openswmm wheel.

Run from the repo root (or any directory — the installed wheel's data is
located via importlib.metadata, with a dev-tree fallback):

    python python/smoke_test.py

Checks:
  1. All primary engine classes importable
  2. Enum values accessible
  3. ModelBuilder — programmatic build round-trip (add + pop_last)
  4. Solver — open / initialize / run / read results lifecycle
  5. OPENED-state editing (Nodes.add, Links.add, Subcatchments.add, Gages.add,
     Nodes.pop_last, Links.pop_last)
  6. Inflows in OPENED state
  7. Wheel metadata (version string)

Portability notes
-----------------
* Uses pathlib.Path throughout to avoid Windows vs. POSIX path separator issues.
* Uses explicit shutil.rmtree(ignore_errors=True) cleanup instead of
  tempfile.TemporaryDirectory context managers: on Windows, the SWMM engine
  may hold a file handle briefly after close(), causing TemporaryDirectory to
  raise PermissionError during __exit__ even on Python 3.12+.
"""

import importlib.metadata
import shutil
import tempfile
from pathlib import Path

# ---------------------------------------------------------------------------
# 1. Imports
# ---------------------------------------------------------------------------

print("[ 1 ] Importing engine classes...", end=" ")
from openswmm.engine import (
    Controls,
    EngineError,
    EngineState,
    Forcing,
    Gages,
    HotStart,
    Inflows,
    Infrastructure,
    Links,
    LinkType,
    MassBalance,
    ModelBuilder,
    NodeType,
    Nodes,
    OutputReader,
    Pollutants,
    Quality,
    RouteModel,
    Solver,
    Spatial,
    Statistics,
    Subcatchments,
    Tables,
    run,
)
print("OK")

# ---------------------------------------------------------------------------
# 2. Enums
# ---------------------------------------------------------------------------

print("[ 2 ] Checking enum values...", end=" ")
assert int(NodeType.JUNCTION) == 0
assert int(NodeType.OUTFALL)  == 1
assert int(NodeType.STORAGE)  == 2
assert int(NodeType.DIVIDER)  == 3
assert int(LinkType.CONDUIT)  == 0
assert int(LinkType.PUMP)     == 1
assert int(RouteModel.DYNWAVE) >= 0
print("OK")

# ---------------------------------------------------------------------------
# 3. ModelBuilder round-trip (BUILDING state)
# ---------------------------------------------------------------------------

print("[ 3 ] ModelBuilder add / pop_last...", end=" ")
m = ModelBuilder()

assert m.add_node("J1",   int(NodeType.JUNCTION)) == 0
assert m.add_node("J2",   int(NodeType.JUNCTION)) == 0
assert m.add_node("OUT1", int(NodeType.OUTFALL))  == 0
assert m.add_link("C1",   int(LinkType.CONDUIT))  == 0

rc = m.pop_last_link("C1")
assert rc == 0, f"pop_last_link failed: rc={rc}"

rc_bad = m.pop_last_node("J1")
assert rc_bad != 0, "wrong-tail pop should fail (SWMM_ERR_BADINDEX)"

assert m.pop_last_node("OUT1") == 0
del m
print("OK")

# ---------------------------------------------------------------------------
# Helper: locate the test .inp regardless of install method or OS
# ---------------------------------------------------------------------------

def _find_inp() -> Path:
    """Locate site_drainage_example.inp.

    Tries (in order):
      1. importlib.metadata — resolves correctly when the package is
         installed from the wheel (works on Windows, macOS, Linux).
      2. Path relative to this script — works in the dev source tree.
    """
    target = "site_drainage_example.inp"
    try:
        dist = importlib.metadata.distribution("openswmm")
        for f in (dist.files or []):
            if Path(str(f)).name == target:
                return dist.locate_file(f).resolve()
    except importlib.metadata.PackageNotFoundError:
        pass
    return Path(__file__).parent / "tests" / "data" / "solver" / target


INP = _find_inp()

# ---------------------------------------------------------------------------
# 4. Full Solver lifecycle (open / init / run / read)
# ---------------------------------------------------------------------------

if not INP.exists():
    print(f"[ 4 ] SKIP — test .inp not found at {INP}")
else:
    print("[ 4 ] Solver lifecycle (open/init/run/read)...", end=" ")
    td = Path(tempfile.mkdtemp())
    try:
        rpt = str(td / "out.rpt")
        out = str(td / "out.out")

        run(str(INP), rpt, out)

        assert Path(rpt).exists(), "report file not written"
        assert Path(out).exists(), "output file not written"

        reader = OutputReader(out)
        assert reader.get_node_count() > 0
        assert reader.get_link_count() > 0
        reader.close()
    finally:
        shutil.rmtree(td, ignore_errors=True)
    print("OK")

# ---------------------------------------------------------------------------
# 5. OPENED-state editing (Nodes / Links / Subcatchments / Gages)
# ---------------------------------------------------------------------------

if not INP.exists():
    print("[ 5 ] SKIP — test .inp not found")
else:
    print("[ 5 ] OPENED-state editing...", end=" ")
    td = Path(tempfile.mkdtemp())
    try:
        solver = Solver(str(INP), str(td / "edit.rpt"), str(td / "edit.out"))
        solver.open()

        nodes = Nodes(solver)
        links = Links(solver)
        subs  = Subcatchments(solver)
        gages = Gages(solver)

        n_before = nodes.count()
        l_before = links.count()
        s_before = subs.count()
        g_before = gages.count()

        assert nodes.add("PY_N1", int(NodeType.JUNCTION)) == 0
        assert nodes.count() == n_before + 1
        assert links.add("PY_L1", int(LinkType.CONDUIT)) == 0
        assert links.count() == l_before + 1
        assert subs.add("PY_SC1") == 0
        assert subs.count() == s_before + 1
        assert gages.add("PY_G1") == 0
        assert gages.count() == g_before + 1

        # pop_last undoes add (link must go before its nodes)
        assert links.pop_last("PY_L1") == 0
        assert links.count() == l_before
        assert nodes.pop_last("PY_N1") == 0
        assert nodes.count() == n_before

        solver.close()
        solver.destroy()
    finally:
        shutil.rmtree(td, ignore_errors=True)
    print("OK")

# ---------------------------------------------------------------------------
# 6. Inflows in OPENED state
# ---------------------------------------------------------------------------

if not INP.exists():
    print("[ 6 ] SKIP — test .inp not found")
else:
    print("[ 6 ] Inflows in OPENED state...", end=" ")
    td = Path(tempfile.mkdtemp())
    try:
        solver = Solver(str(INP), str(td / "i.rpt"), str(td / "i.out"))
        solver.open()
        inflows = Inflows(solver)
        before = inflows.ext_inflow_count()
        inflows.add_external(0, "FLOW", baseline=0.5)
        assert inflows.ext_inflow_count() >= before + 1
        inflows.add_dwf(0, "FLOW", 1.5)
        solver.close()
        solver.destroy()
    finally:
        shutil.rmtree(td, ignore_errors=True)
    print("OK")

# ---------------------------------------------------------------------------
# 7. Wheel metadata
# ---------------------------------------------------------------------------

print("[ 7 ] Checking wheel metadata...", end=" ")
meta = importlib.metadata.metadata("openswmm")
assert meta["Name"] == "openswmm"
version = meta["Version"]
assert version, "empty version string"
print(f"OK  (version={version})")

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

print()
print("=" * 52)
print("  All smoke tests passed.")
print(f"  openswmm {version} — wheel is deployment-ready.")
print("=" * 52)
