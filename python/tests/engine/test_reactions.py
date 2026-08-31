"""Round-trips, refusals and text-tab contracts for ``solver.reactions``.

Covers:
  * ``Reactions.hydraulic_variables()`` / ``Reactions.functions()`` — the
    **engine-less statics**: exercised with no ``Solver`` constructed at all,
    and asserted against the documented vocabulary (``D Q U RE US FF AV HRT
    DT``; ``EXP``..``TAN`` arity 1, ``MIN`` ``MAX`` ``POW`` arity 2).
  * ``reactions.species`` / ``.coefficients`` / ``.terms`` — the container
    protocol (``len``, iteration, index and name lookup, ``__contains__``,
    ``get_index``, ``repr``) and every setter round-tripped against its
    getter.
  * ``reactions.initial`` — the per-node/per-link initial-quality rows.
  * ``reactions.get_option`` / ``set_option`` — the ``[REACTION_OPTIONS]``
    tokens.
  * ``reactions.validate`` — compile-only, never mutating.
  * The documented refusals: removing a still-referenced species,
    coefficient or term; a malformed expression reported by ``validate``;
    ``check_text`` on bad text leaving state byte-identical; and a failed
    ``apply_text`` leaving state byte-identical.
  * The D-RC6 invariant: ``serialize() -> apply_text() -> serialize()`` is
    byte-identical.

The model is authored inline because ``site_drainage_model.inp`` carries no
``[REACTION_*]`` sections. The ``.inp``, its ``.rxn`` component config and
every report/output file land in ``tests/engine/output`` so a user can
review them after a run.

@author: Caleb Buahin
"""

from __future__ import annotations

import os
import unittest

from openswmm.engine import (
    BadParamError,
    ReactionCoefficient,
    ReactionExprForm,
    ReactionFunction,
    ReactionHydVar,
    ReactionInitialEntry,
    ReactionScope,
    ReactionSpecies,
    ReactionTerm,
    Reactions,
)

from tests.engine._solver_cases import EngineSolverCase

_OUT_DIR = os.path.join(os.path.dirname(__file__), "output")

#: ``kComponentId`` in ``src/engine/transport/components/ReactionModule/
#: ReactionsComponent.cpp``.
_RXN_COMPONENT = "org.hydrocouple.openswmm.reactions"

#: Text that parses as ``.rxn`` sections but cannot COMPILE — the reference
#: to ``NO_SUCH_COEF`` resolves against nothing.
_UNCOMPILABLE_RXN = (
    "[REACTION_OPTIONS]\n"
    "RATE_UNITS SEC\n"
    "[REACTION_SPECIES]\n"
    "BULK Z MG\n"
    "[REACTION_PIPES]\n"
    "RATE Z -NO_SUCH_COEF * Z\n"
)


