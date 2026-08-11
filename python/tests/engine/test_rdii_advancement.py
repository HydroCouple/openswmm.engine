"""Tests for the RDII advancement in :class:`openswmm.engine.Inflows`.

Covers the new hydrograph / RDII assignment / RDII decay accessors added
to the C API and Cython bindings. The decay surface (``add_rdii_decay``,
``get_rdii_decay``, ``rdii_decay_count``) is the user-facing entry point
for the exponential temperature-dependent initial-abstraction model.

See ``docs/RDII_ExpDecay_Implementation.md`` for the formulation.
"""

import unittest

from openswmm.engine import ModelBuilder, NodeType


# ---------------------------------------------------------------------------
# Helpers: minimal in-memory model with one node, exposed via ModelBuilder
# ---------------------------------------------------------------------------
def _builder_with_node():
    """A fresh ModelBuilder with one junction so RDII assignment is valid."""
    m = ModelBuilder()
    m.add_node("J1", NodeType.JUNCTION)
    return m


def _inflows():
    """An :class:`Inflows` accessor on a one-node ModelBuilder solver."""
    solver = _builder_with_node().to_solver()
    return solver.inflows


class InflowsCase(unittest.TestCase):
    """Base TestCase providing a fresh ``self.inflows`` per test."""

    def setUp(self):
        self.inflows = _inflows()


# ---------------------------------------------------------------------------
# [HYDROGRAPHS] — parameter lines
# ---------------------------------------------------------------------------
class TestHydrographParameters(InflowsCase):
    """``add_hydrograph`` / ``get_hydrograph`` / ``hydrograph_count``."""

    def test_count_starts_at_zero(self):
        self.assertEqual(self.inflows.hydrograph_count, 0)

    def test_add_one_returns_one(self):
        self.inflows.add_hydrograph("SanSewer", -1, 0, 0.055, 1.0, 2.0)
        self.assertEqual(self.inflows.hydrograph_count, 1)

    def test_round_trip_all_fields(self):
        self.inflows.add_hydrograph(
            "SanSewer", -1, 0,
            r=0.055, t=1.0, k=2.0,
            dmax=8.0, drecov=0.10, dinit=2.0,
        )
        e = self.inflows.get_hydrograph(0)
        self.assertEqual(e.uh_name, "SanSewer")
        self.assertEqual(e.month, -1)
        self.assertEqual(e.response, 0)
        self.assertAlmostEqual(e.r, 0.055, places=6)
        self.assertAlmostEqual(e.t, 1.0, places=6)
        self.assertAlmostEqual(e.k, 2.0, places=6)
        self.assertAlmostEqual(e.dmax, 8.0, places=6)
        self.assertAlmostEqual(e.drecov, 0.10, places=6)
        self.assertAlmostEqual(e.dinit, 2.0, places=6)

    def test_three_responses(self):
        for response, t in [(0, 1.0), (1, 3.5), (2, 14.0)]:
            self.inflows.add_hydrograph("SanSewer", -1, response,
                                        r=0.05, t=t, k=2.0)
        self.assertEqual(self.inflows.hydrograph_count, 3)
        self.assertEqual(self.inflows.get_hydrograph(1).response, 1)
        self.assertAlmostEqual(self.inflows.get_hydrograph(2).t, 14.0, places=6)

    def test_monthly_entries(self):
        # 12 monthly entries plus a single ALL entry
        for m in range(12):
            self.inflows.add_hydrograph("UH", m, 0, 0.05 + 0.001 * m, 1.0, 2.0)
        self.inflows.add_hydrograph("UH", -1, 0, 0.05, 1.0, 2.0)
        self.assertEqual(self.inflows.hydrograph_count, 13)
        self.assertEqual(self.inflows.get_hydrograph(0).month, 0)
        self.assertEqual(self.inflows.get_hydrograph(11).month, 11)
        self.assertEqual(self.inflows.get_hydrograph(12).month, -1)

    def test_rejects_bad_response(self):
        for bad_response in [-1, 3, 99]:
            with self.subTest(bad_response=bad_response):
                with self.assertRaises(ValueError):
                    self.inflows.add_hydrograph("UH", -1, bad_response, 0.05, 1.0, 2.0)

    def test_rejects_bad_month(self):
        for bad_month in [-2, 12, 100]:
            with self.subTest(bad_month=bad_month):
                with self.assertRaises(ValueError):
                    self.inflows.add_hydrograph("UH", bad_month, 0, 0.05, 1.0, 2.0)

    def test_get_bad_index_raises(self):
        with self.assertRaises(IndexError):
            self.inflows.get_hydrograph(0)


