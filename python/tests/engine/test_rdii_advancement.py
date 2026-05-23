"""Tests for the RDII advancement in :class:`openswmm.engine.Inflows`.

Covers the new hydrograph / RDII assignment / RDII decay accessors added
to the C API and Cython bindings. The decay surface (``add_rdii_decay``,
``get_rdii_decay``, ``rdii_decay_count``) is the user-facing entry point
for the exponential temperature-dependent initial-abstraction model.

See ``docs/RDII_ExpDecay_Implementation.md`` for the formulation.
"""

import pytest

from openswmm.engine import Inflows, ModelBuilder, NodeType


# ---------------------------------------------------------------------------
# Fixture: minimal in-memory model with one node, exposed via ModelBuilder
# ---------------------------------------------------------------------------
@pytest.fixture
def builder_with_node():
    """A fresh ModelBuilder with one junction so RDII assignment is valid."""
    m = ModelBuilder()
    m.add_node("J1", NodeType.JUNCTION)
    return m


@pytest.fixture
def inflows(builder_with_node):
    """An :class:`Inflows` accessor on a one-node ModelBuilder solver."""
    solver = builder_with_node.to_solver()
    return Inflows(solver)


# ---------------------------------------------------------------------------
# [HYDROGRAPHS] — parameter lines
# ---------------------------------------------------------------------------
class TestHydrographParameters:
    """``add_hydrograph`` / ``get_hydrograph`` / ``hydrograph_count``."""

    def test_count_starts_at_zero(self, inflows):
        assert inflows.hydrograph_count() == 0

    def test_add_one_returns_one(self, inflows):
        inflows.add_hydrograph("SanSewer", -1, 0, 0.055, 1.0, 2.0)
        assert inflows.hydrograph_count() == 1

    def test_round_trip_all_fields(self, inflows):
        inflows.add_hydrograph(
            "SanSewer", -1, 0,
            r=0.055, t=1.0, k=2.0,
            dmax=8.0, drecov=0.10, dinit=2.0,
        )
        e = inflows.get_hydrograph(0)
        assert e["uh_name"] == "SanSewer"
        assert e["month"] == -1
        assert e["response"] == 0
        assert e["r"] == pytest.approx(0.055)
        assert e["t"] == pytest.approx(1.0)
        assert e["k"] == pytest.approx(2.0)
        assert e["dmax"] == pytest.approx(8.0)
        assert e["drecov"] == pytest.approx(0.10)
        assert e["dinit"] == pytest.approx(2.0)

    def test_three_responses(self, inflows):
        for response, t in [(0, 1.0), (1, 3.5), (2, 14.0)]:
            inflows.add_hydrograph("SanSewer", -1, response,
                                   r=0.05, t=t, k=2.0)
        assert inflows.hydrograph_count() == 3
        assert inflows.get_hydrograph(1)["response"] == 1
        assert inflows.get_hydrograph(2)["t"] == pytest.approx(14.0)

    def test_monthly_entries(self, inflows):
        # 12 monthly entries plus a single ALL entry
        for m in range(12):
            inflows.add_hydrograph("UH", m, 0, 0.05 + 0.001 * m, 1.0, 2.0)
        inflows.add_hydrograph("UH", -1, 0, 0.05, 1.0, 2.0)
        assert inflows.hydrograph_count() == 13
        assert inflows.get_hydrograph(0)["month"] == 0
        assert inflows.get_hydrograph(11)["month"] == 11
        assert inflows.get_hydrograph(12)["month"] == -1

    @pytest.mark.parametrize("bad_response", [-1, 3, 99])
    def test_rejects_bad_response(self, inflows, bad_response):
        with pytest.raises(RuntimeError):
            inflows.add_hydrograph("UH", -1, bad_response, 0.05, 1.0, 2.0)

    @pytest.mark.parametrize("bad_month", [-2, 12, 100])
    def test_rejects_bad_month(self, inflows, bad_month):
        with pytest.raises(RuntimeError):
            inflows.add_hydrograph("UH", bad_month, 0, 0.05, 1.0, 2.0)

    def test_get_bad_index_raises(self, inflows):
        with pytest.raises(RuntimeError):
            inflows.get_hydrograph(0)


# ---------------------------------------------------------------------------
# [HYDROGRAPHS] — gage assignment lines
# ---------------------------------------------------------------------------
class TestHydrographGageAssignments:
    """``add_hydrograph_gage`` / ``get_hydrograph_gage``."""

    def test_count_starts_at_zero(self, inflows):
        assert inflows.hydrograph_gage_count() == 0

    def test_round_trip(self, inflows):
        inflows.add_hydrograph_gage("SanSewer", "RG1")
        inflows.add_hydrograph_gage("Combined", "RG2")
        assert inflows.hydrograph_gage_count() == 2

        uh, gage = inflows.get_hydrograph_gage(0)
        assert (uh, gage) == ("SanSewer", "RG1")
        uh, gage = inflows.get_hydrograph_gage(1)
        assert (uh, gage) == ("Combined", "RG2")

    def test_get_bad_index_raises(self, inflows):
        with pytest.raises(RuntimeError):
            inflows.get_hydrograph_gage(0)