def _reactions_model(tag):
    """Write a reaction-enabled ``.inp`` plus its ``.rxn`` config.

    Section spellings are the engine's own — ``[REACTION_OPTIONS]``,
    ``[REACTION_SPECIES]``, ``[REACTION_COEFFICIENTS]``,
    ``[REACTION_TERMS]``, ``[REACTION_PIPES]``, ``[REACTION_TANKS]`` and
    ``[REACTION_QUALITY]`` (``ReactionsComponent.cpp`` parses them,
    ``ReactionsWriter.cpp`` emits them) — and the component is bound through
    ``[PROCESS_COMPONENTS] <id> config="…"``.

    The system is wired so that every "remove while referenced" refusal has
    a referent: ``X`` is referenced by ``Y``'s pipe expression, ``Kb`` by
    both, and the term ``Kf`` by ``X``'s pipe expression.
    """
    os.makedirs(_OUT_DIR, exist_ok=True)
    rxn_name = f"reactions_{tag}.rxn"

    with open(os.path.join(_OUT_DIR, rxn_name), "w") as f:
        f.write(
            "[REACTION_OPTIONS]\n"
            "RATE_UNITS SEC\n"
            "[REACTION_SPECIES]\n"
            "BULK X MG\n"
            "BULK Y MG\n"
            "[REACTION_COEFFICIENTS]\n"
            "CONSTANT Kb 0.05\n"
            "[REACTION_TERMS]\n"
            "Kf 0.5 * RE / D\n"
            "[REACTION_PIPES]\n"
            "RATE X -Kb * Kf * X\n"
            "RATE Y -Kb * X\n"
            "[REACTION_TANKS]\n"
            "RATE X -Kb * X\n"
            "RATE Y 0\n"
            "[REACTION_QUALITY]\n"
            "GLOBAL X 8\n"
        )

    text = (
        "[TITLE]\n"
        "Python reactions-binding round-trip deck\n"
        "\n[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "QUALITY_SOLVER       EULERIAN_ARD\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             01:00:00\n"
        "ROUTING_STEP         5\n"
        "REPORT_STEP          00:05:00\n"
        "\n[JUNCTIONS]\n"
        ";;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
        "J0  10.0  10  1.5  0  0\n"
        "J1   9.4  10  1.5  0  0\n"
        "\n[OUTFALLS]\n"
        "OUT  7.0  FREE  NO\n"
        "\n[CONDUITS]\n"
        "C1  J0  J1   500  0.013  0  0  0\n"
        "C2  J1  OUT  500  0.013  0  0  0\n"
        "\n[XSECTIONS]\n"
        "C1  CIRCULAR  2.0  0  0  0\n"
        "C2  CIRCULAR  2.0  0  0  0\n"
        "\n[INFLOWS]\n"
        'J0  FLOW  ""  FLOW  1.0  1.0  5\n'
        "\n[POLLUTANTS]\n"
        ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit\n"
        "TSS   MG/L  0  0  0  0  NO  *  0  0  10\n"
        "\n[PROCESS_COMPONENTS]\n"
        f'{_RXN_COMPONENT} config="{rxn_name}"\n'
        "\n[REPORT]\n"
        "INPUT NO\n"
    )
    path = os.path.join(_OUT_DIR, f"reactions_{tag}.inp")
    with open(path, "w") as f:
        f.write(text)
    return path


class _ReactionsCase(EngineSolverCase):
    """``EngineSolverCase`` bound to the inline reaction deck."""

    def solver_files(self):
        # Class AND method: several classes share a method name (e.g.
        # ``test_container_protocol``), and the method name alone would put
        # two tests on the same deck file.
        tag = "_".join(self.id().split(".")[-2:])
        inp = _reactions_model(tag)
        base = os.path.splitext(inp)[0]
        return inp, base + ".rpt", base + ".out"


# ---------------------------------------------------------------------------
# Engine-less statics — no Solver is constructed anywhere in this class
# ---------------------------------------------------------------------------
class TestReactionStatics(unittest.TestCase):
    """The discovery statics must work with **no engine at all**.

    These are the authoritative completer/highlighter vocabulary, and a GUI
    populates them before any model is loaded — so the contract under test is
    precisely that no ``Solver`` is needed. Nothing in this class builds one.
    """

    def test_hydraulic_variables_need_no_solver(self):
        hydvars = Reactions.hydraulic_variables()
        self.assertIsInstance(hydvars, tuple)
        self.assertTrue(hydvars)
        for hv in hydvars:
            self.assertIsInstance(hv, ReactionHydVar)
            self.assertTrue(hv.name)
            self.assertTrue(hv.description)

    def test_documented_hydraulic_variable_vocabulary(self):
        names = [hv.name for hv in Reactions.hydraulic_variables()]
        self.assertEqual(
            names,
            ["D", "Q", "U", "RE", "US", "FF", "AV", "HRT", "DT", "TEMP"])

    def test_functions_need_no_solver(self):
        functions = Reactions.functions()
        self.assertIsInstance(functions, tuple)
        self.assertTrue(functions)
        for fn in functions:
            self.assertIsInstance(fn, ReactionFunction)
            self.assertTrue(fn.name)
            self.assertIn(fn.arity, (1, 2))

    def test_documented_function_arities(self):
        arity = {fn.name: fn.arity for fn in Reactions.functions()}
        # EXP .. TAN take one argument.
        for name in ("EXP", "LOG", "LOG10", "SQRT", "ABS", "SGN", "STEP",
                     "SIN", "COS", "TAN"):
            with self.subTest(function=name):
                self.assertEqual(arity.get(name), 1)
        # MIN, MAX and POW take two.
        for name in ("MIN", "MAX", "POW"):
            with self.subTest(function=name):
                self.assertEqual(arity.get(name), 2)

    def test_statics_are_callable_on_the_class(self):
        # Not just on an instance: a GUI reaches them as `Reactions.foo()`.
        self.assertIs(type(Reactions.functions()), tuple)
        self.assertIs(type(Reactions.hydraulic_variables()), tuple)


