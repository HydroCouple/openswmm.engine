"""Tests for :class:`openswmm.engine.ModelBuilder` programmatic model building."""

import os
import pytest

from openswmm.engine import ModelBuilder, Solver, NodeType, LinkType, XSectShape


# ---------------------------------------------------------------------------
# Construction
# ---------------------------------------------------------------------------
class TestModelBuilderConstruction:
    """ModelBuilder instantiation."""

    def test_create(self):
        m = ModelBuilder()
        # Should not raise -- engine is in BUILDING state
        assert m is not None


# ---------------------------------------------------------------------------
# Adding objects
# ---------------------------------------------------------------------------
class TestModelBuilderAddObjects:
    """Adding nodes, links, subcatchments, and gages."""

    def test_add_node_junction(self):
        m = ModelBuilder()
        err = m.add_node("J1", NodeType.JUNCTION)
        assert err == 0

    def test_add_node_outfall(self):
        m = ModelBuilder()
        err = m.add_node("OUT1", NodeType.OUTFALL)
        assert err == 0

    def test_add_node_storage(self):
        m = ModelBuilder()
        err = m.add_node("S1", NodeType.STORAGE)
        assert err == 0

    def test_add_node_divider(self):
        m = ModelBuilder()
        err = m.add_node("D1", NodeType.DIVIDER)
        assert err == 0

    def test_add_link_conduit(self):
        m = ModelBuilder()
        m.add_node("J1", NodeType.JUNCTION)
        m.add_node("OUT1", NodeType.OUTFALL)
        err = m.add_link("C1", LinkType.CONDUIT)
        assert err == 0

    def test_add_link_pump(self):
        m = ModelBuilder()
        m.add_node("J1", NodeType.JUNCTION)
        m.add_node("J2", NodeType.JUNCTION)
        err = m.add_link("P1", LinkType.PUMP)
        assert err == 0

    def test_add_subcatchment(self):
        m = ModelBuilder()
        err = m.add_subcatchment("SC1")
        assert err == 0

    def test_add_gage(self):
        m = ModelBuilder()
        err = m.add_gage("RG1")
        assert err == 0


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
class TestModelBuilderConfiguration:
    """Setting node and link properties."""

    @pytest.fixture
    def simple_model(self):
        """A minimal model with one junction, one outfall, one conduit."""
        m = ModelBuilder()
        m.add_node("J1", NodeType.JUNCTION)
        m.add_node("OUT1", NodeType.OUTFALL)
        m.add_link("C1", LinkType.CONDUIT)
        return m

    def test_set_node_invert(self, simple_model):
        simple_model.set_node_invert(0, 100.0)
        # No exception means success

    def test_set_node_max_depth(self, simple_model):
        simple_model.set_node_max_depth(0, 6.0)

    def test_set_link_nodes(self, simple_model):
        simple_model.set_link_nodes(0, 0, 1)

    def test_set_link_length(self, simple_model):
        simple_model.set_link_length(0, 400.0)

    def test_set_link_roughness(self, simple_model):
        simple_model.set_link_roughness(0, 0.013)

    def test_set_link_xsect_circular(self, simple_model):
        simple_model.set_link_xsect(0, XSectShape.CIRCULAR, 2.0)

    def test_set_link_xsect_rect(self, simple_model):
        simple_model.set_link_xsect(0, XSectShape.RECT_CLOSED, 3.0, 2.0)


# ---------------------------------------------------------------------------
# Validation and finalization
# ---------------------------------------------------------------------------
class TestModelBuilderFinalization:
    """Validate and finalize a programmatic model."""

    @pytest.fixture
    def complete_model(self):
        m = ModelBuilder()
        m.add_node("J1", NodeType.JUNCTION)
        m.add_node("OUT1", NodeType.OUTFALL)
        m.add_link("C1", LinkType.CONDUIT)
        m.set_node_invert(0, 10.0)
        m.set_node_invert(1, 0.0)
        m.set_link_nodes(0, 0, 1)
        m.set_link_length(0, 300.0)
        m.set_link_roughness(0, 0.013)
        m.set_link_xsect(0, XSectShape.CIRCULAR, 1.5)
        return m

    def test_validate(self, complete_model):
        complete_model.validate()

    def test_finalize(self, complete_model):
        complete_model.validate()
        complete_model.finalize()

    def test_to_solver(self, complete_model):
        complete_model.validate()
        complete_model.finalize()
        solver = complete_model.to_solver()
        assert isinstance(solver, Solver)
        assert solver.handle != 0
        solver.destroy()


# ---------------------------------------------------------------------------
# Model write
# ---------------------------------------------------------------------------
class TestModelBuilderWrite:
    """Export a programmatic model to .inp file."""

    def test_write(self, tmp_path):
        m = ModelBuilder()
        m.add_node("J1", NodeType.JUNCTION)
        m.add_node("OUT1", NodeType.OUTFALL)
        m.add_link("C1", LinkType.CONDUIT)
        m.set_link_nodes(0, 0, 1)

        out_path = str(tmp_path / "model.inp")
        m.write(out_path)
        assert os.path.exists(out_path)
        assert os.path.getsize(out_path) > 0


# ---------------------------------------------------------------------------
# Title management
# ---------------------------------------------------------------------------
class TestModelBuilderTitle:
    """Title CRUD operations on a model."""

    def test_title_count_initially_zero(self):
        m = ModelBuilder()
        assert m.get_title_count() == 0

    def test_add_title_line(self):
        m = ModelBuilder()
        m.add_title_line("First line")
        assert m.get_title_count() == 1
        assert m.get_title_line(0) == "First line"

    def test_set_title(self):
        m = ModelBuilder()
        m.set_title("Line A\nLine B")
        assert m.get_title_count() == 2

    def test_clear_title(self):
        m = ModelBuilder()
        m.add_title_line("Something")
        m.clear_title()
        assert m.get_title_count() == 0
