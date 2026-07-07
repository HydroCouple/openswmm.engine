"""Tests for control-rule name extraction (DA-ENG-02).

The engine parses a rule's ``RULE <name>`` header server-side so callers
get a stable ``id`` field without re-parsing the rule text. In v1 the
controls collection yields :class:`ControlRule` named-tuples where
``.id`` is the extracted name (empty string when malformed).

Migrated to the v1 Pythonic bindings.
"""

import pytest

from openswmm.engine import ModelBuilder
from openswmm.engine._exceptions import BadParamError


@pytest.fixture
def controls():
    """A :class:`Controls` view on a fresh ModelBuilder solver."""
    m = ModelBuilder()
    solver = m.to_solver()
    return solver.controls


class TestControlRuleNameExtraction:
    """``controls[i].id`` returns the canonical name parsed from rule text."""

    def test_canonical_name(self, controls):
        controls.append(
            "RULE PumpOnHigh\n"
            "IF NODE J1 DEPTH > 5\n"
            "THEN PUMP P1 STATUS = ON"
        )
        assert controls[0].id == "PumpOnHigh"

    def test_lowercase_keyword(self, controls):
        controls.append(
            "rule WeirBypass\n"
            "IF NODE J2 DEPTH < 1\n"
            "THEN WEIR W1 SETTING = 0"
        )
        controls.append(
            "Rule TankFill\n"
            "IF NODE T1 DEPTH < 2\n"
            "THEN PUMP P2 STATUS = ON"
        )
        assert controls[0].id == "WeirBypass"
        assert controls[1].id == "TankFill"

    def test_leading_whitespace(self, controls):
        controls.append(
            "  \t\nRULE OrificeClose\n"
            "IF NODE J3 DEPTH > 10\n"
            "THEN ORIFICE O1 SETTING = 0"
        )
        assert controls[0].id == "OrificeClose"

    @pytest.mark.parametrize(
        "malformed_text",
        [
            "IF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON",
            "RULE\n",
            "RULES are not RULE\n",
            "   \n\t\n",
        ],
    )
    def test_malformed_rule_returns_empty_id(self, controls, malformed_text):
        """In the a2 API the engine cannot extract a name from a malformed
        rule, so ``swmm_control_get_id`` returns ``SWMM_ERR_BADPARAM`` and
        indexing the rule raises :class:`BadParamError` (was an empty-string
        id in v1, ``None`` in v0). The rule text is still stored — the append
        succeeds and the count increments — but the ``id`` field is
        unavailable. Callers must catch this to fall back to a sentinel
        display name."""
        controls.append(malformed_text)
        idx = len(controls) - 1
        # The malformed rule is stored (count incremented) ...
        assert idx == 0
        # ... but its name cannot be parsed, so ``.id`` access raises.
        with pytest.raises(BadParamError):
            _ = controls[idx].id

    def test_bad_index_raises(self, controls):
        controls.append(
            "RULE R1\nIF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON"
        )
        with pytest.raises(IndexError):
            _ = controls[1]
        with pytest.raises(IndexError):
            # Negative-index wrap reaches -1 which still resolves out of range
            # for a single-element collection.
            _ = controls[-2]

    def test_get_id_independent_of_clear(self, controls):
        controls.append(
            "RULE R1\nIF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON"
        )
        assert controls[0].id == "R1"
        controls.clear()
        assert len(controls) == 0
        with pytest.raises(IndexError):
            _ = controls[0]
