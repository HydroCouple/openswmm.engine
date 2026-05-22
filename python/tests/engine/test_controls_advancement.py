"""Tests for the control-rule advancement in :class:`openswmm.engine.Controls`.

Covers the new ``get_id`` accessor (DA-ENG-02) which parses rule names
from the stored rule text server-side and returns ``None`` for malformed
rules so callers can render a sentinel display name without catching
exceptions.
"""

import pytest

from openswmm.engine import Controls, ModelBuilder


# ---------------------------------------------------------------------------
# Fixture: minimal in-memory model exposed via ModelBuilder
# ---------------------------------------------------------------------------
@pytest.fixture
def controls():
    """A :class:`Controls` accessor on a fresh ModelBuilder solver."""
    m = ModelBuilder()
    solver = m.to_solver()
    return Controls(solver)


# ---------------------------------------------------------------------------
# DA-ENG-02 — get_id (rule-name extraction)
# ---------------------------------------------------------------------------
class TestControlRuleNameExtraction:
    """``get_id`` returns the canonical name parsed from rule text."""

    def test_canonical_name(self, controls):
        controls.add_rule(
            "RULE PumpOnHigh\n"
            "IF NODE J1 DEPTH > 5\n"
            "THEN PUMP P1 STATUS = ON"
        )
        assert controls.get_id(0) == "PumpOnHigh"

    def test_lowercase_keyword(self, controls):
        controls.add_rule(
            "rule WeirBypass\n"
            "IF NODE J2 DEPTH < 1\n"
            "THEN WEIR W1 SETTING = 0"
        )
        controls.add_rule(
            "Rule TankFill\n"
            "IF NODE T1 DEPTH < 2\n"
            "THEN PUMP P2 STATUS = ON"
        )
        assert controls.get_id(0) == "WeirBypass"
        assert controls.get_id(1) == "TankFill"

    def test_leading_whitespace(self, controls):
        controls.add_rule(
            "  \t\nRULE OrificeClose\n"
            "IF NODE J3 DEPTH > 10\n"
            "THEN ORIFICE O1 SETTING = 0"
        )
        assert controls.get_id(0) == "OrificeClose"

    @pytest.mark.parametrize(
        "malformed_text",
        [
            # No RULE keyword at all.
            "IF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON",
            # RULE keyword with no following name token.
            "RULE\n",
            # RULE keyword as part of a longer word ("RULES").
            "RULES are not RULE\n",
            # Just whitespace.
            "   \n\t\n",
        ],
    )
    def test_malformed_rule_returns_none(self, controls, malformed_text):
        """get_id returns None (not raises) so callers can fall back to
        a sentinel name like ``Rule N [unnamed]``."""
        controls.add_rule(malformed_text)
        idx = controls.count() - 1
        assert controls.get_id(idx) is None

    def test_bad_index_raises(self, controls):
        controls.add_rule(
            "RULE R1\nIF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON"
        )
        with pytest.raises(RuntimeError):
            controls.get_id(1)
        with pytest.raises(RuntimeError):
            controls.get_id(-1)

    def test_get_id_independent_of_clear(self, controls):
        controls.add_rule(
            "RULE R1\nIF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON"
        )
        assert controls.get_id(0) == "R1"
        controls.clear_rules()
        assert controls.count() == 0
        with pytest.raises(RuntimeError):
            controls.get_id(0)
