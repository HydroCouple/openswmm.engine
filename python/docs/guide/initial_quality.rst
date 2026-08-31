========================================
Initial quality  (``[INITIAL_QUALITY]``)
========================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

``solver.initial_quality`` is the row-based view over
``[INITIAL_QUALITY]``: per-element starting concentrations for nodes and
links. Rows are indexed ``0 .. len-1`` in file order and seed state at
``initialize()``.

Reference: ``openswmm_initial_quality.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver, InitialQuality

    with Solver("model.inp") as s:
        s.initial_quality.set("TSS", 12.5, node="J1")
        s.initial_quality.set("TSS", 3.0, link="C1")

        # Reserved species: hours (signed) and degC.
        s.initial_quality.set(InitialQuality.WATER_AGE, 6.0, node="J1")
        s.initial_quality.set(InitialQuality.TEMPERATURE, 14.2, node="J1")

        for row in s.initial_quality:
            print(row.is_link, row.elem_index, row.constituent, row.value)

        s.initial_quality.remove(0)     # later rows shift down

.. warning::

   Mutation is **BUILDING/OPENED only**. Rows seed state at
   ``initialize()``, so writing them mid-run would change nothing — the
   same contract as :attr:`Pollutant.init_conc`.

----

Rows are an upsert
==================

:meth:`InitialQuality.set` is an **upsert keyed on**
``(is_link, elem_index, constituent)``. Writing the same triple twice
updates the existing row rather than appending a duplicate, so an editor
can change a value it just wrote.

Pass exactly one of ``node=`` or ``link=``; either accepts an ``int``
index or a ``str`` id.

.. list-table::
   :header-rows: 1
   :widths: 42 58

   * - Operation
     - Behaviour
   * - ``len(s.initial_quality)``
     - Number of rows.
   * - ``s.initial_quality[row_index]``
     - One :class:`InitialQualityEntry` —
       ``(is_link, elem_index, constituent, value)``.
   * - ``for row in s.initial_quality:``
     - Iterate rows in file order.
   * - ``.set(constituent, value, *, node=..., link=...)``
     - Upsert on the ``(is_link, elem_index, constituent)`` key.
   * - ``.remove(row_index)``
     - Delete by **row index**.

.. warning::

   :meth:`InitialQuality.remove` shifts every later row down by one, so a
   cached row index goes **stale** the moment anything before it is
   removed. Re-enumerate after removing, or remove from the highest index
   downward.

``elem_index`` reads ``-1`` when the row's element could not be resolved.

----

Constituents
============

The constituent is a **name**: either a ``[POLLUTANTS]`` pollutant, or
one of two reserved species exposed as class attributes so you never have
to spell the sentinel by hand.

.. list-table::
   :header-rows: 1
   :widths: 32 20 48

   * - Constituent
     - Units
     - Notes
   * - A ``[POLLUTANTS]`` name
     - Pollutant's own units
     - Concentration. **Must be non-negative** — a negative value is
       refused.
   * - :attr:`InitialQuality.WATER_AGE`
     - hours
     - The literal ``"__WATER_AGE__"``. **Signed** — negative values are
       legal.
   * - :attr:`InitialQuality.TEMPERATURE`
     - degC
     - The literal ``"__TEMPERATURE__"``. Signed.

An unknown constituent name, a bad element index, or a negative pollutant
value is refused.

.. code-block:: python

    InitialQuality.WATER_AGE     # "__WATER_AGE__"
    InitialQuality.TEMPERATURE   # "__TEMPERATURE__"

----

See also
========

* :doc:`pollutants` — declaring the pollutants these rows may name, and
  the model-wide :attr:`Pollutant.init_conc` default.
* :doc:`quality` — buildup, washoff, and treatment.
* :doc:`water_age` — the source-age table the ``__WATER_AGE__`` rows seed
  against.
* :doc:`heat` — the source-temperature table behind ``__TEMPERATURE__``.
* :doc:`error_handling` — what a refused row raises.