# ---------------------------------------------------------------------------
# [HYDROGRAPHS] — gage assignment lines
# ---------------------------------------------------------------------------
class TestHydrographGageAssignments(InflowsCase):
    """``add_hydrograph_gage`` / ``get_hydrograph_gage``."""

    def test_count_starts_at_zero(self):
        self.assertEqual(self.inflows.hydrograph_gage_count, 0)

    def test_round_trip(self):
        self.inflows.add_hydrograph_gage("SanSewer", "RG1")
        self.inflows.add_hydrograph_gage("Combined", "RG2")
        self.assertEqual(self.inflows.hydrograph_gage_count, 2)

        uh, gage = self.inflows.get_hydrograph_gage(0)
        self.assertEqual((uh, gage), ("SanSewer", "RG1"))
        uh, gage = self.inflows.get_hydrograph_gage(1)
        self.assertEqual((uh, gage), ("Combined", "RG2"))

    def test_get_bad_index_raises(self):
        with self.assertRaises(IndexError):
            self.inflows.get_hydrograph_gage(0)


# ---------------------------------------------------------------------------
# [RDII] — assignment getter (the new round-trip)
# ---------------------------------------------------------------------------
class TestRdiiAssignmentGetter(InflowsCase):
    """``get_rdii`` returns previously-added assignments."""

    def test_round_trip(self):
        self.inflows.add_rdii(0, "SanSewer", 1234.5)
        self.assertEqual(self.inflows.rdii_count, 1)

        node_idx, uh_name, area = self.inflows.get_rdii(0)
        self.assertEqual(node_idx, 0)
        self.assertEqual(uh_name, "SanSewer")
        self.assertAlmostEqual(area, 1234.5, delta=1234.5 * 1e-6)

    def test_multiple_assignments_preserve_order(self):
        self.inflows.add_rdii(0, "A", 100.0)
        self.inflows.add_rdii(0, "B", 200.0)
        self.inflows.add_rdii(0, "C", 300.0)
        self.assertEqual(self.inflows.rdii_count, 3)
        self.assertEqual(self.inflows.get_rdii(0)[1], "A")
        self.assertAlmostEqual(self.inflows.get_rdii(2)[2], 300.0,
                               delta=300.0 * 1e-6)

    def test_get_bad_index_raises(self):
        with self.assertRaises(IndexError):
            self.inflows.get_rdii(0)


