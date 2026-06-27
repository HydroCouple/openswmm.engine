"""User-flag schema definitions ([USER_FLAGS]) and per-object values
([USER_FLAG_VALUES]).

Exercises the 7 schema-level C API functions added for GUI support
(``swmm_userflag_define`` / ``undefine`` / ``def_count`` / ``def_get`` /
``value_get`` / ``value_set`` / ``value_clear``) through both surfaces:

* ``ModelBuilder.define_userflag`` etc. (BUILDING state), and
* ``Solver.userflags`` view methods (OPENED+ states),

against the real handle-based engine (no mocks), per
``docs/API_GAP_CLOSURE_PLAN_2026-06-10.md`` Phase A.6.
"""

from __future__ import annotations

import pytest

pytest.importorskip("openswmm.engine._model")

from openswmm.engine import EngineError, ModelBuilder, UserFlagType


# ---------------------------------------------------------------------------
# Schema definitions on ModelBuilder
# ---------------------------------------------------------------------------
class TestBuilderDefinitions:
    def test_define_and_enumerate(self):
        m = ModelBuilder()
        assert m.userflag_def_count() == 0
        m.define_userflag("priority", UserFlagType.INTEGER, "Asset priority")
        m.define_userflag("inspected", UserFlagType.BOOLEAN)
        assert m.userflag_def_count() == 2
        # Names are stored uppercase; insertion order is preserved.
        name, type_, desc = m.get_userflag_def(0)
        assert name == "PRIORITY"
        assert type_ == UserFlagType.INTEGER
        assert desc == "Asset priority"
        name, type_, desc = m.get_userflag_def(1)
        assert name == "INSPECTED"
        assert type_ == UserFlagType.BOOLEAN
        assert desc == ""

    def test_redefine_overwrites_in_place(self):
        m = ModelBuilder()
        m.define_userflag("zone", UserFlagType.INTEGER, "old")
        m.define_userflag("ZONE", UserFlagType.STRING, "new")
        assert m.userflag_def_count() == 1
        _name, type_, desc = m.get_userflag_def(0)
        assert type_ == UserFlagType.STRING
        assert desc == "new"

    def test_undefine_removes_definition(self):
        m = ModelBuilder()
        m.define_userflag("temp", UserFlagType.REAL)
        m.undefine_userflag("temp")
        assert m.userflag_def_count() == 0

    def test_undefine_unknown_raises(self):
        m = ModelBuilder()
        with pytest.raises(EngineError):
            m.undefine_userflag("never_defined")

    def test_define_invalid_type_raises(self):
        m = ModelBuilder()
        with pytest.raises(EngineError):
            m.define_userflag("bad", 4)

    def test_def_get_out_of_range_raises(self):
        m = ModelBuilder()
        with pytest.raises(EngineError):
            m.get_userflag_def(0)


# ---------------------------------------------------------------------------
# Per-object values on ModelBuilder
# ---------------------------------------------------------------------------
class TestBuilderObjectValues:
    def _builder_with_node(self):
        m = ModelBuilder()
        m.add_node("J1", 0)
        return m

    def test_roundtrip_each_type(self):
        m = self._builder_with_node()
        m.define_userflag("flagged", UserFlagType.BOOLEAN)
        m.define_userflag("rank", UserFlagType.INTEGER)
        m.define_userflag("score", UserFlagType.REAL)
        m.define_userflag("note", UserFlagType.STRING)

        m.set_userflag_value("NODE", "J1", "flagged", "YES")
        m.set_userflag_value("NODE", "J1", "rank", "3")
        m.set_userflag_value("NODE", "J1", "score", "2.5")
        m.set_userflag_value("NODE", "J1", "note", "verify invert")

        # String form mirrors the INP encoding.
        assert m.get_userflag_value("NODE", "J1", "flagged") == "YES"
        assert m.get_userflag_value("NODE", "J1", "rank") == "3"
        assert m.get_userflag_value("NODE", "J1", "score") == "2.5"
        assert m.get_userflag_value("NODE", "J1", "note") == "verify invert"

    def test_boolean_parse_variants(self):
        m = self._builder_with_node()
        m.define_userflag("flagged", UserFlagType.BOOLEAN)
        for token in ("TRUE", "1", "yes"):
            m.set_userflag_value("NODE", "J1", "flagged", token)
            assert m.get_userflag_value("NODE", "J1", "flagged") == "YES"
        m.set_userflag_value("NODE", "J1", "flagged", "NO")
        assert m.get_userflag_value("NODE", "J1", "flagged") == "NO"

    def test_unassigned_returns_none(self):
        m = self._builder_with_node()
        m.define_userflag("rank", UserFlagType.INTEGER)
        assert m.get_userflag_value("NODE", "J1", "rank") is None

    def test_set_on_undefined_flag_raises(self):
        m = self._builder_with_node()
        with pytest.raises(EngineError):
            m.set_userflag_value("NODE", "J1", "ghost", "1")

    def test_bad_typed_parse_raises(self):
        m = self._builder_with_node()
        m.define_userflag("rank", UserFlagType.INTEGER)
        with pytest.raises(EngineError):
            m.set_userflag_value("NODE", "J1", "rank", "not_an_int")

    def test_clear_is_idempotent(self):
        m = self._builder_with_node()
        m.define_userflag("rank", UserFlagType.INTEGER)
        m.set_userflag_value("NODE", "J1", "rank", "7")
        m.clear_userflag_value("NODE", "J1", "rank")
        assert m.get_userflag_value("NODE", "J1", "rank") is None
        # Clearing an unassigned value also succeeds.
        m.clear_userflag_value("NODE", "J1", "rank")

    def test_undefine_drops_values(self):
        m = self._builder_with_node()
        m.define_userflag("rank", UserFlagType.INTEGER)
        m.set_userflag_value("NODE", "J1", "rank", "7")
        m.undefine_userflag("rank")
        # Re-defining yields a clean slate for value lookups.
        m.define_userflag("rank", UserFlagType.INTEGER)
        assert m.get_userflag_value("NODE", "J1", "rank") is None

    def test_redefine_keeps_values(self):
        m = self._builder_with_node()
        m.define_userflag("note", UserFlagType.STRING, "old")
        m.set_userflag_value("NODE", "J1", "note", "keep me")
        m.define_userflag("note", UserFlagType.STRING, "new description")
        assert m.get_userflag_value("NODE", "J1", "note") == "keep me"


