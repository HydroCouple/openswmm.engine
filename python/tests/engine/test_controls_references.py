"""``Controls.find_references`` and ``Controls.remove_rule`` — new bindings for
``swmm_control_find_references`` / ``swmm_control_remove_rule``.

Rules are stored as text and parsed server-side, so ``find_references`` is a
purely textual scan (it does not require the referenced objects to exist in
the model). ``remove_rule`` binds the direct C removal path used by
``del controls[i]``.
"""

import unittest

from openswmm.engine import ModelBuilder


def _controls():
    """A :class:`Controls` view on a fresh ModelBuilder solver."""
    m = ModelBuilder()
    solver = m.to_solver()
    return solver.controls


def _seed(controls):
    """Append three rules referencing distinct objects; return the view."""
    controls.append(
        "RULE R1\nIF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON"
    )
    controls.append(
        "RULE R2\nIF NODE J2 DEPTH < 1\nTHEN WEIR W1 SETTING = 0"
    )
    controls.append(
        "RULE R3\nIF NODE J1 DEPTH > 2\nTHEN ORIFICE O1 SETTING = 0"
    )
    return controls


class TestFindReferences(unittest.TestCase):
    def test_single_match(self):
        controls = _seed(_controls())
        # P1 is referenced only by R1 (index 0).
        self.assertEqual(controls.find_references("P1"), [0])

    def test_multiple_matches_ascending(self):
        controls = _seed(_controls())
        # J1 is referenced by R1 and R3 (indices 0 and 2), ascending.
        self.assertEqual(controls.find_references("J1"), [0, 2])

    def test_no_match_returns_empty(self):
        controls = _seed(_controls())
        self.assertEqual(controls.find_references("DOES_NOT_EXIST"), [])

    def test_result_is_list_of_int(self):
        controls = _seed(_controls())
        refs = controls.find_references("J1")
        self.assertIsInstance(refs, list)
        self.assertTrue(all(isinstance(i, int) for i in refs))


class TestRemoveRule(unittest.TestCase):
    def test_remove_shifts_indices_down(self):
        controls = _seed(_controls())
        self.assertEqual(len(controls), 3)
        controls.remove_rule(0)
        self.assertEqual(len(controls), 2)
        # R2 is now at index 0, R3 at index 1.
        self.assertEqual(controls[0].id, "R2")
        self.assertEqual(controls[1].id, "R3")

    def test_remove_negative_index(self):
        controls = _seed(_controls())
        controls.remove_rule(-1)  # removes R3
        self.assertEqual(len(controls), 2)
        self.assertEqual(controls[-1].id, "R2")

    def test_remove_out_of_range_raises(self):
        controls = _seed(_controls())
        with self.assertRaises(IndexError):
            controls.remove_rule(99)

    def test_delitem_uses_direct_removal(self):
        controls = _seed(_controls())
        del controls[1]  # removes R2 via swmm_control_remove_rule
        self.assertEqual(len(controls), 2)
        self.assertEqual([controls[i].id for i in range(2)], ["R1", "R3"])


if __name__ == "__main__":
    unittest.main()
