# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Reaction system (Pythonic v1 surface)
=====================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

``solver.reactions`` exposes the ``[REACTION_*]`` / ``.rxn`` surface: the
multi-species reaction vocabulary (species, coefficients, intermediate
terms), the per-species PIPE/TANK expressions, initial quality, the
``[REACTION_OPTIONS]`` tokens, and the whole-file text tab contract
(``serialize`` / ``check_text`` / ``apply_text`` / ``save``).

Two contracts govern everything here and are worth reading once:

**Eager validation with rollback.** Every mutator recompiles the whole
reaction system before it returns. A mutation that would leave *any*
expression uncompilable is rolled back and refused with a
:class:`~openswmm.engine.BadParamError` — the engine can never be left
holding an uncompilable model. Removal of a species, coefficient, or term
that is still referenced by a compiled expression is refused for the same
reason. All of these mutations are ``BUILDING``/``OPENED`` only; reaction
rows seed at simulation start, so they cannot be edited on a running model.

**Byte-identical text round trip.** ``serialize() -> apply_text() ->
serialize()`` is byte-identical, which is what makes a GUI text tab safe:
the user's edits either apply whole or leave the previous system untouched
(``apply_text`` is staged — on *any* error the previous reaction state and
registry block are byte-identical to before the call).

The discovery getters are the AUTHORITATIVE completer/highlighter
vocabulary. A GUI or agent must *enumerate* species, coefficients, terms,
hydraulic variables, and functions from here rather than hard-coding lists,
so vocabulary drift is structural rather than disciplinary.
:meth:`Reactions.hydraulic_variables` and :meth:`Reactions.functions` are
engine-less statics — they need no open model, and they read the expression
compiler's own tables, so there is exactly one copy of the truth.

.. code-block:: python

    from openswmm.engine import (
        Solver, Reactions, ReactionScope, ReactionExprForm)

    # Completer vocabulary — no model, no engine handle required.
    for hv in Reactions.hydraulic_variables():
        print(hv.name, "-", hv.description)
    for fn in Reactions.functions():
        print(fn.name, "arity", fn.arity)

    s = Solver("model.inp")
    s.open()                                  # BUILDING/OPENED: edits allowed

    rxn = s.reactions
    nh3 = rxn.species.add("NH3", units="MG", atol=1e-6, rtol=1e-6)
    rxn.coefficients.add("Kb", value=0.05)
    rxn.terms.add("Kf", "1.5826e-4 * RE^0.88 / D")

    nh3.set_expression(ReactionScope.PIPE, ReactionExprForm.RATE, "-Kb * NH3")
    nh3.initial = 0.5

    diag = rxn.validate("-Kb * NH3", ReactionScope.PIPE)
    if not diag.valid:
        print(diag.message, "at column", diag.column)

    text = rxn.serialize()
    assert rxn.check_text(text).valid
    rxn.apply_text(text)                      # byte-identical round trip
    s.close()
