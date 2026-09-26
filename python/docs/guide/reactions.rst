========================================================
Reactions  (multi-species: species, coefficients, terms)
========================================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

``solver.reactions`` is the editable multi-species reaction system — the
``[REACTION_*]`` sections of a ``.rxn`` component file — as live objects:
species with per-scope expressions, coefficients, intermediate terms,
initial-quality rows, a whole-file text surface, and an engine-less
static vocabulary for completers.

Reference: ``openswmm_reactions.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver, ReactionScope, ReactionExprForm

    with Solver("model.inp") as s:
        k = s.reactions.coefficients.add("Kb", parameter=False, value=0.3)
        cl = s.reactions.species.add("CL2", units="MG", atol=1e-4, rtol=1e-4)

        cl.set_expression(ReactionScope.PIPE, ReactionExprForm.RATE, "-Kb*CL2")
        cl.set_expression(ReactionScope.TANK, ReactionExprForm.RATE, "-Kb*CL2")

        diag = s.reactions.validate("-Kb*CL2", ReactionScope.PIPE)
        print(diag.valid, diag.message, diag.column)

        text = s.reactions.serialize()      # canonical .rxn text
        s.reactions.apply_text(text)        # transactional round trip
        s.reactions.save("model.rxn")

.. warning::

   Mutation is **BUILDING/OPENED only** — reaction rows seed state at
   ``initialize()``, so there is nothing meaningful to change once a run
   is under way.

----

Eager validation: a stored model is never uncompilable
======================================================

Every mutator validates **eagerly**. The whole reaction system is
recompiled before the call returns, and a mutation that would leave any
expression uncompilable is **rolled back and refused**. There is no
deferred check at run time and no "save now, discover later": if the call
succeeded, the system compiles.

Two consequences follow directly:

* Removing a species, coefficient, or term that any compiled expression
  still references is **refused**. Rewrite the referring expressions
  first, then remove.
* An editor can never store an uncompilable model, so it does not need a
  separate "is this model still valid?" pass after each keystroke.

:meth:`Reactions.validate` is the read-only counterpart — compile-only,
zero state change — returning an :class:`ExpressionDiagnostic` named
tuple of ``(valid, message, column)``. ``column`` is 1-based, or ``-1``
when the diagnostic is not attributable to a position.

.. note::

   Under :attr:`ReactionScope.TERM`, validation accepts references to
   **all** terms, including ones defined later. The forward-only ordering
   rule is enforced at file-apply time, where ordinal position exists.
   Validation answers "is this well-formed against the vocabulary", not
   "is the file orderable".

----

Species, coefficients, terms
============================

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Collection
     - Contents
   * - :attr:`Reactions.species`
     - :class:`ReactionSpecies` — ``name``, ``is_wall``, ``units``,
       ``atol``, ``rtol``, ``initial``, and the two scope expressions.
   * - :attr:`Reactions.coefficients`
     - :class:`ReactionCoefficient` — ``name``, ``is_param``
       (PARAMETER vs CONSTANT), ``value``.
   * - :attr:`Reactions.terms`
     - :class:`ReactionTerm` — ``name`` and its source ``expression``.
   * - :attr:`Reactions.initial`
     - :class:`ReactionInitialEntry` rows —
       ``(is_link, elem_index, species_index, value)``.

All three collections support ``len()``, iteration, ``in``, indexing by
``int | str``, ``get_index(key)``, ``add(...)`` and ``remove(key)``.

.. code-block:: python

    for sp in s.reactions.species:
        print(sp.index, sp.name, sp.units, sp.is_wall)

    "CL2" in s.reactions.species          # True
    s.reactions.species.get_index("CL2")  # 0

    s.reactions.terms.add("Kf", "1.5826e-4*RE^0.88/D")

Per-scope expressions
---------------------

A species carries one expression per scope. Read them through
:attr:`ReactionSpecies.pipe_expression` /
:attr:`ReactionSpecies.tank_expression`, or generically through
:meth:`ReactionSpecies.get_expression`; both return a
``(ReactionExprForm, str)`` pair.
:meth:`ReactionSpecies.set_expression` writes one, and passing
:attr:`ReactionExprForm.NONE` **clears** it.

.. code-block:: python

    form, expr = s.reactions.species["CL2"].get_expression(ReactionScope.PIPE)
    s.reactions.species["CL2"].set_expression(
        ReactionScope.TANK, ReactionExprForm.NONE)

Options are canonical string tokens via :meth:`Reactions.get_option` /
:meth:`Reactions.set_option`, keyed on ``SOLVER``, ``COUPLING``,
``RATE_UNITS``, ``AREA_UNITS``, ``TIMESTEP``, ``ATOL`` and ``RTOL``.

----

Whole-file text surface
=======================

:meth:`Reactions.serialize` renders canonical ``.rxn`` text from engine
state; :meth:`Reactions.apply_text` replaces the whole system from text;
:meth:`Reactions.check_text` is the dry run, with zero state change on
success or failure. :meth:`Reactions.save` writes to the component's
bound config path, or to an explicit ``path``.

.. code-block:: python

    text = s.reactions.serialize()
    diag = s.reactions.check_text(text)      # ExpressionDiagnostic
    s.reactions.apply_text(text)
    assert s.reactions.serialize() == text   # byte-identical

Two guarantees this surface makes:

* **Round trip.** ``serialize() -> apply_text() -> serialize()`` is
  byte-identical. A text editor tab can hand the user its own output back
  without churn.
* **Transactional apply.** :meth:`Reactions.apply_text` is staged: on
  **any** error, the previous system — reaction state and registry block
  alike — is byte-identical to what it was before the call. A failed
  paste costs nothing.

----

Static vocabulary  (completers)
===============================

:meth:`Reactions.hydraulic_variables` and :meth:`Reactions.functions` are
``staticmethod``\ s: they need **no engine and no open model**, and are
usable before anything is loaded.

.. code-block:: python

    from openswmm.engine import Reactions

    for hv in Reactions.hydraulic_variables():
        print(hv.name, hv.description)        # ReactionHydVar

    for fn in Reactions.functions():
        print(fn.name, fn.arity)              # ReactionFunction

.. important::

   These two are the **authoritative** completer and syntax-highlighter
   vocabulary, sourced from the expression compiler's own tables. Tooling
   must enumerate them rather than hard-code a list — that is what makes
   vocabulary drift structurally impossible instead of a matter of
   discipline. Combine them with the live
   :attr:`Reactions.species` / :attr:`Reactions.coefficients` /
   :attr:`Reactions.terms` names for the full identifier set.

----

See also
========

* :doc:`pollutants` — reaction expressions may reference pollutants.
* :doc:`initial_quality` — the ``[INITIAL_QUALITY]`` sibling surface.
* :doc:`process_components` — registering the ``.rxn`` config path.
* :doc:`quality` — single-species buildup/washoff and treatment.
* :doc:`error_handling` — what a refused mutation raises.
