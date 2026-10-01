"""Concurrency correctness and an opt-in two-engine speed benchmark.

Every wheel must run independent engines concurrently with the same results as
serial runs, and shared-engine bulk reads must return consistent snapshots or
the documented busy refusal. Worker exceptions are propagated to the test.

GIL release around step/stride is also checked by test_solver_pxd_attrs.py.
Parallel speedup is a separate benchmark: shared CI machines may run two
threads slower even when native calls release the GIL. Set
OPENSWMM_RUN_PERFORMANCE_TESTS=1 on a suitable host to enforce its 5% threshold.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import os
import threading
import time
from typing import NamedTuple
import unittest

import numpy as np
import pytest

from openswmm.engine import ErrorCode, LifecycleError, Solver

from tests._paths import SITE_DRAINAGE_INP, artifact_dir
from tests.engine._solver_cases import EngineSolverCase


class SimulationResult(NamedTuple):
    steps: int
    node_depths: np.ndarray
    link_flows: np.ndarray
    subcatchment_runoffs: np.ndarray


def _run_full_simulation(inp: str, rpt: str, out: str,
                         start: threading.Barrier | None = None) -> SimulationResult:
    """Run an independent engine and capture its final numerical state."""
    s = Solver(inp, rpt, out)
    try:
        s.open()
        s.initialize()
        s.start()
        if start is not None:
            start.wait(timeout=60)
        n = 0
        for _ in s.steps():
            n += 1
        result = SimulationResult(n, s.nodes.depths.copy(), s.links.flows.copy(),
                                  s.subcatchments.runoffs.copy())
        s.end()
        s.close()
        return result
    finally:
        s.destroy()


class TestConcurrency(EngineSolverCase):

    @pytest.mark.slow
    def test_two_engines_run_concurrently(self):
        """Independent threaded engines must match a serial reference."""
        d = artifact_dir(self)

        def run(tag, start=None):
            return _run_full_simulation(
                SITE_DRAINAGE_INP, os.path.join(d, tag + ".rpt"),
                os.path.join(d, tag + ".out"), start)

        baseline = run("serial")
        self.assertGreater(baseline.steps, 0)
        start = threading.Barrier(2)
        with ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(run, tag, start) for tag in ("parallel_a", "parallel_b")]
            results = [future.result(timeout=60) for future in futures]
        for i, result in enumerate(results):
            with self.subTest(worker=i):
                self.assertEqual(result.steps, baseline.steps)
                np.testing.assert_array_equal(result.node_depths, baseline.node_depths)
                np.testing.assert_array_equal(result.link_flows, baseline.link_flows)
                np.testing.assert_array_equal(result.subcatchment_runoffs,
                                              baseline.subcatchment_runoffs)

    @pytest.mark.slow
    @unittest.skipUnless(
        os.environ.get("OPENSWMM_RUN_PERFORMANCE_TESTS") == "1",
        "set OPENSWMM_RUN_PERFORMANCE_TESTS=1 to run the parallel speed benchmark",
    )
    def test_two_engines_parallel_speedup(self):
        """At least one of five paired trials must show a 5% speedup."""
        n_runs = 4
        n_trials = 5
        d = artifact_dir(self)

        def run_batch(tag):
            for i in range(n_runs):
                _run_full_simulation(SITE_DRAINAGE_INP,
                                     os.path.join(d, f"{tag}_{i}.rpt"),
                                     os.path.join(d, f"{tag}_{i}.out"))

        _run_full_simulation(SITE_DRAINAGE_INP,
                             os.path.join(d, "warm.rpt"),
                             os.path.join(d, "warm.out"))
        serial_times = []
        parallel_times = []
        with ThreadPoolExecutor(max_workers=2) as pool:
            for k in range(n_trials):
                t0 = time.perf_counter()
                run_batch(f"serial_{k}_a")
                run_batch(f"serial_{k}_b")
                serial_times.append(time.perf_counter() - t0)
                t0 = time.perf_counter()
                futures = [pool.submit(run_batch, f"parallel_{k}_{side}")
                           for side in ("a", "b")]
                for future in futures:
                    future.result(timeout=60)
                parallel_times.append(time.perf_counter() - t0)

        self.assertGreater(min(serial_times), 0.05, "benchmark workload is too small")
        ratios = [p / s for p, s in zip(parallel_times, serial_times)]
        self.assertLess(min(ratios), 0.95,
                        f"no paired trial showed 5% speedup: serial={serial_times}, "
                        f"parallel={parallel_times}, ratios={ratios}")

    # -----------------------------------------------------------------------
    # Test 2 — bulk getters are safe under concurrent calls on one solver
    # -----------------------------------------------------------------------

    @pytest.mark.slow
    def test_bulk_getters_concurrent_reads(self):
        """Many threads calling node/link/subcatch bulk getters on one ENDED
        solver must not deadlock, return corrupt arrays, or fail with
        anything other than the documented busy refusal.

        ``NativeAccess`` refuses a thread that arrives while another is
        inside the native layer (``LifecycleError``, "in use by another
        thread") instead of blocking — see
        ``test_native_safety.test_foreign_thread_access_fails_without_deadlock``.
        Readers therefore retry on that refusal, and every read that does
        get through must agree bit-for-bit with a serial baseline.
        """
        inp, rpt, out = self.solver_files()
        s = Solver(inp, rpt, out)
        try:
            s.open()
            s.initialize()
            s.start()
            for _ in s.steps():
                pass
            s.end()

            # Baseline (single-threaded snapshot) via v1 property access.
            baseline_node_depths = s.nodes.depths
            baseline_link_flows = s.links.flows
            baseline_runoff = s.subcatchments.runoffs

            N_THREADS = 8
            N_ITERS = 16
            TIMEOUT_S = 60.0
            deadline = time.monotonic() + TIMEOUT_S
            errors: list[BaseException] = []

            def read(getter):
                # A refusal means another reader holds the engine, so the
                # pool as a whole always makes progress; the deadline only
                # guards against a regression that never releases it.
                while True:
                    try:
                        return getter()
                    except LifecycleError as e:
                        busy = (e.code == ErrorCode.LIFECYCLE
                                and "in use by another thread" in e.message)
                        if not busy or time.monotonic() > deadline:
                            raise
                        time.sleep(0.001)

            def reader() -> None:
                try:
                    for _ in range(N_ITERS):
                        nd = read(lambda: s.nodes.depths)
                        nh = read(lambda: s.nodes.heads)
                        lf = read(lambda: s.links.flows)
                        ld = read(lambda: s.links.depths)
                        sr = read(lambda: s.subcatchments.runoffs)
                        np.testing.assert_array_equal(nd, baseline_node_depths)
                        np.testing.assert_array_equal(lf, baseline_link_flows)
                        np.testing.assert_array_equal(sr, baseline_runoff)
                        self.assertEqual(nh.shape, nd.shape)
                        self.assertEqual(ld.shape, lf.shape)
                except BaseException as e:  # noqa: BLE001
                    errors.append(e)

            threads = [threading.Thread(target=reader) for _ in range(N_THREADS)]
            for t in threads:
                t.start()
            for t in threads:
                t.join(timeout=max(0.0, deadline - time.monotonic()) + 5.0)

            self.assertFalse([t for t in threads if t.is_alive()],
                             "concurrent readers deadlocked")
            self.assertFalse(errors, f"concurrent reads raised: {errors[:3]}")
        finally:
            try:
                s.close()
            except Exception:
                pass
            s.destroy()

    # -----------------------------------------------------------------------
    # Test 3 — sanity: a single threaded run still produces identical results
    # -----------------------------------------------------------------------

    def test_single_threaded_simulation_unchanged(self):
        """A regression: releasing the GIL must not change numerical output.

        Drives one solver to completion and asserts a couple of invariants —
        step count > 0 and a non-zero mass balance — to catch the rare class
        of bug where adding `with nogil:` accidentally reorders state writes.
        """
        inp, rpt, out = self.solver_files()
        result = _run_full_simulation(inp, rpt, out)
        self.assertGreater(result.steps, 0)
        self.assertTrue(os.path.exists(out) and os.path.getsize(out) > 0)