"""

# cython: language_level=3

from ._exceptions import ElementNotFoundError, raise_for_code
from collections.abc import Iterator
from typing import NamedTuple, Tuple

from ._common cimport *
from ._enums import ReactionExprForm, ReactionScope


cdef inline SWMM_Engine _h(solver):
    return <SWMM_Engine><size_t>solver.handle


# =============================================================================
# Flat snapshots (typing.NamedTuple — cheap, immutable, tuple-compatible)
# =============================================================================

class ReactionHydVar(NamedTuple):
    """One built-in hydraulic variable usable inside a reaction expression.

    :ivar name: Variable token as written in an expression (e.g. ``"RE"``).
    :ivar description: Human-readable meaning, for tooltips/completers.
    """

    name: str
    description: str


class ReactionFunction(NamedTuple):
    """One built-in math function usable inside a reaction expression.

    :ivar name: Function token (e.g. ``"EXP"``, ``"POW"``).
    :ivar arity: Number of arguments the function takes.
    """

    name: str
    arity: int


class ReactionInitialEntry(NamedTuple):
    """One NODE/LINK initial-quality row.

    :ivar is_link: ``True`` when ``elem_index`` addresses a link, ``False``
        when it addresses a node.
    :ivar elem_index: Zero-based node or link index.
    :ivar species_index: Zero-based reaction-species index.
    :ivar value: Initial concentration, in the species' own units.
    """

    is_link: bool
    elem_index: int
    species_index: int
    value: float


class ExpressionDiagnostic(NamedTuple):
    """Result of a compile-only expression or whole-file check.

    :ivar valid: ``True`` when the text compiles against this model.
    :ivar message: Engine diagnostic; empty string when ``valid``.
    :ivar column: 1-based error column, or ``-1`` when the failure is not
        attributable to a column (and always ``-1`` for whole-file checks,
        which report no column).
    """

    valid: bool
    message: str
    column: int


# =============================================================================
# Row readers — one fresh C call each, shared by the live views
# =============================================================================

def _species_row(solver, int idx):
    """Read species *idx* as ``(name, is_wall, units, atol, rtol)``."""
    cdef char name[128]
    cdef char units[128]
    cdef int is_wall = 0
    cdef double atol = 0.0
    cdef double rtol = 0.0
    name[0] = 0
    units[0] = 0
    _check(swmm_reaction_species_get(
        _h(solver), idx, name, 128, &is_wall, units, 128, &atol, &rtol))
    return (name.decode('utf-8'), is_wall != 0, units.decode('utf-8'),
            atol, rtol)


def _coeff_row(solver, int idx):
    """Read coefficient *idx* as ``(name, is_param, value)``."""
    cdef char name[128]
    cdef int is_param = 0
    cdef double value = 0.0
    name[0] = 0
    _check(swmm_reaction_coeff_get(_h(solver), idx, name, 128, &is_param, &value))
    return (name.decode('utf-8'), is_param != 0, value)


def _term_row(solver, int idx):
    """Read term *idx* as ``(name, expression)``."""
    cdef char name[128]
    cdef char expr[1024]
    name[0] = 0
    expr[0] = 0
    _check(swmm_reaction_term_get(_h(solver), idx, name, 128, expr, 1024))
    return (name.decode('utf-8'), expr.decode('utf-8'))


def _species_count(solver) -> int:
    return swmm_reaction_species_count(_h(solver))


def _coeff_count(solver) -> int:
    return swmm_reaction_coeff_count(_h(solver))


def _term_count(solver) -> int:
    return swmm_reaction_term_count(_h(solver))


def _resolve_row(solver, key, count_fn, row_fn, str label) -> int:
    """Resolve ``int | str`` *key* to a validated row index.

    There is no ``swmm_reaction_*_index`` C helper, so a string key is
    resolved by an **O(n) linear scan** over the collection, comparing the
    name of each row exactly (case-sensitively). The collections are small
    (tens of entries at most), so this is cheap in practice — but a caller
    resolving the same name in a tight loop should hoist the index out.

    :param key: Zero-based index (negatives count from the end) or exact name.
    :returns: A validated index in ``[0, count)``.
    :raises ElementNotFoundError: *key* is a string and no row has that name.
    :raises IndexError: *key* is an out-of-range integer.
    :raises TypeError: *key* is neither ``int`` nor ``str``.
    """
    cdef int n = count_fn(solver)
    cdef int i
    if isinstance(key, str):
        for i in range(n):
            if row_fn(solver, i)[0] == key:
                return i
        raise ElementNotFoundError(key, f"{label} '{key}' not found")
    if not isinstance(key, int):
        raise TypeError(
            f"{label} key must be an int index or a str name, got "
            f"{type(key).__name__}")
    i = key
    if i < 0:
        i += n
    if not (0 <= i < n):
        raise IndexError(f"{label} index out of range: {key}")
    return i


# =============================================================================
# Live row views — every property access is one fresh C call, no snapshots
# =============================================================================

class ReactionSpecies:
    """One declared reaction species. Reach via ``solver.reactions.species[key]``.

    This is a **live view** over ``(solver, index)``: every attribute read
    issues a fresh C call, so the object never holds stale data. It does hold
    a positional index, though — a structural mutation (species add/remove,
    :meth:`Reactions.apply_text`) shifts indices, so re-look-up by name after
    one rather than reusing an old view.
    """

    def __init__(self, solver, int index):
        self._solver = solver
        self._index = index

    @property
    def index(self) -> int:
        """Zero-based position in ``[REACTION_SPECIES]``."""
        return self._index

    @property
    def name(self) -> str:
        """Species name as referenced from expressions."""
        return _species_row(self._solver, self._index)[0]

    @property
    def is_wall(self) -> bool:
        """``True`` for a WALL species, ``False`` for a BULK species."""
        return _species_row(self._solver, self._index)[1]

    @property
    def units(self) -> str:
        """Concentration units token (e.g. ``"MG"``, ``"UG"``, ``"#"``)."""
        return _species_row(self._solver, self._index)[2]

    @property
    def atol(self) -> float:
        """Absolute solver tolerance; ``0.0`` means "use the global ATOL"."""
        return _species_row(self._solver, self._index)[3]

    @property
    def rtol(self) -> float:
        """Relative solver tolerance; ``0.0`` means "use the global RTOL"."""
        return _species_row(self._solver, self._index)[4]

    # ------------------------------------------------------------------
    # Expressions
    # ------------------------------------------------------------------

    def get_expression(self, scope) -> Tuple[object, str]:
        """Return this species' expression in one scope.

        :param scope: :class:`~openswmm.engine.ReactionScope` ``PIPE`` or
            ``TANK`` (species expressions exist only in those two scopes).
        :returns: ``(form, expression)`` where *form* is a
            :class:`~openswmm.engine.ReactionExprForm`. ``form`` is ``NONE``
            and ``expression`` is ``""`` when the species has no expression
            in that scope.
        :raises BadParamError: *scope* is not a species expression scope.
        """
        cdef int form = 0
        cdef char expr[1024]
        expr[0] = 0
        _check(swmm_reaction_expr_get(
            _h(self._solver), int(scope), self._index, &form, expr, 1024))
        return (ReactionExprForm(form), expr.decode('utf-8'))

    def set_expression(self, scope, form, str expression="") -> None:
        """Set — or with ``ReactionExprForm.NONE`` clear — one scope's expression.

        ``BUILDING``/``OPENED`` only. Validated eagerly: the whole reaction
        system is recompiled before this returns, and if the new expression
        (or anything it would invalidate) fails to compile the change is
        rolled back and refused.

        :param scope: :class:`~openswmm.engine.ReactionScope` ``PIPE`` or ``TANK``.
        :param form: :class:`~openswmm.engine.ReactionExprForm` — ``RATE``
            (``dC/dt = f(...)``), ``EQUIL`` (``0 = f(...)``), ``FORMULA``
            (``C = f(...)``), or ``NONE`` to clear the scope.
        :param expression: Expression text; ignored when *form* is ``NONE``.
        :raises BadParamError: The expression does not compile (nothing changed).
        :raises LifecycleError: The engine is past ``OPENED``.
        """
        cdef bytes b = expression.encode('utf-8')
        _check(swmm_reaction_expr_set(
            _h(self._solver), int(scope), self._index, int(form), b))

    @property
    def pipe_expression(self) -> Tuple[object, str]:
        """``(form, expression)`` for the conduit (flowing) scope."""
        return self.get_expression(ReactionScope.PIPE)

    @property
    def tank_expression(self) -> Tuple[object, str]:
        """``(form, expression)`` for the storage-unit (mixed) scope."""
        return self.get_expression(ReactionScope.TANK)

    # ------------------------------------------------------------------
    # Global initial quality
    # ------------------------------------------------------------------

    @property
    def initial(self) -> float:
        """Model-wide initial concentration, in this species' own units.

        Per-node / per-link overrides live in ``solver.reactions.initial``.
        """
        cdef double v = 0.0
        _check(swmm_reaction_init_global_get(_h(self._solver), self._index, &v))
        return v

    @initial.setter
    def initial(self, double value) -> None:
        """Set the model-wide initial concentration (species units, ``>= 0``).

        ``BUILDING``/``OPENED`` only.
        """
        _check(swmm_reaction_init_global_set(_h(self._solver), self._index, value))

    def __repr__(self) -> str:
        try:
            name, is_wall, units, atol, rtol = _species_row(
                self._solver, self._index)
            kind = "WALL" if is_wall else "BULK"
            return (f"<ReactionSpecies name={name!r} index={self._index} "
                    f"{kind} units={units!r}>")
        except Exception:
            return f"<ReactionSpecies index={self._index} (unavailable)>"


class ReactionCoefficient:
    """One ``[REACTION_COEFFICIENTS]`` entry — a CONSTANT or a PARAMETER.

    A **live view** over ``(solver, index)``; every read is a fresh C call.
    """

    def __init__(self, solver, int index):
        self._solver = solver
        self._index = index

    @property
    def index(self) -> int:
        """Zero-based position in ``[REACTION_COEFFICIENTS]``."""
        return self._index

    @property
    def name(self) -> str:
        """Coefficient name as referenced from expressions."""
        return _coeff_row(self._solver, self._index)[0]

    @property
    def is_param(self) -> bool:
        """``True`` for a PARAMETER (pipe/tank-overridable), ``False`` for a
        CONSTANT."""
        return _coeff_row(self._solver, self._index)[1]

    @property
    def value(self) -> float:
        """Current numeric value, in whatever units the expressions imply."""
        return _coeff_row(self._solver, self._index)[2]

    @value.setter
    def value(self, double v) -> None:
        """Change the coefficient's value.

        ``BUILDING``/``OPENED`` only. Validated eagerly and rolled back if the
        system stops compiling.
        """
        _check(swmm_reaction_coeff_set_value(_h(self._solver), self._index, v))

    def __repr__(self) -> str:
        try:
            name, is_param, value = _coeff_row(self._solver, self._index)
            kind = "PARAMETER" if is_param else "CONSTANT"
            return (f"<ReactionCoefficient name={name!r} index={self._index} "
                    f"{kind} value={value!r}>")
        except Exception:
            return f"<ReactionCoefficient index={self._index} (unavailable)>"


class ReactionTerm:
    """One ``[REACTION_TERMS]`` entry — a named intermediate expression.

    A **live view** over ``(solver, index)``; every read is a fresh C call.

    Terms are ordered, and a term may only reference terms declared *before*
    it. That forward-only rule is enforced when a whole file is applied (where
    ordinal position exists), not by :meth:`Reactions.validate` in
    ``ReactionScope.TERM``.
    """

    def __init__(self, solver, int index):
        self._solver = solver
        self._index = index

    @property
    def index(self) -> int:
        """Zero-based position in ``[REACTION_TERMS]`` — its ordering rank."""
        return self._index

    @property
    def name(self) -> str:
        """Term name as referenced from later terms and species expressions."""
        return _term_row(self._solver, self._index)[0]

    @property
    def expression(self) -> str:
        """The term's source expression text."""
        return _term_row(self._solver, self._index)[1]

    @expression.setter
    def expression(self, str expr) -> None:
        """Replace this term's expression.

        ``BUILDING``/``OPENED`` only. Validated eagerly: if the replacement
        breaks this term or anything downstream of it, the whole edit is
        rolled back and refused.
        """
        cdef bytes b = expr.encode('utf-8')
        _check(swmm_reaction_term_set_expr(_h(self._solver), self._index, b))

    def __repr__(self) -> str:
        try:
            name, expr = _term_row(self._solver, self._index)
            return (f"<ReactionTerm name={name!r} index={self._index} "
                    f"expression={expr!r}>")
        except Exception:
            return f"<ReactionTerm index={self._index} (unavailable)>"


