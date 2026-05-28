"""Runtime coverage of every collection-level add/pop_last in OPENED state.

The C engine widened the lifecycle contract so node, link, subcatchment,
gage, and inflow additions are valid in BUILDING *or* OPENED state.
These tests verify that v1's collection-level editors succeed against a
Solver that has been ``open()``-ed but not initialized.

Migrated to the v1 Pythonic bindings.
"""

import pytest

from openswmm.engine import (
    BadIndexError,
    LinkType,
    NodeType,
)


class TestOpenedStateNodeEditing:
    def test_add_node_round_trip(self, opened_solver):
        before = len(opened_solver.nodes)
        node = opened_solver.nodes.add("PY_TEST_NODE", NodeType.JUNCTION)
        assert len(opened_solver.nodes) == before + 1
        assert opened_solver.nodes.get_index("PY_TEST_NODE") == before
        assert node.id == "PY_TEST_NODE"

    def test_pop_last_node_undoes_add(self, opened_solver):
        before = len(opened_solver.nodes)
        opened_solver.nodes.add("PY_TMP_NODE", NodeType.JUNCTION)
        opened_solver.nodes.pop_last("PY_TMP_NODE")
        assert len(opened_solver.nodes) == before
        with pytest.raises(KeyError):
            opened_solver.nodes.get_index("PY_TMP_NODE")

    def test_pop_last_node_wrong_tail_raises(self, opened_solver):
        opened_solver.nodes.add("PY_REAL_TAIL", NodeType.JUNCTION)
        # v1 raises BadIndexError (also IndexError) instead of returning rc.
        with pytest.raises((BadIndexError, IndexError)):
            opened_solver.nodes.pop_last("NOT_THE_TAIL")
        assert opened_solver.nodes.get_index("PY_REAL_TAIL") >= 0


class TestOpenedStateLinkEditing:
    def test_add_link_round_trip(self, opened_solver):
        opened_solver.nodes.add("PY_LINK_U", NodeType.JUNCTION)
        opened_solver.nodes.add("PY_LINK_D", NodeType.OUTFALL)
        before = len(opened_solver.links)
        link = opened_solver.links.add("PY_TEST_LINK", LinkType.CONDUIT)
        assert len(opened_solver.links) == before + 1
        assert opened_solver.links.get_index("PY_TEST_LINK") == before
        assert link.id == "PY_TEST_LINK"

    def test_pop_last_link_undoes_add(self, opened_solver):
        opened_solver.nodes.add("PY_PL_U", NodeType.JUNCTION)
        opened_solver.nodes.add("PY_PL_D", NodeType.OUTFALL)
        before = len(opened_solver.links)
        opened_solver.links.add("PY_TMP_LINK", LinkType.CONDUIT)
        opened_solver.links.pop_last("PY_TMP_LINK")
        assert len(opened_solver.links) == before
        with pytest.raises(KeyError):
            opened_solver.links.get_index("PY_TMP_LINK")

    def test_pop_last_link_wrong_tail_raises(self, opened_solver):
        opened_solver.nodes.add("PY_PLW_U", NodeType.JUNCTION)
        opened_solver.nodes.add("PY_PLW_D", NodeType.OUTFALL)
        opened_solver.links.add("PY_REAL_LINK_TAIL", LinkType.CONDUIT)
        with pytest.raises((BadIndexError, IndexError)):
            opened_solver.links.pop_last("NOT_THE_LINK_TAIL")
        assert opened_solver.links.get_index("PY_REAL_LINK_TAIL") >= 0


class TestOpenedStateSubcatchmentEditing:
    def test_add_subcatchment_round_trip(self, opened_solver):
        before = len(opened_solver.subcatchments)
        sub = opened_solver.subcatchments.add("PY_TEST_SC")
        assert len(opened_solver.subcatchments) == before + 1
        assert opened_solver.subcatchments.get_index("PY_TEST_SC") == before
        assert sub.id == "PY_TEST_SC"


class TestOpenedStateGageEditing:
    def test_add_gage_round_trip(self, opened_solver):
        before = len(opened_solver.gages)
        gage = opened_solver.gages.add("PY_TEST_GAGE")
        assert len(opened_solver.gages) == before + 1
        assert opened_solver.gages.get_index("PY_TEST_GAGE") == before
        assert gage.id == "PY_TEST_GAGE"


class TestOpenedStateInflowEditing:
    """OPENED state must accept inflow additions."""

    def test_add_external_inflow_in_opened_state(self, opened_solver):
        assert len(opened_solver.nodes) > 0
        opened_solver.inflows.add_external(0, "FLOW", baseline=0.5)
        assert opened_solver.inflows.external_count >= 1

    def test_add_dwf_in_opened_state(self, opened_solver):
        assert len(opened_solver.nodes) > 0
        opened_solver.inflows.add_dwf(0, "FLOW", avg_value=1.5)

    def test_add_rdii_in_opened_state(self, opened_solver):
        opened_solver.inflows.add_rdii(0, "NONEXISTENT_UH", 100.0)