# ---------------------------------------------------------------------------
# [RDII_DECAY] — exponential temperature-dependent IA recovery
# ---------------------------------------------------------------------------
class TestRdiiDecay(InflowsCase):
    """The user-facing entry point for the exponential IA model."""

    def test_count_starts_at_zero(self):
        self.assertEqual(self.inflows.rdii_decay_count, 0)

    def test_add_one_returns_one(self):
        self.inflows.add_rdii_decay("SanSewer", 0,
                                    k_dep=0.15, k_0=0.010, k_T=0.070,
                                    T_ref=10.0, theta_rec=0.0, T_freeze=0.0)
        self.assertEqual(self.inflows.rdii_decay_count, 1)

    def test_round_trip_baseline_values(self):
        """A baseline row: T_ref=10, theta_rec=0, T_freeze=0."""
        self.inflows.add_rdii_decay("SanSewer", 0,
                                    k_dep=0.15, k_0=0.010, k_T=0.070,
                                    T_ref=10.0, theta_rec=0.0, T_freeze=0.0)
        e = self.inflows.get_rdii_decay(0)
        self.assertEqual(e.uh_name, "SanSewer")
        self.assertEqual(e.response, 0)
        self.assertAlmostEqual(e.k_dep, 0.15, places=6)
        self.assertAlmostEqual(e.k_0, 0.010, places=6)
        self.assertAlmostEqual(e.k_T, 0.070, places=6)
        self.assertAlmostEqual(e.T_ref, 10.0, places=6)
        self.assertAlmostEqual(e.theta_rec, 0.0, places=6)
        self.assertAlmostEqual(e.T_freeze, 0.0, places=6)

    def test_round_trip_full_parameters(self):
        self.inflows.add_rdii_decay(
            "SanSewer", 1,
            k_dep=0.10, k_0=0.008, k_T=0.037,
            T_ref=12.5, theta_rec=0.055, T_freeze=-1.0,
        )
        e = self.inflows.get_rdii_decay(0)
        self.assertEqual(e.uh_name, "SanSewer")
        self.assertEqual(e.response, 1)
        self.assertAlmostEqual(e.k_dep, 0.10, places=6)
        self.assertAlmostEqual(e.k_0, 0.008, places=6)
        self.assertAlmostEqual(e.k_T, 0.037, places=6)
        self.assertAlmostEqual(e.T_ref, 12.5, places=6)
        self.assertAlmostEqual(e.theta_rec, 0.055, places=6)
        self.assertAlmostEqual(e.T_freeze, -1.0, places=6)

    def test_three_responses(self):
        params = [
            (0, 0.15, 0.010, 0.070),
            (1, 0.10, 0.008, 0.037),
            (2, 0.05, 0.005, 0.013),
        ]
        for response, k_dep, k_0, k_T in params:
            self.inflows.add_rdii_decay("UH", response, k_dep, k_0, k_T,
                                        T_ref=10.0, theta_rec=0.0, T_freeze=0.0)
        self.assertEqual(self.inflows.rdii_decay_count, 3)
        for i, (resp, k_dep, k_0, k_T) in enumerate(params):
            e = self.inflows.get_rdii_decay(i)
            self.assertEqual(e.response, resp)
            self.assertAlmostEqual(e.k_dep, k_dep, places=6)
            self.assertAlmostEqual(e.k_T, k_T, places=6)

    def test_rejects_bad_response(self):
        for bad_response in [-1, 3, 99]:
            with self.subTest(bad_response=bad_response):
                with self.assertRaises(ValueError):
                    self.inflows.add_rdii_decay("UH", bad_response,
                                                k_dep=0.1, k_0=0.01, k_T=0.01,
                                                T_ref=10.0, theta_rec=0.0,
                                                T_freeze=0.0)

    def test_rejects_negative_rate(self):
        for field in ["k_dep", "k_0", "k_T"]:
            with self.subTest(field=field):
                kwargs = dict(k_dep=0.1, k_0=0.01, k_T=0.01,
                              T_ref=10.0, theta_rec=0.0, T_freeze=0.0)
                kwargs[field] = -1.0
                with self.assertRaises(ValueError):
                    self.inflows.add_rdii_decay("UH", 0, **kwargs)

    def test_get_bad_index_raises(self):
        with self.assertRaises(IndexError):
            self.inflows.get_rdii_decay(0)


# ---------------------------------------------------------------------------
# Integrated end-to-end: build a complete RDII setup programmatically
# ---------------------------------------------------------------------------
class TestEndToEndRdiiSetup(InflowsCase):
    """A user building a full RDII model in Python from scratch."""

    def test_full_setup_round_trips(self):
        inflows = self.inflows
        # HYDROGRAPHS — RTK
        inflows.add_hydrograph_gage("SanSewer", "RG1")
        inflows.add_hydrograph("SanSewer", -1, 0,
                                r=0.055, t=1.0,  k=2.0, dmax=8.0, dinit=2.0)
        inflows.add_hydrograph("SanSewer", -1, 1,
                                r=0.032, t=3.5,  k=2.0, dmax=8.0, dinit=2.0)
        inflows.add_hydrograph("SanSewer", -1, 2,
                                r=0.018, t=14.0, k=2.0, dmax=8.0, dinit=2.0)
        # RDII_DECAY — physics-based recovery
        inflows.add_rdii_decay("SanSewer", 0,
                                k_dep=0.15, k_0=0.010, k_T=0.070,
                                T_ref=10.0, theta_rec=0.055, T_freeze=0.0)
        inflows.add_rdii_decay("SanSewer", 1,
                                k_dep=0.10, k_0=0.008, k_T=0.037,
                                T_ref=10.0, theta_rec=0.055, T_freeze=0.0)
        inflows.add_rdii_decay("SanSewer", 2,
                                k_dep=0.05, k_0=0.005, k_T=0.013,
                                T_ref=10.0, theta_rec=0.040, T_freeze=0.0)
        # RDII — node assignment
        inflows.add_rdii(0, "SanSewer", 1000.0)

        self.assertEqual(inflows.hydrograph_gage_count, 1)
        self.assertEqual(inflows.hydrograph_count, 3)
        self.assertEqual(inflows.rdii_decay_count, 3)
        self.assertEqual(inflows.rdii_count, 1)

        # Spot-check that the long response row survived
        long_decay = inflows.get_rdii_decay(2)
        self.assertEqual(long_decay.response, 2)
        self.assertAlmostEqual(long_decay.k_dep, 0.05, places=6)
        self.assertAlmostEqual(long_decay.theta_rec, 0.040, places=6)

    def test_no_decay_rows_means_linear_path(self):
        """A model with HYDROGRAPHS but no RDII_DECAY uses linear IA."""
        inflows = self.inflows
        inflows.add_hydrograph_gage("UH", "G1")
        inflows.add_hydrograph("UH", -1, 0, r=0.05, t=1.0, k=2.0,
                                dmax=5.0, drecov=0.1)
        inflows.add_rdii(0, "UH", 100.0)
        self.assertEqual(inflows.hydrograph_count, 1)
        self.assertEqual(inflows.rdii_decay_count, 0)  # exponential model is off


