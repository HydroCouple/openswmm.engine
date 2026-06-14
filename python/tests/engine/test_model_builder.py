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


# ---------------------------------------------------------------------------
# Plugins (new additions)
# ---------------------------------------------------------------------------
class TestModelBuilderPlugins:
    """plugins_count / plugin_set / plugin_get / plugin_remove."""

    def test_plugins_count_initially_zero(self):
        m = ModelBuilder()
        count = m.plugins_count()
        assert isinstance(count, int)
        assert count >= 0

    def test_plugin_set_increments_count(self):
        m = ModelBuilder()
        before = m.plugins_count()
        m.plugin_set("myplugin.so", "arg1 arg2")
        assert m.plugins_count() == before + 1

    def test_plugin_get_roundtrip(self):
        m = ModelBuilder()
        m.plugin_set("myplugin.so", "arg1")
        idx = m.plugins_count() - 1
        path, args = m.plugin_get(idx)
        assert "myplugin.so" in path
        assert "arg1" in args

    def test_plugin_remove_decrements_count(self):
        m = ModelBuilder()
        m.plugin_set("todelete.so", "")
        before = m.plugins_count()
        m.plugin_remove("todelete.so")
        assert m.plugins_count() == before - 1

    def test_plugin_remove_idempotent(self):
        m = ModelBuilder()
        # Removing a non-existent plugin should not raise
        m.plugin_remove("nonexistent.so")


# ---------------------------------------------------------------------------
# Files section (new additions)
# ---------------------------------------------------------------------------
class TestModelBuilderFiles:
    """files_get / files_set."""

    def test_files_get_known_key_returns_str(self):
        m = ModelBuilder()
        val = m.files_get("HOTSTART_SAVE_PATH")
        assert isinstance(val, str)

    def test_files_set_roundtrip(self):
        m = ModelBuilder()
        m.files_set("HOTSTART_SAVE_PATH", "/tmp/test.hs")
        val = m.files_get("HOTSTART_SAVE_PATH")
        assert val == "/tmp/test.hs"

    def test_files_set_clear(self):
        m = ModelBuilder()
        m.files_set("HOTSTART_SAVE_PATH", "/tmp/test.hs")
        m.files_set("HOTSTART_SAVE_PATH", "")
        val = m.files_get("HOTSTART_SAVE_PATH")
        assert val == ""


# ---------------------------------------------------------------------------
# Write with plugin (new addition)
# ---------------------------------------------------------------------------
class TestModelBuilderWriteWithPlugin:
    """write_with_plugin uses the built-in writer when plugin id is empty."""

    def test_write_with_plugin_empty_id(self, tmp_path):
        m = ModelBuilder()
        m.add_node("J1", NodeType.JUNCTION)
        m.add_node("OUT1", NodeType.OUTFALL)
        m.add_link("C1", LinkType.CONDUIT)
        m.set_link_nodes(0, 0, 1)
        out_path = str(tmp_path / "model_plugin.inp")
        m.write_with_plugin(out_path, "")
        assert os.path.exists(out_path)
        assert os.path.getsize(out_path) > 0