# =============================================================================
# Collections
# =============================================================================

class _SpeciesCollection:
    """``solver.reactions.species`` — collection of :class:`ReactionSpecies`.

    This is the authoritative species vocabulary for a completer; enumerate it
    rather than hard-coding names.
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        return _species_count(self._solver)

    def __iter__(self) -> Iterator[ReactionSpecies]:
        cdef int n = _species_count(self._solver)
        cdef int i
        for i in range(n):
            yield ReactionSpecies(self._solver, i)

    def __getitem__(self, key) -> ReactionSpecies:
        """Look a species up by zero-based index or by exact name.

        A string key costs an **O(n) linear scan** (there is no C-side name
        index for reaction species).

        :raises ElementNotFoundError: No species has that name.
        :raises IndexError: Integer key out of range.
        """
        return ReactionSpecies(self._solver, self.get_index(key))

    def __contains__(self, key) -> bool:
        try:
            self.get_index(key)
            return True
        except (KeyError, IndexError, TypeError):
            return False

    def get_index(self, key) -> int:
        """Resolve ``int | str`` *key* to a zero-based species index.

        String lookup is an O(n) exact-name scan.
        """
        return _resolve_row(
            self._solver, key, _species_count, _species_row, "Reaction species")

    def add(self, str name, *, bint wall=False, str units="",
            double atol=0.0, double rtol=0.0) -> ReactionSpecies:
        """Declare a new species and return a live view of it.

        ``BUILDING``/``OPENED`` only. The species registry's MSX block is
        rebuilt in place and the whole system recompiled before this returns;
        if anything stops compiling the addition is rolled back and refused.

        :param name: Species name; becomes an expression identifier.
        :param wall: ``True`` declares a WALL species, ``False`` a BULK species.
        :param units: Concentration units token (e.g. ``"MG"``, ``"UG"``, ``"#"``).
        :param atol: Absolute solver tolerance; ``0.0`` uses the global ATOL.
        :param rtol: Relative solver tolerance; ``0.0`` uses the global RTOL.
        :returns: A :class:`ReactionSpecies` view of the appended row.
        :raises BadParamError: The declaration is rejected (nothing changed).
        :raises LifecycleError: The engine is past ``OPENED``.
        """
        cdef bytes b_name = name.encode('utf-8')
        cdef bytes b_units = units.encode('utf-8')
        _check(swmm_reaction_species_add(
            _h(self._solver), b_name, 1 if wall else 0, b_units, atol, rtol))
        self._solver._bump_generation()
        return ReactionSpecies(self._solver, _species_count(self._solver) - 1)

    def remove(self, key) -> None:
        """Remove the species named or indexed by *key*.

        ``BUILDING``/``OPENED`` only. **Refused while the species is still
        referenced** by any compiled expression — remove or rewrite the
        referring expressions first. All later species shift down by one.

        :raises BadParamError: The species is still referenced (nothing changed).
        :raises ElementNotFoundError: No species has that name.
        """
        cdef int idx = self.get_index(key)
        _check(swmm_reaction_species_remove(_h(self._solver), idx))
        self._solver._bump_generation()

    def __repr__(self) -> str:
        try:
            return f"<ReactionSpecies collection n={len(self)}>"
        except Exception:
            return "<ReactionSpecies collection (engine closed)>"


class _CoefficientCollection:
    """``solver.reactions.coefficients`` — collection of
    :class:`ReactionCoefficient`.

    The authoritative coefficient vocabulary for a completer; enumerate it
    rather than hard-coding names.
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        return _coeff_count(self._solver)

    def __iter__(self) -> Iterator[ReactionCoefficient]:
        cdef int n = _coeff_count(self._solver)
        cdef int i
        for i in range(n):
            yield ReactionCoefficient(self._solver, i)

    def __getitem__(self, key) -> ReactionCoefficient:
        """Look a coefficient up by zero-based index or by exact name
        (string keys cost an O(n) scan)."""
        return ReactionCoefficient(self._solver, self.get_index(key))

    def __contains__(self, key) -> bool:
        try:
            self.get_index(key)
            return True
        except (KeyError, IndexError, TypeError):
            return False

    def get_index(self, key) -> int:
        """Resolve ``int | str`` *key* to a zero-based coefficient index
        (string lookup is an O(n) exact-name scan)."""
        return _resolve_row(
            self._solver, key, _coeff_count, _coeff_row, "Reaction coefficient")

    def add(self, str name, *, bint parameter=False,
            double value=0.0) -> ReactionCoefficient:
        """Add a coefficient and return a live view of it.

        ``BUILDING``/``OPENED`` only, validated eagerly with rollback on any
        compile failure.

        :param name: Coefficient name; becomes an expression identifier.
        :param parameter: ``True`` declares a PARAMETER (overridable per pipe
            or tank), ``False`` a CONSTANT.
        :param value: Initial numeric value.
        :returns: A :class:`ReactionCoefficient` view of the appended row.
        :raises BadParamError: The declaration is rejected (nothing changed).
        :raises LifecycleError: The engine is past ``OPENED``.
        """
        cdef bytes b = name.encode('utf-8')
        _check(swmm_reaction_coeff_add(
            _h(self._solver), b, 1 if parameter else 0, value))
        self._solver._bump_generation()
        return ReactionCoefficient(self._solver, _coeff_count(self._solver) - 1)

    def remove(self, key) -> None:
        """Remove the coefficient named or indexed by *key*.

        ``BUILDING``/``OPENED`` only. **Refused while referenced** by any
        compiled expression. All later coefficients shift down by one.

        :raises BadParamError: The coefficient is still referenced.
        :raises ElementNotFoundError: No coefficient has that name.
        """
        cdef int idx = self.get_index(key)
        _check(swmm_reaction_coeff_remove(_h(self._solver), idx))
        self._solver._bump_generation()

    def __repr__(self) -> str:
        try:
            return f"<ReactionCoefficient collection n={len(self)}>"
        except Exception:
            return "<ReactionCoefficient collection (engine closed)>"