# ---------------------------------------------------------------------------
# Species
# ---------------------------------------------------------------------------
class TestReactionSpecies(_ReactionsCase):

    def test_container_protocol(self):
        s = self.opened_solver()
        species = s.reactions.species
        self.assertEqual(len(species), 2)
        self.assertEqual([sp.name for sp in species], ["X", "Y"])
        self.assertIsInstance(species[0], ReactionSpecies)
        self.assertEqual(species["X"].index, 0)
        self.assertEqual(species["Y"].index, 1)
        self.assertEqual(species[-1].name, "Y")
        self.assertIn("X", species)
        self.assertNotIn("NOPE", species)
        self.assertEqual(species.get_index("Y"), 1)
        self.assertIsInstance(repr(species), str)
        self.assertIsInstance(repr(species[0]), str)

    def test_unknown_name_and_bad_index(self):
        s = self.opened_solver()
        species = s.reactions.species
        with self.assertRaises(KeyError):
            species["NOPE"]
        with self.assertRaises(IndexError):
            species[9]
        with self.assertRaises(TypeError):
            species.get_index(1.5)

    def test_attribute_reads(self):
        s = self.opened_solver()
        x = s.reactions.species["X"]
        self.assertEqual(x.name, "X")
        self.assertFalse(x.is_wall)
        self.assertEqual(x.units, "MG")
        self.assertIsInstance(x.atol, float)
        self.assertIsInstance(x.rtol, float)

    def test_global_initial_roundtrip(self):
        s = self.opened_solver()
        x = s.reactions.species["X"]
        self.assertAlmostEqual(x.initial, 8.0)
        x.initial = 2.5
        self.assertAlmostEqual(x.initial, 2.5)
        self.assertAlmostEqual(s.reactions.species["X"].initial, 2.5)

    def test_expression_roundtrip(self):
        s = self.opened_solver()
        x = s.reactions.species["X"]
        form, expr = x.get_expression(ReactionScope.PIPE)
        self.assertEqual(form, ReactionExprForm.RATE)
        self.assertTrue(expr)
        self.assertEqual(x.pipe_expression, (form, expr))

        x.set_expression(ReactionScope.PIPE, ReactionExprForm.RATE, "-Kb * X")
        self.assertEqual(x.get_expression(ReactionScope.PIPE),
                         (ReactionExprForm.RATE, "-Kb * X"))

        x.set_expression(ReactionScope.TANK, ReactionExprForm.FORMULA,
                         "Kb * 2")
        self.assertEqual(x.tank_expression,
                         (ReactionExprForm.FORMULA, "Kb * 2"))

    def test_expression_can_be_cleared(self):
        s = self.opened_solver()
        x = s.reactions.species["X"]
        x.set_expression(ReactionScope.TANK, ReactionExprForm.NONE)
        self.assertEqual(x.get_expression(ReactionScope.TANK),
                         (ReactionExprForm.NONE, ""))

    def test_uncompilable_expression_is_refused_and_rolled_back(self):
        s = self.opened_solver()
        x = s.reactions.species["X"]
        before = x.pipe_expression
        with self.assertRaises(BadParamError):
            x.set_expression(ReactionScope.PIPE, ReactionExprForm.RATE,
                             "-NO_SUCH_COEF * X")
        self.assertEqual(x.pipe_expression, before)

    def test_add_species(self):
        s = self.opened_solver()
        species = s.reactions.species
        z = species.add("Z", units="UG", atol=1e-6, rtol=1e-5)
        self.assertEqual(len(species), 3)
        self.assertEqual(z.name, "Z")
        self.assertEqual(z.index, 2)
        self.assertFalse(z.is_wall)
        self.assertEqual(z.units, "UG")
        self.assertAlmostEqual(z.atol, 1e-6)
        self.assertAlmostEqual(z.rtol, 1e-5)
        self.assertIn("Z", species)

    def test_add_wall_species(self):
        s = self.opened_solver()
        w = s.reactions.species.add("W", wall=True, units="MG")
        self.assertTrue(w.is_wall)

    def test_remove_unreferenced_species(self):
        s = self.opened_solver()
        species = s.reactions.species
        species.add("Z", units="MG")
        self.assertEqual(len(species), 3)
        species.remove("Z")
        self.assertEqual(len(species), 2)
        self.assertNotIn("Z", species)

    def test_remove_referenced_species_is_refused(self):
        """``X`` appears in ``Y``'s pipe expression — removal must be refused."""
        s = self.opened_solver()
        species = s.reactions.species
        before = len(species)
        with self.assertRaises(BadParamError):
            species.remove("X")
        # Refused, not partially applied.
        self.assertEqual(len(species), before)
        self.assertEqual([sp.name for sp in species], ["X", "Y"])