# ---------------------------------------------------------------------------
# Solver.userflags view (schema + values + mapping interplay)
# ---------------------------------------------------------------------------
class TestSolverUserFlagsView:
    def test_define_enumerate_undefine(self, opened_solver):
        flags = opened_solver.userflags
        base = len(flags)
        flags.define("priority", UserFlagType.INTEGER, "Asset priority")
        assert len(flags) == base + 1
        defs = flags.definitions()
        entry = [d for d in defs if d.name == "PRIORITY"][0]
        assert entry.type == UserFlagType.INTEGER
        assert entry.description == "Asset priority"
        assert "PRIORITY" in iter(flags)
        flags.undefine("priority")
        assert len(flags) == base

    def test_scalar_setter_auto_defines(self, opened_solver):
        flags = opened_solver.userflags
        flags["CALIBRATED"] = True
        names = [d.name for d in flags.definitions()]
        assert "CALIBRATED" in names
        assert flags["CALIBRATED"] is True

    def test_string_scalar_roundtrip(self, opened_solver):
        flags = opened_solver.userflags
        flags["REVIEWER"] = "C. Buahin"
        assert flags["REVIEWER"] == "C. Buahin"
        entry = [d for d in flags.definitions() if d.name == "REVIEWER"][0]
        assert entry.type == UserFlagType.STRING

    def test_delitem_undefines(self, opened_solver):
        flags = opened_solver.userflags
        flags.define("scratch", UserFlagType.BOOLEAN)
        del flags["scratch"]
        assert "SCRATCH" not in [d.name for d in flags.definitions()]
        with pytest.raises(KeyError):
            del flags["scratch"]

    def test_object_values_roundtrip(self, opened_solver):
        flags = opened_solver.userflags
        flags.define("rank", UserFlagType.INTEGER)
        node_id = opened_solver.nodes[0].id
        flags.set_value("NODE", node_id, "rank", "5")
        assert flags.get_value("NODE", node_id, "rank") == "5"
        flags.clear_value("NODE", node_id, "rank")
        assert flags.get_value("NODE", node_id, "rank") is None
        flags.undefine("rank")


# ---------------------------------------------------------------------------
# INP persistence round-trip
# ---------------------------------------------------------------------------
class TestInpPersistence:
    def test_write_and_reparse(self, tmp_path):
        m = ModelBuilder()
        m.add_node("J1", 0)
        m.add_node("OUT1", 1)
        m.add_link("C1", 0)
        m.set_link_nodes(0, 0, 1)
        m.set_link_length(0, 300.0)
        m.set_link_roughness(0, 0.013)
        m.define_userflag("priority", UserFlagType.INTEGER, "Asset priority")
        m.define_userflag("note", UserFlagType.STRING)
        m.set_userflag_value("NODE", "J1", "priority", "2")
        m.set_userflag_value("LINK", "C1", "note", "lined 2024")

        inp = str(tmp_path / "userflags_roundtrip.inp")
        m.write(inp)
        text = open(inp).read()
        assert "PRIORITY" in text

        from openswmm.engine import Solver

        s = Solver(inp, str(tmp_path / "r.rpt"), str(tmp_path / "r.out"))
        s.open()
        try:
            flags = s.userflags
            names = [d.name for d in flags.definitions()]
            assert "PRIORITY" in names and "NOTE" in names
            assert flags.get_value("NODE", "J1", "priority") == "2"
            assert flags.get_value("LINK", "C1", "note") == "lined 2024"
        finally:
            s.close()
            s.destroy()