# ---------------------------------------------------------------------------
# [HYDROGRAPHS] — group enumeration (DA-ENG-01)
# ---------------------------------------------------------------------------
class TestHydrographGroupEnumeration(InflowsCase):
    """``hydrograph_group_count`` / ``get_hydrograph_group_id``.

    These accessors de-duplicate the per-(group, month, response) entry
    list so consumers (GUI Object Browser, MCP tools) can enumerate
    groups without manually scanning the raw entries.
    """

    def test_empty_model_has_zero_groups(self):
        self.assertEqual(self.inflows.hydrograph_group_count, 0)

    def test_one_group_twelve_months_counts_as_one(self):
        for m in range(12):
            self.inflows.add_hydrograph("SanSewer", m, 0, 0.05, 1.0, 2.0)
        self.assertEqual(self.inflows.hydrograph_count, 12)
        self.assertEqual(self.inflows.hydrograph_group_count, 1)
        self.assertEqual(self.inflows.get_hydrograph_group_id(0), "SanSewer")

    def test_multiple_groups_first_occurrence_order(self):
        self.inflows.add_hydrograph("Combined", 0, 0, 0.1, 1.0, 2.0)
        self.inflows.add_hydrograph("Sanitary", 0, 0, 0.1, 1.0, 2.0)
        self.inflows.add_hydrograph("Combined", 1, 0, 0.1, 1.0, 2.0)
        self.inflows.add_hydrograph("Storm",    0, 0, 0.1, 1.0, 2.0)
        self.inflows.add_hydrograph("Sanitary", 1, 0, 0.1, 1.0, 2.0)

        self.assertEqual(self.inflows.hydrograph_group_count, 3)
        self.assertEqual(self.inflows.get_hydrograph_group_id(0), "Combined")
        self.assertEqual(self.inflows.get_hydrograph_group_id(1), "Sanitary")
        self.assertEqual(self.inflows.get_hydrograph_group_id(2), "Storm")

    def test_gage_only_groups_are_counted(self):
        # A group introduced via gage assignment alone (no parameter rows
        # yet) still surfaces in the group enumeration so it shows up in
        # the GUI browser.
        self.inflows.add_hydrograph_gage("GageOnly", "G1")
        self.inflows.add_hydrograph("Params", -1, 0, 0.1, 1.0, 2.0)
        self.assertEqual(self.inflows.hydrograph_group_count, 2)
        # Parameter-entry groups come before gage-only groups.
        self.assertEqual(self.inflows.get_hydrograph_group_id(0), "Params")
        self.assertEqual(self.inflows.get_hydrograph_group_id(1), "GageOnly")

    def test_bad_index_raises(self):
        self.inflows.add_hydrograph("UH", -1, 0, 0.1, 1.0, 2.0)
        with self.assertRaises(IndexError):
            self.inflows.get_hydrograph_group_id(1)
