"""Tests for the Phase 1b runoff interface Python bindings.

The bindings wrap the new C API entry points (``swmm_runoff_iface_*``).
SAVE-mode is fully integrated with the engine's runoff loop — the engine
emits one record per substep automatically.  USE-mode currently requires
the caller to drive ``read_runoff_step`` themselves; this test exercises
the C API without claiming the engine yet skips its own runoff
computation in USE mode (that integration is tracked as a follow-up).
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import EngineState, Solver

from tests._paths import SITE_DRAINAGE_INP, artifact_dir


def _drive_to_end(solver):
    while solver.state == EngineState.RUNNING:
        rc = solver.step()
        if rc != 0:
            break


# ---------------------------------------------------------------------------
# SAVE round trip
# ---------------------------------------------------------------------------


class TestRunoffInterfaceSaveMode(unittest.TestCase):
    """SAVE mode is the headline path — open the file, run the
    simulation, the engine auto-emits records, close the file."""

    def test_save_mode_writes_a_non_trivial_file(self):
        d = artifact_dir(self)
        rpt = os.path.join(d, "rfi_save.rpt")
        out = os.path.join(d, "rfi_save.out")
        rfi = os.path.join(d, "phase1b.rfi")

        s = Solver(SITE_DRAINAGE_INP, rpt, out)
        try:
            s.open()
            s.initialize()
            s.start()
            s.open_runoff_interface_write(rfi)
            _drive_to_end(s)
            s.end()
            s.close_runoff_interface()
        finally:
            try:
                s.close()
            except Exception:
                pass
            s.destroy()

        # File must exist and be larger than the 28-byte header.
        self.assertTrue(os.path.exists(rfi))
        self.assertGreater(
            os.path.getsize(rfi), 28,
            "Expected at least one substep record beyond the header")

    def test_save_then_read_round_trip(self):
        d = artifact_dir(self)
        rpt = os.path.join(d, "rfi_rt.rpt")
        out = os.path.join(d, "rfi_rt.out")
        rfi = os.path.join(d, "phase1b_rt.rfi")

        # Phase 1: SAVE mode — drive the simulation and write the file.
        s = Solver(SITE_DRAINAGE_INP, rpt, out)
        try:
            s.open()
            s.initialize()
            s.start()
            s.open_runoff_interface_write(rfi)
            _drive_to_end(s)
            s.end()
            s.close_runoff_interface()
        finally:
            try:
                s.close()
            except Exception:
                pass
            s.destroy()

        # Phase 2: USE mode — reopen, count records by polling read_step
        # until it returns False (EOF). The bindings expose the file but
        # do not yet skip the engine's runoff — that's a follow-up. For
        # this test we just verify the read API can drain the file
        # cleanly without errors.
        s2 = Solver(SITE_DRAINAGE_INP, rpt, out)
        records = 0
        try:
            s2.open()
            s2.initialize()
            s2.start()
            s2.open_runoff_interface_read(rfi)
            for _ in range(100_000):  # generous upper bound
                if not s2.read_runoff_step():
                    break
                records += 1
            else:
                self.fail("read_runoff_step appears to loop past EOF")
            s2.close_runoff_interface()
            s2.end()
        finally:
            try:
                s2.close()
            except Exception:
                pass
            s2.destroy()

        self.assertGreater(records, 0)


# ---------------------------------------------------------------------------
# Contract tests — no engine work needed beyond initialize().
# ---------------------------------------------------------------------------


class TestRunoffInterfaceContracts(unittest.TestCase):
    """Lifecycle + idempotency contracts that don't require a full run."""

    def test_close_is_idempotent(self):
        d = artifact_dir(self)
        rpt = os.path.join(d, "rfi_close.rpt")
        out = os.path.join(d, "rfi_close.out")
        s = Solver(SITE_DRAINAGE_INP, rpt, out)
        try:
            s.open()
            s.initialize()
            # Never opened — close should still succeed.
            s.close_runoff_interface()
            s.close_runoff_interface()
        finally:
            try:
                s.close()
            except Exception:
                pass
            s.destroy()

    def test_read_step_returns_false_when_no_file_open(self):
        d = artifact_dir(self)
        rpt = os.path.join(d, "rfi_nofile.rpt")
        out = os.path.join(d, "rfi_nofile.out")
        s = Solver(SITE_DRAINAGE_INP, rpt, out)
        try:
            s.open()
            s.initialize()
            # No file ever opened.
            self.assertIs(s.read_runoff_step(), False)
        finally:
            try:
                s.close()
            except Exception:
                pass
            s.destroy()

    def test_save_step_is_noop_when_no_file_open(self):
        d = artifact_dir(self)
        rpt = os.path.join(d, "rfi_nosave.rpt")
        out = os.path.join(d, "rfi_nosave.out")
        s = Solver(SITE_DRAINAGE_INP, rpt, out)
        try:
            s.open()
            s.initialize()
            # No file open — explicit save must succeed silently.
            s.save_runoff_step(1.0)
        finally:
            try:
                s.close()
            except Exception:
                pass
            s.destroy()