# ---------------------------------------------------------------------------
# [RDII] — assignment getter (the new round-trip)
# ---------------------------------------------------------------------------
class TestRdiiAssignmentGetter:
    """``get_rdii`` returns previously-added assignments."""

    def test_round_trip(self, inflows):
        inflows.add_rdii(0, "SanSewer", 1234.5)
        assert inflows.rdii_count() == 1

        node_idx, uh_name, area = inflows.get_rdii(0)
        assert node_idx == 0
        assert uh_name == "SanSewer"
        assert area == pytest.approx(1234.5)

    def test_multiple_assignments_preserve_order(self, inflows):
        inflows.add_rdii(0, "A", 100.0)
        inflows.add_rdii(0, "B", 200.0)
        inflows.add_rdii(0, "C", 300.0)
        assert inflows.rdii_count() == 3
        assert inflows.get_rdii(0)[1] == "A"
        assert inflows.get_rdii(2)[2] == pytest.approx(300.0)

    def test_get_bad_index_raises(self, inflows):
        with pytest.raises(RuntimeError):
            inflows.get_rdii(0)


# ---------------------------------------------------------------------------
# [RDII_DECAY] — exponential temperature-dependent IA recovery
# ---------------------------------------------------------------------------
class TestRdiiDecay:
    """The user-facing entry point for the exponential IA model."""

    def test_count_starts_at_zero(self, inflows):
        assert inflows.rdii_decay_count() == 0

    def test_add_one_returns_one(self, inflows):
        inflows.add_rdii_decay("SanSewer", 0,
                               k_dep=0.15, k_0=0.010, k_T=0.070)
        assert inflows.rdii_decay_count() == 1

    def test_round_trip_with_defaults(self, inflows):
        """Defaults: T_ref=10, theta_rec=0, T_freeze=0."""
        inflows.add_rdii_decay("SanSewer", 0,
                               k_dep=0.15, k_0=0.010, k_T=0.070)
        e = inflows.get_rdii_decay(0)
        assert e["uh_name"] == "SanSewer"
        assert e["response"] == 0
        assert e["k_dep"] == pytest.approx(0.15)
        assert e["k_0"]   == pytest.approx(0.010)
        assert e["k_T"]   == pytest.approx(0.070)
        assert e["T_ref"] == pytest.approx(10.0)
        assert e["theta_rec"] == pytest.approx(0.0)
        assert e["T_freeze"]  == pytest.approx(0.0)

    def test_round_trip_full_parameters(self, inflows):
        inflows.add_rdii_decay(
            "SanSewer", 1,
            k_dep=0.10, k_0=0.008, k_T=0.037,
            T_ref=12.5, theta_rec=0.055, T_freeze=-1.0,
        )
        e = inflows.get_rdii_decay(0)
        assert e == {
            "uh_name":   "SanSewer",
            "response":  1,
            "k_dep":     pytest.approx(0.10),
            "k_0":       pytest.approx(0.008),
            "k_T":       pytest.approx(0.037),
            "T_ref":     pytest.approx(12.5),
            "theta_rec": pytest.approx(0.055),
            "T_freeze":  pytest.approx(-1.0),
        }

    def test_three_responses(self, inflows):
        params = [
            (0, 0.15, 0.010, 0.070),
            (1, 0.10, 0.008, 0.037),
            (2, 0.05, 0.005, 0.013),
        ]
        for response, k_dep, k_0, k_T in params:
            inflows.add_rdii_decay("UH", response, k_dep, k_0, k_T)
        assert inflows.rdii_decay_count() == 3
        for i, (resp, k_dep, k_0, k_T) in enumerate(params):
            e = inflows.get_rdii_decay(i)
            assert e["response"] == resp
            assert e["k_dep"] == pytest.approx(k_dep)
            assert e["k_T"]   == pytest.approx(k_T)

    @pytest.mark.parametrize("bad_response", [-1, 3, 99])
    def test_rejects_bad_response(self, inflows, bad_response):
        with pytest.raises(RuntimeError):
            inflows.add_rdii_decay("UH", bad_response,
                                   k_dep=0.1, k_0=0.01, k_T=0.01)

    @pytest.mark.parametrize("field", ["k_dep", "k_0", "k_T"])
    def test_rejects_negative_rate(self, inflows, field):
        kwargs = dict(k_dep=0.1, k_0=0.01, k_T=0.01)
        kwargs[field] = -1.0
        with pytest.raises(RuntimeError):
            inflows.add_rdii_decay("UH", 0, **kwargs)

    def test_get_bad_index_raises(self, inflows):
        with pytest.raises(RuntimeError):
            inflows.get_rdii_decay(0)


