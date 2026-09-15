"""Round-trips and documented refusals for ``solver.process_components``.

Covers:
  * The ``[PROCESS_COMPONENTS]`` registration table as a sequence — ``len``,
    iteration, ``__getitem__`` by index *and* by id, ``__contains__``,
    ``get_index``, ``repr``.
  * ``register`` — including the documented flow where the ``config=`` path
    **does not exist yet** (registration is legal; the path is read at the
    next open), so the new row's ``resolved`` is ``""``.
  * A deck-declared registration whose ``resolved`` IS filled in, because
    opening the model resolved it — the contrast that gives ``resolved``
    its meaning.
  * The documented refusals: a duplicate id, an out-of-range index, an
    unknown id, and a key that is neither ``int`` nor ``str`` (``bool``
    included, so ``components[True]`` cannot silently mean index 1).
  * ``remove`` shifting later registrations down.

The model is authored inline: ``site_drainage_model.inp`` has no
``[PROCESS_COMPONENTS]`` section at all. The ``.inp``, its ``.age`` sidecar
and every report/output file land in ``tests/engine/output`` so a user can
review them after a run.

@author: Caleb Buahin
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import BadParamError, ProcessComponent

from tests.engine._solver_cases import EngineSolverCase

_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")

#: ``kAgeId`` in ``src/engine/transport/components/WaterAgeModule/
#: WaterAgeComponent.cpp`` — a real, resolvable component so the deck row
#: has a non-empty ``resolved`` after open.
_AGE_COMPONENT = "org.hydrocouple.openswmm.waterage"


def _process_components_model(tag):
    """Write an ``.inp`` with one ``[PROCESS_COMPONENTS]`` row plus its config.

    The row spelling is ``<id> config="<path>"`` — the form
    ``InpWriter.cpp`` emits and ``SWMMEngine.cpp`` resolves at open. The
    ``config=`` path is relative, so it resolves against the ``.inp``'s own
    directory.
    """
    os.makedirs(_OUT_DIR, exist_ok=True)
    age_name = f"process_components_{tag}.age"
    with open(os.path.join(_OUT_DIR, age_name), "w") as f:
        f.write("[WATER_AGE_SOURCES]\nDWF GLOBAL 2.0\n")

    text = (
        "[TITLE]\n"
        "Python process-components binding deck\n"
        "\n[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "QUALITY_SOLVER       EULERIAN_ARD\n"
        "WATER_AGE            ON\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:30:00\n"
        "ROUTING_STEP         5\n"
        "REPORT_STEP          00:05:00\n"
        "\n[JUNCTIONS]\n"
        ";;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
        "J0  10.0  10  1.0  0  0\n"
        "J1   9.0  10  1.0  0  0\n"
        "\n[OUTFALLS]\n"
        "OUT  8.0  FREE  NO\n"
        "\n[CONDUITS]\n"
        "C1  J0  J1   400  0.013  0  0  0\n"
        "C2  J1  OUT  400  0.013  0  0  0\n"
        "\n[XSECTIONS]\n"
        "C1  CIRCULAR  1.5  0  0  0\n"
        "C2  CIRCULAR  1.5  0  0  0\n"
        "\n[PROCESS_COMPONENTS]\n"
        f'{_AGE_COMPONENT} config="{age_name}"\n'
        "\n[REPORT]\n"
        "INPUT NO\n"
    )
    path = os.path.join(_OUT_DIR, f"process_components_{tag}.inp")
    with open(path, "w") as f:
        f.write(text)
    return path


class _ProcessComponentsCase(EngineSolverCase):
    """``EngineSolverCase`` bound to the inline [PROCESS_COMPONENTS] deck."""

    def solver_files(self):
        inp = _process_components_model(self.tag())
        base = os.path.splitext(inp)[0]
        return inp, base + ".rpt", base + ".out"

    def tag(self):
        # Class AND method, so a method name reused by another class can
        # never put two tests on the same deck file.
        return "_".join(self.id().split(".")[-2:])

    def config_name(self):
        return f"process_components_{self.tag()}.age"


# ---------------------------------------------------------------------------
# Container protocol
# ---------------------------------------------------------------------------
class TestProcessComponentsContainer(_ProcessComponentsCase):

    def test_parsed_registration(self):
        s = self.opened_solver()
        pcs = s.process_components
        self.assertEqual(len(pcs), 1)
        row = pcs[0]
        self.assertIsInstance(row, ProcessComponent)
        self.assertEqual(row.component_index, 0)
        self.assertEqual(row.id, _AGE_COMPONENT)
        self.assertEqual(row.config, self.config_name())

    def test_lookup_by_id_matches_lookup_by_index(self):
        s = self.opened_solver()
        pcs = s.process_components
        self.assertEqual(pcs[_AGE_COMPONENT], pcs[0])
        self.assertEqual(pcs.get_index(_AGE_COMPONENT), 0)
        self.assertEqual(pcs[-1], pcs[0])

    def test_contains(self):
        s = self.opened_solver()
        pcs = s.process_components
        self.assertIn(_AGE_COMPONENT, pcs)
        self.assertNotIn("not.a.component", pcs)
        # A non-string is never a component id.
        self.assertNotIn(0, pcs)

    def test_iteration(self):
        s = self.opened_solver()
        rows = list(s.process_components)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].id, _AGE_COMPONENT)

    def test_repr_does_not_raise(self):
        s = self.opened_solver()
        self.assertIsInstance(repr(s.process_components), str)


# ---------------------------------------------------------------------------
# resolved
# ---------------------------------------------------------------------------
class TestProcessComponentsResolved(_ProcessComponentsCase):

    def test_deck_registration_is_resolved_by_the_open(self):
        s = self.opened_solver()
        row = s.process_components[_AGE_COMPONENT]
        self.assertTrue(
            row.resolved,
            "opening the model should record the path the config was read "
            "from")
        self.assertTrue(row.resolved.endswith(self.config_name()))

    def test_new_registration_has_no_resolved_path_yet(self):
        """`resolved` is empty until an open resolves it — including for a
        row registered after the open."""
        s = self.opened_solver()
        pcs = s.process_components
        row = pcs.register("test.component.fresh", "fresh_config.rxn")
        self.assertEqual(row.resolved, "")
        # And re-reading the row through the collection agrees.
        self.assertEqual(pcs["test.component.fresh"].resolved, "")


# ---------------------------------------------------------------------------
# register()
# ---------------------------------------------------------------------------
class TestProcessComponentsRegister(_ProcessComponentsCase):

    def test_register_appends_and_round_trips(self):
        s = self.opened_solver()
        pcs = s.process_components
        row = pcs.register("test.component.a", "component_a.cfg")
        self.assertEqual(len(pcs), 2)
        self.assertEqual(row.component_index, 1)
        self.assertEqual(row.id, "test.component.a")
        self.assertEqual(row.config, "component_a.cfg")
        self.assertIn("test.component.a", pcs)

    def test_register_against_a_nonexistent_config_path_is_accepted(self):
        """The "create component, then write its config" flow: registration
        first, file afterwards."""
        s = self.opened_solver()
        missing = os.path.join(_OUT_DIR, "definitely_not_written_yet.rxn")
        self.assertFalse(os.path.exists(missing))
        row = s.process_components.register("test.component.later", missing)
        self.assertEqual(row.config, missing)
        self.assertEqual(row.resolved, "")

    def test_register_without_a_config_path(self):
        s = self.opened_solver()
        row = s.process_components.register("test.component.bare")
        self.assertEqual(row.config, "")

    def test_duplicate_id_is_refused(self):
        s = self.opened_solver()
        pcs = s.process_components
        pcs.register("test.component.dup", "one.cfg")
        before = len(pcs)
        with self.assertRaises(BadParamError):
            pcs.register("test.component.dup", "two.cfg")
        # Refused, not overwritten: the count and the first config survive.
        self.assertEqual(len(pcs), before)
        self.assertEqual(pcs["test.component.dup"].config, "one.cfg")

    def test_duplicate_of_the_deck_registration_is_refused(self):
        s = self.opened_solver()
        pcs = s.process_components
        with self.assertRaises(BadParamError):
            pcs.register(_AGE_COMPONENT, "other.age")
        self.assertEqual(len(pcs), 1)


# ---------------------------------------------------------------------------
# remove()
# ---------------------------------------------------------------------------
class TestProcessComponentsRemove(_ProcessComponentsCase):

    def test_remove_by_id(self):
        s = self.opened_solver()
        pcs = s.process_components
        pcs.register("test.component.x", "x.cfg")
        self.assertEqual(len(pcs), 2)
        pcs.remove("test.component.x")
        self.assertEqual(len(pcs), 1)
        self.assertNotIn("test.component.x", pcs)

    def test_remove_by_index_shifts_later_rows_down(self):
        s = self.opened_solver()
        pcs = s.process_components
        pcs.register("test.component.x", "x.cfg")
        pcs.register("test.component.y", "y.cfg")
        self.assertEqual(len(pcs), 3)
        pcs.remove(0)
        self.assertEqual(len(pcs), 2)
        self.assertEqual([r.id for r in pcs],
                         ["test.component.x", "test.component.y"])
        # Indexes are re-derived, never stale.
        self.assertEqual(pcs["test.component.y"].component_index, 1)

    def test_remove_does_not_delete_the_config_file(self):
        s = self.opened_solver()
        cfg = os.path.join(_OUT_DIR, self.config_name())
        self.assertTrue(os.path.exists(cfg))
        s.process_components.remove(_AGE_COMPONENT)
        self.assertTrue(os.path.exists(cfg))


# ---------------------------------------------------------------------------
# Key handling
# ---------------------------------------------------------------------------
class TestProcessComponentsKeys(_ProcessComponentsCase):

    def test_unknown_id_raises_keyerror(self):
        s = self.opened_solver()
        pcs = s.process_components
        with self.assertRaises(KeyError):
            pcs["not.a.component"]
        with self.assertRaises(KeyError):
            pcs.get_index("not.a.component")
        with self.assertRaises(KeyError):
            pcs.remove("not.a.component")

    def test_out_of_range_index_raises_indexerror(self):
        s = self.opened_solver()
        with self.assertRaises(IndexError):
            s.process_components[99]
        with self.assertRaises(IndexError):
            s.process_components[-99]

    def test_bool_key_is_rejected(self):
        """``components[True]`` must not silently mean index 1."""
        s = self.opened_solver()
        with self.assertRaises(TypeError):
            s.process_components[True]

    def test_non_int_non_str_key_is_rejected(self):
        s = self.opened_solver()
        with self.assertRaises(TypeError):
            s.process_components[1.5]


if __name__ == "__main__":
    unittest.main()