# ---------------------------------------------------------------------------
# Coefficients
# ---------------------------------------------------------------------------
class TestReactionCoefficients(_ReactionsCase):

    def test_container_protocol(self):
        s = self.opened_solver()
        coeffs = s.reactions.coefficients
        self.assertEqual(len(coeffs), 1)
        self.assertIsInstance(coeffs[0], ReactionCoefficient)
        self.assertEqual(coeffs["Kb"].index, 0)
        self.assertIn("Kb", coeffs)
        self.assertNotIn("NOPE", coeffs)
        self.assertEqual(coeffs.get_index("Kb"), 0)
        self.assertIsInstance(repr(coeffs), str)
        self.assertIsInstance(repr(coeffs[0]), str)

    def test_attribute_reads_and_value_roundtrip(self):
        s = self.opened_solver()
        kb = s.reactions.coefficients["Kb"]
        self.assertEqual(kb.name, "Kb")
        self.assertFalse(kb.is_param)
        self.assertAlmostEqual(kb.value, 0.05)
        kb.value = 0.125
        self.assertAlmostEqual(kb.value, 0.125)
        self.assertAlmostEqual(s.reactions.coefficients["Kb"].value, 0.125)

    def test_add_constant_and_parameter(self):
        s = self.opened_solver()
        coeffs = s.reactions.coefficients
        kc = coeffs.add("Kc", value=1.75)
        self.assertEqual(kc.name, "Kc")
        self.assertFalse(kc.is_param)
        self.assertAlmostEqual(kc.value, 1.75)
        kp = coeffs.add("Kp", parameter=True, value=0.5)
        self.assertTrue(kp.is_param)
        self.assertAlmostEqual(kp.value, 0.5)
        self.assertEqual(len(coeffs), 3)

    def test_remove_unreferenced_coefficient(self):
        s = self.opened_solver()
        coeffs = s.reactions.coefficients
        coeffs.add("Kc", value=1.0)
        coeffs.remove("Kc")
        self.assertEqual(len(coeffs), 1)
        self.assertNotIn("Kc", coeffs)

    def test_remove_referenced_coefficient_is_refused(self):
        """``Kb`` is referenced by both species' expressions."""
        s = self.opened_solver()
        coeffs = s.reactions.coefficients
        with self.assertRaises(BadParamError):
            coeffs.remove("Kb")
        self.assertEqual(len(coeffs), 1)
        self.assertAlmostEqual(coeffs["Kb"].value, 0.05)


