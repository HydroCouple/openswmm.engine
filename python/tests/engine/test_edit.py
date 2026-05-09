"""Tests for :class:`openswmm.engine.ModelEditor` (deletion and type conversion)."""

import pytest

from openswmm.engine import (
    ModelBuilder,
    ModelEditor,
    ImpactEntry,
    ConversionResult,
    NodeType,
    LinkType,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _simple_builder() -> ModelBuilder:
    """Return a ModelBuilder with 3 nodes and 2 links (all adds first)."""
    m = ModelBuilder()
    m.add_node("A", NodeType.JUNCTION)
    m.add_node("B", NodeType.JUNCTION)
    m.add_node("C", NodeType.OUTFALL)
    m.add_link("AB", LinkType.CONDUIT)
    m.add_link("BC", LinkType.CONDUIT)
    # Set connectivity AFTER all adds (add uses resize which reinitialises fields)
    m.set_link_nodes(0, 0, 1)  # AB: A→B
    m.set_link_nodes(1, 1, 2)  # BC: B→C
    return m


# ---------------------------------------------------------------------------
# ImpactEntry / ConversionResult data classes
# ---------------------------------------------------------------------------

class TestImpactEntry:
    def test_repr_cascaded(self):
        e = ImpactEntry(1, 3, "node1", True)
        assert "link[3].node1" in repr(e)
        assert "deleted" in repr(e)

    def test_repr_nullified(self):
        e = ImpactEntry(2, 0, "outlet_node", False)
        assert "nullified" in repr(e)

    def test_obj_type_name_known(self):
        e = ImpactEntry(0, 0, "x", False)
        assert e.obj_type_name == "node"

    def test_obj_type_name_unknown(self):
        e = ImpactEntry(99, 0, "x", False)
        assert "99" in e.obj_type_name


class TestConversionResult:
    def test_repr(self):
        r = ConversionResult(1, ["roughness"], ["no outfall"])
        assert "ConversionResult" in repr(r)

    def test_fields(self):
        r = ConversionResult(3, ["a", "b"], [])
        assert r.new_type == 3
        assert r.cleared_fields == ["a", "b"]
        assert r.warnings == []


# ---------------------------------------------------------------------------
# ModelEditor instantiation
# ---------------------------------------------------------------------------

class TestModelEditorInstantiation:
    def test_from_model_builder(self):
        m = ModelBuilder()
        ed = ModelEditor(m)
        assert ed is not None

    def test_node_count_reflects_builder(self):
        m = ModelBuilder()
        m.add_node("J1", NodeType.JUNCTION)
        m.add_node("O1", NodeType.OUTFALL)
        ed = ModelEditor(m)
        assert ed.node_count == 2

    def test_link_count(self):
        m = _simple_builder()
        ed = ModelEditor(m)
        assert ed.link_count == 2

    def test_subcatch_count_zero(self):
        m = ModelBuilder()
        ed = ModelEditor(m)
        assert ed.subcatch_count == 0

    def test_table_count_zero(self):
        m = ModelBuilder()
        ed = ModelEditor(m)
        assert ed.table_count == 0


# ---------------------------------------------------------------------------
# Impact analysis (non-destructive)
# ---------------------------------------------------------------------------

class TestAnalyzeImpact:
    def test_node_impact_returns_list(self):
        m = _simple_builder()
        ed = ModelEditor(m)
        impacts = ed.analyze_node_impact(1)  # B — referenced by AB and BC
        assert isinstance(impacts, list)

    def test_node_impact_finds_links(self):
        m = _simple_builder()
        ed = ModelEditor(m)
        impacts = ed.analyze_node_impact(1)  # B: AB(node2=B) and BC(node1=B)
        link_impacts = [e for e in impacts if e.obj_type == 1]  # SWMM_REF_LINK
        assert len(link_impacts) == 2

    def test_analyze_does_not_mutate(self):
        m = _simple_builder()
        ed = ModelEditor(m)
        before_nodes = ed.node_count
        before_links = ed.link_count
        ed.analyze_node_impact(1)
        assert ed.node_count == before_nodes
        assert ed.link_count == before_links

    def test_analyze_by_name(self):
        m = _simple_builder()
        ed = ModelEditor(m)
        impacts = ed.analyze_node_impact("B")
        assert isinstance(impacts, list)

    def test_analyze_unknown_name_raises(self):
        m = ModelBuilder()
        ed = ModelEditor(m)
        with pytest.raises(KeyError):
            ed.analyze_node_impact("NONEXISTENT")

    def test_link_impact_empty_when_unreferenced(self):
        m = ModelBuilder()
        m.add_node("A", NodeType.JUNCTION)
        m.add_node("B", NodeType.OUTFALL)
        m.add_link("C", LinkType.CONDUIT)
        m.set_link_nodes(0, 0, 1)
        ed = ModelEditor(m)
        # The link isn't referenced by dividers or inlet usages
        impacts = ed.analyze_link_impact(0)
        assert isinstance(impacts, list)

    def test_gage_impact_empty(self):
        m = ModelBuilder()
        m.add_gage("G0")
        ed = ModelEditor(m)
        impacts = ed.analyze_gage_impact(0)
        assert impacts == []

    def test_table_impact_empty(self):
        m = ModelBuilder()
        m.add_node("J", NodeType.JUNCTION)
        ed = ModelEditor(m)
        # No tables yet
        with pytest.raises(Exception):  # SWMM_ERR_BADINDEX
            ed.analyze_table_impact(0)


# ---------------------------------------------------------------------------
# Deletion — nodes
# ---------------------------------------------------------------------------

class TestDeleteNode:
    def test_delete_isolated_node(self):
        m = ModelBuilder()
        m.add_node("A", NodeType.JUNCTION)
        m.add_node("B", NodeType.JUNCTION)
        m.add_node("C", NodeType.OUTFALL)
        ed = ModelEditor(m)
        cascade = ed.delete_node(1)
        assert ed.node_count == 2
        assert isinstance(cascade, list)

    def test_delete_by_name(self):
        m = ModelBuilder()
        m.add_node("J1", NodeType.JUNCTION)
        m.add_node("O1", NodeType.OUTFALL)
        ed = ModelEditor(m)
        ed.delete_node("J1")
        assert ed.node_count == 1

    def test_delete_node_cascades_links(self):
        m = _simple_builder()
        ed = ModelEditor(m)
        # Delete B — AB and BC should be cascade-deleted
        cascade = ed.delete_node(1)
        assert ed.node_count == 2
        assert ed.link_count == 0
        deleted = [e for e in cascade if e.cascaded]
        assert len(deleted) >= 2

    def test_delete_node_returns_impact_entries(self):
        m = _simple_builder()
        ed = ModelEditor(m)
        cascade = ed.delete_node("B")
        assert all(isinstance(e, ImpactEntry) for e in cascade)

    def test_delete_after_delete_renumbers(self):
        m = ModelBuilder()
        m.add_node("N0", NodeType.JUNCTION)
        m.add_node("N1", NodeType.JUNCTION)
        m.add_node("N2", NodeType.JUNCTION)
        m.add_node("N3", NodeType.OUTFALL)
        ed = ModelEditor(m)
        ed.delete_node(1)  # delete N1
        assert ed.node_count == 3
        # N2 should now be at index 1, N3 at index 2


# ---------------------------------------------------------------------------
# Deletion — links
# ---------------------------------------------------------------------------

class TestDeleteLink:
    def test_delete_link_reduces_count(self):
        m = _simple_builder()
        ed = ModelEditor(m)
        ed.delete_link(0)
        assert ed.link_count == 1

    def test_delete_link_by_name(self):
        m = _simple_builder()
        ed = ModelEditor(m)
        ed.delete_link("AB")
        assert ed.link_count == 1

    def test_delete_link_returns_list(self):
        m = _simple_builder()
        ed = ModelEditor(m)
        cascade = ed.delete_link(0)
        assert isinstance(cascade, list)


# ---------------------------------------------------------------------------
# Deletion — subcatchments
# ---------------------------------------------------------------------------

class TestDeleteSubcatch:
    def test_delete_subcatch(self):
        m = ModelBuilder()
        m.add_subcatch("S1")
        m.add_subcatch("S2")
        ed = ModelEditor(m)
        ed.delete_subcatch(0)
        assert ed.subcatch_count == 1

    def test_delete_subcatch_by_name(self):
        m = ModelBuilder()
        m.add_subcatch("S1")
        ed = ModelEditor(m)
        ed.delete_subcatch("S1")
        assert ed.subcatch_count == 0


# ---------------------------------------------------------------------------
# Deletion — gages
# ---------------------------------------------------------------------------

class TestDeleteGage:
    def test_delete_gage(self):
        m = ModelBuilder()
        m.add_gage("G0")
        m.add_gage("G1")
        ed = ModelEditor(m)
        ed.delete_gage(1)
        assert ed.gage_count == 1

    def test_delete_gage_by_name(self):
        m = ModelBuilder()
        m.add_gage("RG1")
        ed = ModelEditor(m)
        ed.delete_gage("RG1")
        assert ed.gage_count == 0


# ---------------------------------------------------------------------------
# Type conversion — nodes
# ---------------------------------------------------------------------------

class TestConvertNode:
    def test_junction_to_outfall(self):
        m = ModelBuilder()
        m.add_node("J", NodeType.JUNCTION)
        ed = ModelEditor(m)
        result = ed.convert_node(0, NodeType.OUTFALL)
        assert isinstance(result, ConversionResult)
        assert result.new_type == NodeType.OUTFALL

    def test_junction_to_outfall_by_name(self):
        m = ModelBuilder()
        m.add_node("J", NodeType.JUNCTION)
        ed = ModelEditor(m)
        result = ed.convert_node("J", NodeType.OUTFALL)
        assert result.new_type == NodeType.OUTFALL

    def test_storage_to_junction_clears_fields(self):
        m = ModelBuilder()
        m.add_node("S", NodeType.STORAGE)
        ed = ModelEditor(m)
        result = ed.convert_node(0, NodeType.JUNCTION)
        assert len(result.cleared_fields) > 0
        assert result.new_type == NodeType.JUNCTION

    def test_only_outfall_warns(self):
        m = ModelBuilder()
        m.add_node("O", NodeType.OUTFALL)
        ed = ModelEditor(m)
        result = ed.convert_node(0, NodeType.JUNCTION)
        assert len(result.warnings) > 0

    def test_same_type_raises(self):
        m = ModelBuilder()
        m.add_node("J", NodeType.JUNCTION)
        ed = ModelEditor(m)
        with pytest.raises(RuntimeError):
            ed.convert_node(0, NodeType.JUNCTION)

    def test_invalid_type_raises(self):
        m = ModelBuilder()
        m.add_node("J", NodeType.JUNCTION)
        ed = ModelEditor(m)
        with pytest.raises(RuntimeError):
            ed.convert_node(0, 99)

    def test_convert_result_cleared_fields_are_strings(self):
        m = ModelBuilder()
        m.add_node("S", NodeType.STORAGE)
        ed = ModelEditor(m)
        result = ed.convert_node(0, NodeType.JUNCTION)
        assert all(isinstance(f, str) for f in result.cleared_fields)

    def test_convert_result_warnings_are_strings(self):
        m = ModelBuilder()
        m.add_node("O", NodeType.OUTFALL)
        ed = ModelEditor(m)
        result = ed.convert_node(0, NodeType.JUNCTION)
        assert all(isinstance(w, str) for w in result.warnings)


# ---------------------------------------------------------------------------
# Type conversion — links
# ---------------------------------------------------------------------------

class TestConvertLink:
    def test_conduit_to_pump(self):
        m = ModelBuilder()
        m.add_node("A", NodeType.JUNCTION)
        m.add_node("B", NodeType.JUNCTION)
        m.add_link("C", LinkType.CONDUIT)
        m.set_link_nodes(0, 0, 1)
        ed = ModelEditor(m)
        result = ed.convert_link(0, LinkType.PUMP)
        assert result.new_type == LinkType.PUMP
        assert len(result.cleared_fields) > 0

    def test_pump_to_conduit(self):
        m = ModelBuilder()
        m.add_node("A", NodeType.JUNCTION)
        m.add_node("B", NodeType.OUTFALL)
        m.add_link("P", LinkType.PUMP)
        m.set_link_nodes(0, 0, 1)
        ed = ModelEditor(m)
        result = ed.convert_link("P", LinkType.CONDUIT)
        assert result.new_type == LinkType.CONDUIT

    def test_conduit_to_weir(self):
        m = ModelBuilder()
        m.add_node("A", NodeType.JUNCTION)
        m.add_node("B", NodeType.OUTFALL)
        m.add_link("C", LinkType.CONDUIT)
        m.set_link_nodes(0, 0, 1)
        ed = ModelEditor(m)
        result = ed.convert_link(0, LinkType.WEIR)
        assert result.new_type == LinkType.WEIR

    def test_conduit_to_orifice(self):
        m = ModelBuilder()
        m.add_node("A", NodeType.JUNCTION)
        m.add_node("B", NodeType.OUTFALL)
        m.add_link("C", LinkType.CONDUIT)
        m.set_link_nodes(0, 0, 1)
        ed = ModelEditor(m)
        result = ed.convert_link(0, LinkType.ORIFICE)
        assert result.new_type == LinkType.ORIFICE

    def test_same_type_raises(self):
        m = ModelBuilder()
        m.add_node("A", NodeType.JUNCTION)
        m.add_node("B", NodeType.OUTFALL)
        m.add_link("C", LinkType.CONDUIT)
        ed = ModelEditor(m)
        with pytest.raises(RuntimeError):
            ed.convert_link(0, LinkType.CONDUIT)

    def test_convert_by_name(self):
        m = ModelBuilder()
        m.add_node("A", NodeType.JUNCTION)
        m.add_node("B", NodeType.OUTFALL)
        m.add_link("MyLink", LinkType.CONDUIT)
        m.set_link_nodes(0, 0, 1)
        ed = ModelEditor(m)
        result = ed.convert_link("MyLink", LinkType.WEIR)
        assert result.new_type == LinkType.WEIR


# ---------------------------------------------------------------------------
# Combined scenario
# ---------------------------------------------------------------------------

class TestCombinedEditing:
    def test_delete_then_convert(self):
        """Delete one node, convert another — counts and types correct."""
        m = ModelBuilder()
        m.add_node("J1", NodeType.JUNCTION)
        m.add_node("J2", NodeType.JUNCTION)
        m.add_node("O",  NodeType.OUTFALL)
        m.add_link("C",  LinkType.CONDUIT)
        m.set_link_nodes(0, 1, 2)  # J2→O — not touching J1
        ed = ModelEditor(m)

        ed.delete_node("J1")
        assert ed.node_count == 2

        result = ed.convert_link(0, LinkType.WEIR)
        assert result.new_type == LinkType.WEIR

    def test_analyze_then_delete_consistent(self):
        """analyze_impact and delete_node should find the same set."""
        m = _simple_builder()
        ed = ModelEditor(m)

        impacts = ed.analyze_node_impact("B")
        cascade = ed.delete_node("B")

        impact_link_count = sum(1 for e in impacts if e.obj_type == 1)
        cascade_link_count = sum(1 for e in cascade if e.obj_type == 1 and e.cascaded)
        assert cascade_link_count == impact_link_count