# ---------------------------------------------------------------------------
# Integrated end-to-end: build a complete RDII setup programmatically
# ---------------------------------------------------------------------------
class TestEndToEndRdiiSetup:
    """A user building a full RDII model in Python from scratch."""

    def test_full_setup_round_trips(self, inflows):
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
                                T_ref=10.0, theta_rec=0.055)
        inflows.add_rdii_decay("SanSewer", 1,
                                k_dep=0.10, k_0=0.008, k_T=0.037,
                                T_ref=10.0, theta_rec=0.055)
        inflows.add_rdii_decay("SanSewer", 2,
                                k_dep=0.05, k_0=0.005, k_T=0.013,
                                T_ref=10.0, theta_rec=0.040)
        # RDII — node assignment
        inflows.add_rdii(0, "SanSewer", 1000.0)

        assert inflows.hydrograph_gage_count() == 1
        assert inflows.hydrograph_count() == 3
        assert inflows.rdii_decay_count() == 3
        assert inflows.rdii_count() == 1

        # Spot-check that the long response row survived
        long_decay = inflows.get_rdii_decay(2)
        assert long_decay["response"] == 2
        assert long_decay["k_dep"] == pytest.approx(0.05)
        assert long_decay["theta_rec"] == pytest.approx(0.040)

    def test_no_decay_rows_means_linear_path(self, inflows):
        """A model with HYDROGRAPHS but no RDII_DECAY uses linear IA."""
        inflows.add_hydrograph_gage("UH", "G1")
        inflows.add_hydrograph("UH", -1, 0, r=0.05, t=1.0, k=2.0,
                                dmax=5.0, drecov=0.1)
        inflows.add_rdii(0, "UH", 100.0)
        assert inflows.hydrograph_count() == 1
        assert inflows.rdii_decay_count() == 0  # exponential model is off


# ---------------------------------------------------------------------------
# [HYDROGRAPHS] — group enumeration (DA-ENG-01)
# ---------------------------------------------------------------------------
class TestHydrographGroupEnumeration:
    """``hydrograph_group_count`` / ``get_hydrograph_group_id``.

    These accessors de-duplicate the per-(group, month, response) entry
    list so consumers (GUI Object Browser, MCP tools) can enumerate
    groups without manually scanning the raw entries.
    """

    def test_empty_model_has_zero_groups(self, inflows):
        assert inflows.hydrograph_group_count() == 0

    def test_one_group_twelve_months_counts_as_one(self, inflows):
        for m in range(12):
            inflows.add_hydrograph("SanSewer", m, 0, 0.05, 1.0, 2.0)
        assert inflows.hydrograph_count() == 12
        assert inflows.hydrograph_group_count() == 1
        assert inflows.get_hydrograph_group_id(0) == "SanSewer"

    def test_multiple_groups_first_occurrence_order(self, inflows):
        inflows.add_hydrograph("Combined", 0, 0, 0.1, 1.0, 2.0)
        inflows.add_hydrograph("Sanitary", 0, 0, 0.1, 1.0, 2.0)
        inflows.add_hydrograph("Combined", 1, 0, 0.1, 1.0, 2.0)
        inflows.add_hydrograph("Storm",    0, 0, 0.1, 1.0, 2.0)
        inflows.add_hydrograph("Sanitary", 1, 0, 0.1, 1.0, 2.0)

        assert inflows.hydrograph_group_count() == 3
        assert inflows.get_hydrograph_group_id(0) == "Combined"
        assert inflows.get_hydrograph_group_id(1) == "Sanitary"
        assert inflows.get_hydrograph_group_id(2) == "Storm"

    def test_gage_only_groups_are_counted(self, inflows):
        # A group introduced via gage assignment alone (no parameter rows
        # yet) still surfaces in the group enumeration so it shows up in
        # the GUI browser.
        inflows.add_hydrograph_gage("GageOnly", "G1")
        inflows.add_hydrograph("Params", -1, 0, 0.1, 1.0, 2.0)
        assert inflows.hydrograph_group_count() == 2
        # Parameter-entry groups come before gage-only groups.
        assert inflows.get_hydrograph_group_id(0) == "Params"
        assert inflows.get_hydrograph_group_id(1) == "GageOnly"

    def test_bad_index_raises(self, inflows):
        inflows.add_hydrograph("UH", -1, 0, 0.1, 1.0, 2.0)
        with pytest.raises(RuntimeError):
            inflows.get_hydrograph_group_id(1)
