"""[RESTART]: record restart states into GeoPackage and HDF5 files and start a run from one."""
from datetime import datetime
import os
import unittest

from openswmm.engine import (RestartSettings, Solver, inspect_restart_file,
                             probe_restart_file)

from tests._paths import artifact_dir

# Three-node dynamic-wave network with a time-varying inflow.
_DECK = """[OPTIONS]
FLOW_UNITS CFS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2020
START_TIME {start}
REPORT_START_DATE 01/01/2020
REPORT_START_TIME 00:00:00
END_DATE 01/01/2020
END_TIME {end}
REPORT_STEP 00:01:00
ROUTING_STEP 15

[JUNCTIONS]
J1 10 6 0 0 0
J2 8 6 0 0 0

[OUTFALLS]
O1 5 FREE

[CONDUITS]
C1 J1 J2 400 0.013 0 0 0 0
C2 J2 O1 400 0.013 0 0 0 0

[XSECTIONS]
C1 CIRCULAR 1.5 0 0 0 1
C2 CIRCULAR 1.5 0 0 0 1

[TIMESERIES]
TS1 0:00 0
TS1 0:20 3
TS1 0:40 1
TS1 1:00 0

[INFLOWS]
J1 FLOW TS1
"""


def _deck(directory, name, start="00:00:00", end="01:00:00"):
    path = os.path.join(directory, name + ".inp")
    with open(path, "w") as f:
        f.write(_DECK.format(start=start, end=end))
    return path


def _depths(solver):
    return [solver.nodes[i].depth for i in range(len(solver.nodes))]


class RestartRecordsTest(unittest.TestCase):
    def test_settings_record_and_initial_conditions(self):
        d = artifact_dir(self)
        states = os.path.join(d, "states.h5")
        for leftover in (states,):
            if os.path.exists(leftover):
                os.remove(leftover)

        # Record at 00:30 and at the end, configured from Python.
        solver = Solver(_deck(d, "a"), os.path.join(d, "a.rpt"), os.path.join(d, "a.out"))
        solver.open()
        restart = solver.restart
        self.assertIsInstance(restart, RestartSettings)
        restart.record_file = states
        restart.record_format = "HDF5"
        restart.record_at = ["END", datetime(2020, 1, 1, 0, 30)]
        restart.record_every = 0.0
        self.assertEqual(restart.record_format, "HDF5")
        self.assertEqual(len(restart.record_at), 2)
        self.assertEqual(restart.record_at[0], "END")
        solver.initialize()
        solver.start()
        for _ in solver.steps():
            pass
        solver.end()
        solver.report()
        solver.close()
        solver.destroy()

        self.assertEqual(probe_restart_file(states), "HDF5")
        catalog = inspect_restart_file(states)
        self.assertEqual(catalog["backend"], "HDF5")
        self.assertEqual(len(catalog["runs"]), 1)
        run = catalog["runs"][0]
        self.assertTrue(run["run_id"].startswith("run_"))
        records = run["records"]
        self.assertEqual([r["record_id"] for r in records], ["rec_000001", "rec_000002"])
        self.assertTrue(all(r["status"] == "complete" for r in records))

        # A two-minute target started from the 00:30 record differs from a cold start.
        target = _deck(d, "b", start="00:30:00", end="00:32:00")
        with Solver(target, os.path.join(d, "b.rpt"), os.path.join(d, "b.out")) as cold:
            for _ in cold.steps():
                pass
            cold_depths = _depths(cold)
        warm = Solver(target, os.path.join(d, "c.rpt"), os.path.join(d, "c.out"))
        warm.open()
        warm.restart.source_file = states
        warm.restart.source_run = run["run_id"]
        warm.restart.source_record = "rec_000001"
        warm.restart.mode = "INITIAL_CONDITIONS"
        warm.initialize()
        warm.start()
        for _ in warm.steps():
            pass
        warm_depths = _depths(warm)
        warm.end()
        warm.close()
        warm.destroy()
        self.assertNotEqual(warm_depths, cold_depths)

    def test_inspecting_a_missing_file_raises(self):
        with self.assertRaises(OSError):
            inspect_restart_file(os.path.join(artifact_dir(self), "nowhere.gpkg"))
        self.assertEqual(probe_restart_file(os.path.join(artifact_dir(self), "nowhere.gpkg")),
                         "NONE")


if __name__ == "__main__":
    unittest.main()