# ---------------------------------------------------------------------------
# Terms
# ---------------------------------------------------------------------------
class TestReactionTerms(_ReactionsCase):

    def test_container_protocol(self):
        s = self.opened_solver()
        terms = s.reactions.terms
        self.assertEqual(len(terms), 1)
        self.assertIsInstance(terms[0], ReactionTerm)
        self.assertEqual(terms["Kf"].index, 0)
        self.assertIn("Kf", terms)
        self.assertNotIn("NOPE", terms)
        self.assertEqual(terms.get_index("Kf"), 0)
        self.assertIsInstance(repr(terms), str)
        self.assertIsInstance(repr(terms[0]), str)

    def test_expression_roundtrip(self):
        s = self.opened_solver()
        kf = s.reactions.terms["Kf"]
        self.assertEqual(kf.name, "Kf")
        self.assertTrue(kf.expression)
        kf.expression = "0.25 * RE"
        self.assertEqual(kf.expression, "0.25 * RE")
        self.assertEqual(s.reactions.terms["Kf"].expression, "0.25 * RE")

    def test_add_term(self):
        s = self.opened_solver()
        terms = s.reactions.terms
        kg = terms.add("Kg", "2.0 * D")
        self.assertEqual(len(terms), 2)
        self.assertEqual(kg.name, "Kg")
        self.assertEqual(kg.index, 1)
        self.assertEqual(kg.expression, "2.0 * D")

    def test_uncompilable_term_is_refused(self):
        s = self.opened_solver()
        terms = s.reactions.terms
        with self.assertRaises(BadParamError):
            terms.add("Kbad", "NO_SUCH_NAME * 2")
        self.assertEqual(len(terms), 1)

    def test_remove_unreferenced_term(self):
        s = self.opened_solver()
        terms = s.reactions.terms
        terms.add("Kg", "2.0 * D")
        terms.remove("Kg")
        self.assertEqual(len(terms), 1)
        self.assertNotIn("Kg", terms)

    def test_remove_referenced_term_is_refused(self):
        """``Kf`` is referenced by ``X``'s pipe expression."""
        s = self.opened_solver()
        terms = s.reactions.terms
        with self.assertRaises(BadParamError):
            terms.remove("Kf")
        self.assertEqual(len(terms), 1)
        self.assertEqual(terms[0].name, "Kf")


# ---------------------------------------------------------------------------
# Per-element initial quality rows
# ---------------------------------------------------------------------------
class TestReactionInitialRows(_ReactionsCase):

    def test_empty_before_any_row_is_set(self):
        s = self.opened_solver()
        rows = s.reactions.initial
        self.assertEqual(len(rows), 0)
        self.assertEqual(list(rows), [])
        self.assertIsInstance(repr(rows), str)

    def test_set_and_read_back(self):
        s = self.opened_solver()
        rows = s.reactions.initial
        node = s.nodes["J1"].index
        rows.set(False, node, "X", 3.0)
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertIsInstance(row, ReactionInitialEntry)
        self.assertFalse(row.is_link)
        self.assertEqual(row.elem_index, node)
        self.assertEqual(row.species_index, s.reactions.species.get_index("X"))
        self.assertAlmostEqual(row.value, 3.0)
        self.assertEqual(rows[-1], row)

    def test_upsert_on_the_same_key(self):
        s = self.opened_solver()
        rows = s.reactions.initial
        node = s.nodes["J1"].index
        rows.set(False, node, "X", 3.0)
        rows.set(False, node, "X", 4.5)
        self.assertEqual(len(rows), 1)
        self.assertAlmostEqual(rows[0].value, 4.5)

    def test_link_rows(self):
        s = self.opened_solver()
        rows = s.reactions.initial
        link = s.links["C1"].index
        rows.set(True, link, "Y", 1.25)
        row = rows[-1]
        self.assertTrue(row.is_link)
        self.assertEqual(row.elem_index, link)
        self.assertAlmostEqual(row.value, 1.25)

    def test_negative_value_refused(self):
        s = self.opened_solver()
        rows = s.reactions.initial
        with self.assertRaises(BadParamError):
            rows.set(False, s.nodes["J1"].index, "X", -1.0)
        self.assertEqual(len(rows), 0)

    def test_unknown_species_raises_keyerror(self):
        s = self.opened_solver()
        with self.assertRaises(KeyError):
            s.reactions.initial.set(False, 0, "NOPE", 1.0)

    def test_remove_and_index_bounds(self):
        s = self.opened_solver()
        rows = s.reactions.initial
        rows.set(False, s.nodes["J0"].index, "X", 1.0)
        rows.set(False, s.nodes["J1"].index, "X", 2.0)
        self.assertEqual(len(rows), 2)
        second = rows[1]
        rows.remove(0)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0], second)
        with self.assertRaises(IndexError):
            rows[1]
        with self.assertRaises(IndexError):
            rows.remove(5)


