"""Runtime coverage of every *_add wrapper that's valid in OPENED state.

The C engine widened the lifecycle contract so that node, link,
subcatchment, gage, and inflow additions are valid in BUILDING *or*
OPENED state. These tests assert that each wrapped call succeeds
against a Solver that has been ``open()``-ed but not yet initialized,
and that the resulting object is observable via the existing query API.

Pop-last symmetry (Nodes.pop_last, Links.pop_last) is also covered.
"""

import pytest

from openswmm.engine import (
    Solver,
    Nodes,
    Links,
    Subcatchments,
    Gages,
    Inflows,
    NodeType,
    LinkType,
)

# SWMM_ERR_BADINDEX — returned when pop_last is given a non-tail id.
SWMM_ERR_BADINDEX = 8


class TestOpenedStateNodeEditing:
    def test_add_node_round_trip(self, opened_solver):
        n = Nodes(opened_solver)
        before = n.count()
        rc = n.add("PY_TEST_NODE", int(NodeType.JUNCTION))
        assert rc == 0, f"node add returned {rc}"
        assert n.count() == before + 1
        assert n.get_index("PY_TEST_NODE") == before

    def test_pop_last_node_undoes_add(self, opened_solver):
        n = Nodes(opened_solver)
        before = n.count()
        n.add("PY_TMP_NODE", int(NodeType.JUNCTION))
        rc = n.pop_last("PY_TMP_NODE")
        assert rc == 0
        assert n.count() == before
        assert n.get_index("PY_TMP_NODE") == -1

    def test_pop_last_node_wrong_tail_returns_badindex(self, opened_solver):
        n = Nodes(opened_solver)
        n.add("PY_REAL_TAIL", int(NodeType.JUNCTION))
        rc = n.pop_last("NOT_THE_TAIL")
        assert rc == SWMM_ERR_BADINDEX, (
            f"wrong-tail pop should return BADINDEX (8); got {rc}"
        )
        # Tail still present after the failed pop.
        assert n.get_index("PY_REAL_TAIL") >= 0


class TestOpenedStateLinkEditing:
    def test_add_link_round_trip(self, opened_solver):
        # Need two nodes for a link to reference.
        n = Nodes(opened_solver)
        u = "PY_LINK_U"
        d = "PY_LINK_D"
        n.add(u, int(NodeType.JUNCTION))
        n.add(d, int(NodeType.OUTFALL))

        ll = Links(opened_solver)
        before = ll.count()
        rc = ll.add("PY_TEST_LINK", int(LinkType.CONDUIT))
        assert rc == 0, f"link add returned {rc}"
        assert ll.count() == before + 1
        assert ll.get_index("PY_TEST_LINK") == before

    def test_pop_last_link_undoes_add(self, opened_solver):
        n = Nodes(opened_solver)
        n.add("PY_PL_U", int(NodeType.JUNCTION))
        n.add("PY_PL_D", int(NodeType.OUTFALL))

        ll = Links(opened_solver)
        before = ll.count()
        ll.add("PY_TMP_LINK", int(LinkType.CONDUIT))
        rc = ll.pop_last("PY_TMP_LINK")
        assert rc == 0
        assert ll.count() == before
        assert ll.get_index("PY_TMP_LINK") == -1

    def test_pop_last_link_wrong_tail_returns_badindex(self, opened_solver):
        n = Nodes(opened_solver)
        n.add("PY_PLW_U", int(NodeType.JUNCTION))
        n.add("PY_PLW_D", int(NodeType.OUTFALL))

        ll = Links(opened_solver)
        ll.add("PY_REAL_LINK_TAIL", int(LinkType.CONDUIT))
        rc = ll.pop_last("NOT_THE_LINK_TAIL")
        assert rc == SWMM_ERR_BADINDEX
        assert ll.get_index("PY_REAL_LINK_TAIL") >= 0


class TestOpenedStateSubcatchmentEditing:
    def test_add_subcatchment_round_trip(self, opened_solver):
        sc = Subcatchments(opened_solver)
        before = sc.count()
        rc = sc.add("PY_TEST_SC")
        assert rc == 0, f"subcatch add returned {rc}"
        assert sc.count() == before + 1
        assert sc.get_index("PY_TEST_SC") == before


class TestOpenedStateGageEditing:
    def test_add_gage_round_trip(self, opened_solver):
        g = Gages(opened_solver)
        before = g.count()
        rc = g.add("PY_TEST_GAGE")
        assert rc == 0, f"gage add returned {rc}"
        assert g.count() == before + 1
        assert g.get_index("PY_TEST_GAGE") == before


class TestOpenedStateInflowEditing:
    """Inflows.add_* wraps swmm_ext_inflow_add / swmm_dwf_add / swmm_rdii_add.

    Header docs do not state a state restriction — these tests verify that
    OPENED state is in fact accepted.
    """

    def test_add_external_inflow_in_opened_state(self, opened_solver):
        nodes = Nodes(opened_solver)
        # site_drainage fixture has at least one node — use index 0.
        assert nodes.count() > 0
        inflows = Inflows(opened_solver)
        # Constant baseline inflow, no time series — minimum-required arguments.
        inflows.add_external(0, "FLOW", baseline=0.5)
        assert inflows.ext_inflow_count() >= 1

    def test_add_dwf_in_opened_state(self, opened_solver):
        nodes = Nodes(opened_solver)
        assert nodes.count() > 0
        inflows = Inflows(opened_solver)
        inflows.add_dwf(0, "FLOW", 1.5)

    def test_add_rdii_in_opened_state(self, opened_solver):
        # The binding dispatches into the C engine in OPENED state
        # without raising EngineError for the LIFECYCLE check. The
        # engine accepts the call and stores the entry; we only assert
        # that the binding completes (no exception).
        inflows = Inflows(opened_solver)
        inflows.add_rdii(0, "NONEXISTENT_UH", 100.0)