class _TermCollection:
    """``solver.reactions.terms`` — collection of :class:`ReactionTerm`.

    Terms are **ordered**: a term may reference only terms declared before it.
    :meth:`add` appends, so a new term can reference every existing one.
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        return _term_count(self._solver)

    def __iter__(self) -> Iterator[ReactionTerm]:
        cdef int n = _term_count(self._solver)
        cdef int i
        for i in range(n):
            yield ReactionTerm(self._solver, i)

    def __getitem__(self, key) -> ReactionTerm:
        """Look a term up by zero-based index or by exact name (string keys
        cost an O(n) scan)."""
        return ReactionTerm(self._solver, self.get_index(key))

    def __contains__(self, key) -> bool:
        try:
            self.get_index(key)
            return True
        except (KeyError, IndexError, TypeError):
            return False

    def get_index(self, key) -> int:
        """Resolve ``int | str`` *key* to a zero-based term index (string
        lookup is an O(n) exact-name scan)."""
        return _resolve_row(
            self._solver, key, _term_count, _term_row, "Reaction term")

    def add(self, str name, str expr) -> ReactionTerm:
        """Append an intermediate term and return a live view of it.

        ``BUILDING``/``OPENED`` only. Validated eagerly with rollback: an
        expression that does not compile against the current vocabulary is
        refused and nothing is added. Because the term is appended last, it
        may reference every term already declared — but nothing declared
        after it.

        :param name: Term name; becomes an expression identifier.
        :param expr: The term's expression text.
        :returns: A :class:`ReactionTerm` view of the appended row.
        :raises BadParamError: The expression does not compile.
        :raises LifecycleError: The engine is past ``OPENED``.
        """
        cdef bytes b_name = name.encode('utf-8')
        cdef bytes b_expr = expr.encode('utf-8')
        _check(swmm_reaction_term_add(_h(self._solver), b_name, b_expr))
        self._solver._bump_generation()
        return ReactionTerm(self._solver, _term_count(self._solver) - 1)

    def remove(self, key) -> None:
        """Remove the term named or indexed by *key*.

        ``BUILDING``/``OPENED`` only. **Refused while referenced** by any
        later term or species expression. All later terms shift down by one.

        :raises BadParamError: The term is still referenced.
        :raises ElementNotFoundError: No term has that name.
        """
        cdef int idx = self.get_index(key)
        _check(swmm_reaction_term_remove(_h(self._solver), idx))
        self._solver._bump_generation()

    def __repr__(self) -> str:
        try:
            return f"<ReactionTerm collection n={len(self)}>"
        except Exception:
            return "<ReactionTerm collection (engine closed)>"


class _InitialQualityRows:
    """``solver.reactions.initial`` — the per-node / per-link initial-quality
    rows.

    Rows are flat :class:`ReactionInitialEntry` **snapshots**, not live views:
    a row carries no identity beyond its ``(is_link, elem_index,
    species_index)`` key, so there is nothing to keep live. Model-wide
    defaults live on :attr:`ReactionSpecies.initial` instead.
    """

    def __init__(self, solver):
        self._solver = solver

    def __len__(self) -> int:
        return swmm_reaction_init_elem_count(_h(self._solver))

    def __iter__(self) -> Iterator[ReactionInitialEntry]:
        cdef int n = swmm_reaction_init_elem_count(_h(self._solver))
        cdef int i
        for i in range(n):
            yield self[i]

    def __getitem__(self, int row) -> ReactionInitialEntry:
        """Return row *row* as a :class:`ReactionInitialEntry` snapshot.

        :param row: Zero-based row index (negatives count from the end).
        :raises IndexError: *row* is out of range.
        """
        cdef int n = swmm_reaction_init_elem_count(_h(self._solver))
        cdef int idx = row
        cdef int is_link = 0
        cdef int elem_idx = 0
        cdef int species_idx = 0
        cdef double value = 0.0
        if idx < 0:
            idx += n
        if not (0 <= idx < n):
            raise IndexError(f"Initial-quality row index out of range: {row}")
        _check(swmm_reaction_init_elem_get(
            _h(self._solver), idx, &is_link, &elem_idx, &species_idx, &value))
        return ReactionInitialEntry(
            is_link != 0, elem_idx, species_idx, value)

    def set(self, bint is_link, int elem_index, species,
            double value) -> None:
        """Upsert the initial concentration for one element and species.

        The row key is ``(is_link, elem_index, species_index)``: an existing
        row with that key is updated in place, otherwise a new row is
        appended. ``BUILDING``/``OPENED`` only.

        :param is_link: ``True`` when *elem_index* addresses a link, ``False``
            when it addresses a node.
        :param elem_index: Zero-based node or link index.
        :param species: Reaction species as an index or an exact name (a
            string costs an O(n) scan).
        :param value: Initial concentration in the species' units; must be
            ``>= 0``.
        :raises BadParamError: *value* is negative, or an index is invalid.
        :raises ElementNotFoundError: No species has that name.
        :raises LifecycleError: The engine is past ``OPENED``.
        """
        cdef int sp = _resolve_row(
            self._solver, species, _species_count, _species_row,
            "Reaction species")
        _check(swmm_reaction_init_elem_set(
            _h(self._solver), 1 if is_link else 0, elem_index, sp, value))

    def remove(self, int row) -> None:
        """Remove initial-quality row *row*; all later rows shift down by one.

        ``BUILDING``/``OPENED`` only.

        :param row: Zero-based row index (negatives count from the end).
        :raises IndexError: *row* is out of range.
        """
        cdef int n = swmm_reaction_init_elem_count(_h(self._solver))
        cdef int idx = row
        if idx < 0:
            idx += n
        if not (0 <= idx < n):
            raise IndexError(f"Initial-quality row index out of range: {row}")
        _check(swmm_reaction_init_elem_remove(_h(self._solver), idx))

    def __repr__(self) -> str:
        try:
            return f"<ReactionInitialEntry rows n={len(self)}>"
        except Exception:
            return "<ReactionInitialEntry rows (engine closed)>"


# =============================================================================
# Reactions view
# =============================================================================

class Reactions:
    """``solver.reactions`` — the ``[REACTION_*]`` / ``.rxn`` surface.

    Groups the discovery/CRUD collections (:attr:`species`,
    :attr:`coefficients`, :attr:`terms`, :attr:`initial`), the
    ``[REACTION_OPTIONS]`` accessors, expression validation, and the
    whole-file text contract. See the module docstring for the two invariants
    (eager validation with rollback; byte-identical text round trip).
    """

    def __init__(self, solver):
        self._solver = solver
        self._species = None
        self._coefficients = None
        self._terms = None
        self._initial = None

    @property
    def species(self) -> _SpeciesCollection:
        """The declared species — authoritative completer vocabulary."""
        if self._species is None:
            self._species = _SpeciesCollection(self._solver)
        return self._species

    @property
    def coefficients(self) -> _CoefficientCollection:
        """The ``[REACTION_COEFFICIENTS]`` entries."""
        if self._coefficients is None:
            self._coefficients = _CoefficientCollection(self._solver)
        return self._coefficients

    @property
    def terms(self) -> _TermCollection:
        """The ordered ``[REACTION_TERMS]`` entries."""
        if self._terms is None:
            self._terms = _TermCollection(self._solver)
        return self._terms

    @property
    def initial(self) -> _InitialQualityRows:
        """The per-node / per-link initial-quality rows."""
        if self._initial is None:
            self._initial = _InitialQualityRows(self._solver)
        return self._initial

    # ------------------------------------------------------------------
    # Validation
    # ------------------------------------------------------------------

    def validate(self, str expr, scope=ReactionScope.PIPE) -> ExpressionDiagnostic:
        """Compile-only check of one expression. **Never changes state.**

        The expression is compiled against this model's live vocabulary —
        species, coefficients, terms, pollutants, and the built-in hydraulic
        variables — so a GUI can validate as the user types.

        *scope* selects the identifier-resolution vocabulary. Note the
        deliberate caveat for ``ReactionScope.TERM``: references to **all**
        terms are accepted there, including ones declared later. The
        forward-only term-ordering rule is enforced at file-apply time, where
        ordinal position exists; ``validate`` answers "is this well-formed
        against the vocabulary", not "is the file orderable".

        :param expr: Expression text.
        :param scope: :class:`~openswmm.engine.ReactionScope`; defaults to
            ``PIPE``.
        :returns: An :class:`ExpressionDiagnostic` — ``valid`` True with an
            empty message on success; otherwise the engine diagnostic and a
            1-based ``column`` (``-1`` when not attributable).
        """
        cdef bytes b = expr.encode('utf-8')
        cdef char errbuf[512]
        cdef int col = -1
        cdef int rc
        errbuf[0] = 0
        rc = swmm_reaction_validate_expression(
            _h(self._solver), int(scope), b, errbuf, 512, &col)
        if rc == 0:
            return ExpressionDiagnostic(True, "", -1)
        return ExpressionDiagnostic(False, errbuf.decode('utf-8'), col)

    # ------------------------------------------------------------------
    # Options
    # ------------------------------------------------------------------

    def get_option(self, str key) -> str:
        """Read one ``[REACTION_OPTIONS]`` value as its canonical token.

        Options are exposed as an explicit ``get_option`` / ``set_option``
        pair rather than a mapping view: the key set is fixed by the engine
        and every value is a token string, so a dict view would only add
        indirection.

        :param key: One of ``SOLVER``, ``COUPLING``, ``RATE_UNITS``,
            ``AREA_UNITS``, ``TIMESTEP``, ``ATOL``, ``RTOL``.
        :returns: The canonical token (numeric options come back as text —
            ``TIMESTEP`` in seconds, ``ATOL``/``RTOL`` dimensionless).
        :raises BadParamError: *key* is not a recognised option.
        """
        cdef bytes b = key.encode('utf-8')
        cdef char buf[128]
        buf[0] = 0
        _check(swmm_reaction_option_get(_h(self._solver), b, buf, 128))
        return buf.decode('utf-8')

    def set_option(self, str key, str value) -> None:
        """Set one ``[REACTION_OPTIONS]`` value from its canonical token.

        ``BUILDING``/``OPENED`` only, validated eagerly with rollback.

        :param key: One of ``SOLVER``, ``COUPLING``, ``RATE_UNITS``,
            ``AREA_UNITS``, ``TIMESTEP``, ``ATOL``, ``RTOL``.
        :param value: The canonical token; numeric options take their value
            as text (``TIMESTEP`` in seconds, ``ATOL``/``RTOL`` dimensionless).
        :raises BadParamError: *key* or *value* is not recognised.
        :raises LifecycleError: The engine is past ``OPENED``.
        """
        cdef bytes b_key = key.encode('utf-8')
        cdef bytes b_val = value.encode('utf-8')
        _check(swmm_reaction_option_set(_h(self._solver), b_key, b_val))

    # ------------------------------------------------------------------
    # Whole-file text surface
    # ------------------------------------------------------------------

    def serialize(self) -> str:
        """Return the canonical ``.rxn`` text for the current engine state.

        This is the GUI text tab's read side, and it round-trips exactly:
        ``serialize() -> apply_text() -> serialize()`` is byte-identical.

        Implemented as the C API's two-pass size query — a first call with a
        NULL buffer fills in the required length (including the NUL), then a
        second call fills a buffer of exactly that size.

        :returns: The canonical ``.rxn`` text; ``""`` when no reaction system
            is configured.
        """
        cdef SWMM_Engine h = _h(self._solver)
        cdef int needed = 0
        cdef int rc
        cdef bytearray raw
        cdef char* buf
        rc = swmm_reactions_serialize(h, NULL, 0, &needed)
        if needed <= 0:
            # Surface a genuine engine failure; otherwise there is simply
            # nothing to serialize.
            _check(rc)
            return ""
        raw = bytearray(needed)
        buf = <char*>raw
        _check(swmm_reactions_serialize(h, buf, needed, &needed))
        return buf.decode('utf-8')

    def check_text(self, str text) -> ExpressionDiagnostic:
        """Dry-run a whole ``.rxn`` file against this model. **Never mutates.**

        A full parse and compile with zero state change on success *or*
        failure — the safe "does this text apply?" probe behind a text tab's
        live error gutter.

        :param text: Complete ``.rxn`` file text.
        :returns: An :class:`ExpressionDiagnostic`. ``column`` is always
            ``-1``: the whole-file checker reports the first diagnostic as
            text, without a column.
        """
        cdef bytes b = text.encode('utf-8')
        cdef char errbuf[512]
        cdef int rc
        errbuf[0] = 0
        rc = swmm_reactions_check_text(_h(self._solver), b, errbuf, 512)
        if rc == 0:
            return ExpressionDiagnostic(True, "", -1)
        return ExpressionDiagnostic(False, errbuf.decode('utf-8'), -1)

    def apply_text(self, str text) -> None:
        """Transactionally replace the whole reaction system from ``.rxn`` text.

        ``BUILDING``/``OPENED`` only. Staged: on **any** error the previous
        system — reaction state *and* the registry block — is byte-identical
        to what it was before the call, so a rejected edit costs nothing.
        Unlike :meth:`check_text`, this mutates on success, and every wrapper
        index (species, coefficients, terms, initial rows) is invalidated.

        This is also where the forward-only term-ordering rule is enforced,
        since ordinal position exists in a file.

        :param text: Complete ``.rxn`` file text — typically an edited
            :meth:`serialize` result.
        :raises BadParamError: The text does not parse or compile; the
            exception message is the engine's own first diagnostic and the
            previous system is intact.
        :raises LifecycleError: The engine is past ``OPENED``.
        """
        cdef bytes b = text.encode('utf-8')
        cdef char errbuf[512]
        cdef int rc
        errbuf[0] = 0
        rc = swmm_reactions_apply_text(_h(self._solver), b, errbuf, 512)
        if rc != 0:
            raise_for_code(rc, errbuf.decode('utf-8'))
        self._solver._bump_generation()

    def save(self, path=None) -> None:
        """Serialize the reaction system to a ``.rxn`` file on disk.

        :param path: Destination path. When ``None`` (the default) the file
            is written to the path bound to the reaction component's config
            entry.
        :raises BadParamError: *path* is ``None`` and no component config
            path is bound, so there is nowhere to write.
        """
        cdef bytes b
        if path is None:
            _check(swmm_reactions_save(_h(self._solver), NULL))
        else:
            b = str(path).encode('utf-8')
            _check(swmm_reactions_save(_h(self._solver), b))

    # ------------------------------------------------------------------
    # Static vocabulary — no engine handle, no open model
    # ------------------------------------------------------------------

    @staticmethod
    def hydraulic_variables() -> Tuple[ReactionHydVar, ...]:
        """Return every built-in hydraulic variable usable in an expression.

        **Engine-less static.** This needs no open model and no ``Solver`` —
        call it on the class (``Reactions.hydraulic_variables()``) at any
        time, including before a model is loaded, to populate a completer or
        a syntax highlighter.

        It reads the expression compiler's own tables, so it is the
        AUTHORITATIVE vocabulary: a GUI or agent must enumerate it rather
        than hard-coding a list, which is what keeps vocabulary drift
        structural instead of a matter of discipline.

        :returns: A tuple of :class:`ReactionHydVar` — the variables
            ``D Q U RE US FF AV HRT DT`` and any the engine adds later.
        """
        cdef int n = swmm_reaction_hydvar_count()
        cdef int i
        cdef char name[128]
        cdef char desc[256]
        out = []
        for i in range(n):
            name[0] = 0
            desc[0] = 0
            _check(swmm_reaction_hydvar_get(i, name, 128, desc, 256))
            out.append(ReactionHydVar(
                name.decode('utf-8'), desc.decode('utf-8')))
        return tuple(out)

    @staticmethod
    def functions() -> Tuple[ReactionFunction, ...]:
        """Return every built-in math function usable in an expression.

        **Engine-less static**, exactly like
        :meth:`hydraulic_variables`: no open model, no ``Solver``, and it
        reads the expression compiler's own tables — so it is the
        AUTHORITATIVE function vocabulary. Enumerate it; do not hard-code.

        :returns: A tuple of :class:`ReactionFunction` — ``EXP`` through
            ``TAN`` with arity 1, and ``MIN``, ``MAX``, ``POW`` with arity 2.
        """
        cdef int n = swmm_reaction_function_count()
        cdef int i
        cdef int arity = 0
        cdef char name[128]
        out = []
        for i in range(n):
            name[0] = 0
            arity = 0
            _check(swmm_reaction_function_get(i, name, 128, &arity))
            out.append(ReactionFunction(name.decode('utf-8'), arity))
        return tuple(out)

    def __repr__(self) -> str:
        try:
            return (f"<Reactions species={len(self.species)} "
                    f"coefficients={len(self.coefficients)} "
                    f"terms={len(self.terms)}>")
        except Exception:
            return "<Reactions (engine closed)>"