# ---------------------------------------------------------------------------
# [REACTION_OPTIONS]
# ---------------------------------------------------------------------------
class TestReactionOptions(_ReactionsCase):

    def test_parsed_option(self):
        s = self.opened_solver()
        self.assertEqual(s.reactions.get_option("RATE_UNITS"), "SEC")

    def test_token_option_roundtrips(self):
        s = self.opened_solver()
        rxn = s.reactions
        for key, value in (("SOLVER", "RK5"), ("SOLVER", "ROS2"),
                           ("COUPLING", "FULL"), ("COUPLING", "NONE"),
                           ("RATE_UNITS", "HR"), ("RATE_UNITS", "DAY"),
                           ("AREA_UNITS", "M2"), ("AREA_UNITS", "CM2")):
            with self.subTest(key=key, value=value):
                rxn.set_option(key, value)
                self.assertEqual(rxn.get_option(key), value)

    def test_numeric_option_roundtrips(self):
        s = self.opened_solver()
        rxn = s.reactions
        for key, value in (("TIMESTEP", 30.0), ("ATOL", 0.001),
                           ("RTOL", 0.002), ("TEMPERATURE", 12.5)):
            with self.subTest(key=key):
                rxn.set_option(key, str(value))
                # Numeric options come back as text; compare numerically.
                self.assertAlmostEqual(float(rxn.get_option(key)), value)

    def test_unknown_key_refused(self):
        s = self.opened_solver()
        with self.assertRaises(BadParamError):
            s.reactions.get_option("NOT_AN_OPTION")
        with self.assertRaises(BadParamError):
            s.reactions.set_option("NOT_AN_OPTION", "1")

    def test_unknown_value_refused(self):
        s = self.opened_solver()
        rxn = s.reactions
        with self.assertRaises(BadParamError):
            rxn.set_option("SOLVER", "NOT_A_SOLVER")
        # Refused, not applied.
        self.assertNotEqual(rxn.get_option("SOLVER"), "NOT_A_SOLVER")


# ---------------------------------------------------------------------------
# validate()
# ---------------------------------------------------------------------------
class TestReactionValidate(_ReactionsCase):

    def test_valid_expression(self):
        s = self.opened_solver()
        diag = s.reactions.validate("-Kb * X", ReactionScope.PIPE)
        self.assertTrue(diag.valid)
        self.assertEqual(diag.message, "")

    def test_malformed_expression_is_invalid_with_a_message(self):
        s = self.opened_solver()
        for expr in ("-NOT_A_NAME * 2", "-Kb *", "* X", "( X"):
            with self.subTest(expr=expr):
                diag = s.reactions.validate(expr, ReactionScope.PIPE)
                self.assertFalse(diag.valid)
                self.assertTrue(diag.message,
                                "a refusal must carry a diagnostic")
                self.assertIsInstance(diag.column, int)

    def test_validate_never_mutates(self):
        s = self.opened_solver()
        rxn = s.reactions
        before = rxn.serialize()
        rxn.validate("-NOT_A_NAME * 2", ReactionScope.PIPE)
        rxn.validate("-Kb * X", ReactionScope.TANK)
        self.assertEqual(rxn.serialize(), before)

    def test_default_scope_is_pipe(self):
        s = self.opened_solver()
        self.assertTrue(s.reactions.validate("-Kb * X").valid)


# ---------------------------------------------------------------------------
# The whole-file text surface
# ---------------------------------------------------------------------------
class TestReactionText(_ReactionsCase):

    def test_serialize_is_non_empty(self):
        s = self.opened_solver()
        text = s.reactions.serialize()
        self.assertTrue(text)
        self.assertIn("[REACTION_SPECIES]", text)

    def test_serialize_apply_text_serialize_is_byte_identical(self):
        """D-RC6 — the invariant that makes a GUI text tab safe."""
        s = self.opened_solver()
        rxn = s.reactions
        first = rxn.serialize()
        self.assertTrue(rxn.check_text(first).valid)
        rxn.apply_text(first)
        self.assertEqual(rxn.serialize(), first)
        # And once more, to show it is a fixed point rather than luck.
        rxn.apply_text(first)
        self.assertEqual(rxn.serialize(), first)

    def test_check_text_accepts_good_text(self):
        s = self.opened_solver()
        diag = s.reactions.check_text(s.reactions.serialize())
        self.assertTrue(diag.valid)
        self.assertEqual(diag.message, "")

    def test_check_text_on_bad_text_does_not_mutate(self):
        s = self.opened_solver()
        rxn = s.reactions
        before = rxn.serialize()
        diag = rxn.check_text(_UNCOMPILABLE_RXN)
        self.assertFalse(diag.valid)
        self.assertTrue(diag.message)
        # Whole-file checks report no column.
        self.assertEqual(diag.column, -1)
        # Zero state change on failure.
        self.assertEqual(rxn.serialize(), before)

    def test_check_text_on_good_text_does_not_mutate_either(self):
        s = self.opened_solver()
        rxn = s.reactions
        before = rxn.serialize()
        rxn.check_text(_UNCOMPILABLE_RXN.replace("NO_SUCH_COEF * Z", "0"))
        self.assertEqual(rxn.serialize(), before)

    def test_failed_apply_text_leaves_state_byte_identical(self):
        s = self.opened_solver()
        rxn = s.reactions
        before = rxn.serialize()
        with self.assertRaises(BadParamError):
            rxn.apply_text(_UNCOMPILABLE_RXN)
        # Staged: on ANY error the previous system is byte-identical.
        self.assertEqual(rxn.serialize(), before)
        # The vocabulary survived too.
        self.assertEqual([sp.name for sp in rxn.species], ["X", "Y"])
        self.assertEqual([c.name for c in rxn.coefficients], ["Kb"])
        self.assertEqual([t.name for t in rxn.terms], ["Kf"])

    def test_apply_text_replaces_the_system(self):
        s = self.opened_solver()
        rxn = s.reactions
        rxn.apply_text(
            "[REACTION_OPTIONS]\n"
            "RATE_UNITS HR\n"
            "[REACTION_SPECIES]\n"
            "BULK P MG\n"
            "[REACTION_PIPES]\n"
            "RATE P 0\n"
        )
        self.assertEqual([sp.name for sp in rxn.species], ["P"])
        self.assertEqual(rxn.get_option("RATE_UNITS"), "HR")

    def test_save_writes_the_serialized_text(self):
        s = self.opened_solver()
        rxn = s.reactions
        out = os.path.join(_OUT_DIR, "reactions_saved.rxn")
        rxn.save(out)
        self.assertTrue(os.path.exists(out))
        with open(out) as f:
            self.assertIn("[REACTION_SPECIES]", f.read())

    def test_repr_does_not_raise(self):
        s = self.opened_solver()
        for view in (s.reactions, s.reactions.species,
                     s.reactions.coefficients, s.reactions.terms,
                     s.reactions.initial):
            with self.subTest(view=type(view).__name__):
                self.assertIsInstance(repr(view), str)


if __name__ == "__main__":
    unittest.main()
