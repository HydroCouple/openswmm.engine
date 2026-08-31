====================================
Water age  (``[WATER_AGE_SOURCES]``)
====================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

``solver.water_age`` is the editable view over the ``[WATER_AGE_SOURCES]``
table of the ``model.age`` component: a **global** age per source
pathway, plus per-node overrides for the two pathways that take node
scope. All values are **hours**, the config file's own unit.

Reference: ``openswmm_water_age.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver, WaterAgeSource

    with Solver("model.inp") as s:
        print(s.water_age.enabled)              # [OPTIONS] WATER_AGE

        s.water_age.globals[WaterAgeSource.RAINFALL] = 0.0
        s.water_age.globals[WaterAgeSource.GW] = 720.0        # hours
        s.water_age.globals[WaterAgeSource.EXTERNAL_INFLOW] = -6.0

        s.water_age.node_overrides.set(WaterAgeSource.DWF, "J1", 12.0)
        for row in s.water_age.node_overrides:
            print(row.source, row.node_index, row.hours)

        s.water_age.save("model.age")

.. note::

   Edits are **live**: the loaders re-read the table every step, so a
   mid-simulation write takes effect on the next routing step.

----

Global source ages
==================

:attr:`WaterAge.globals` maps :class:`WaterAgeSource` to ``float`` hours
and supports ``len()``, iteration, ``in``, and ``int | str`` keys.

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Source
     - Pathway
   * - ``RAINFALL``
     - Rainfall-derived water.
   * - ``DWF``
     - Dry-weather flow. **Takes node scope.**
   * - ``GW``
     - Groundwater.
   * - ``RDII``
     - Rainfall-derived infiltration and inflow.
   * - ``EXTERNAL_INFLOW``
     - ``[INFLOWS]``. **Takes node scope.**
   * - ``IFACE``
     - Interface file.
   * - ``INITIAL_STATE``
     - Water present at ``t = 0``.

.. important::

   **Negative values are legal.** A negative source age *extracts*
   age-volume rather than adding it, and the result is clamped so the
   computed age never goes below zero. Do not add a client-side
   non-negative guard — it would reject a modelling construct the engine
   supports deliberately. (Contrast :doc:`initial_quality`, where
   *pollutant* values must be non-negative.)

The C enum's trailing ``COUNT = 7`` sentinel is deliberately not mirrored
in Python: ``len(WaterAgeSource)`` is the count.

----

Node overrides
==============

:attr:`WaterAge.node_overrides` is a row-indexed sequence of
:class:`WaterAgeOverride` named tuples
``(source, node_index, hours)``. Row order is stable across edits within
a session.

.. list-table::
   :header-rows: 1
   :widths: 44 56

   * - Operation
     - Behaviour
   * - ``len(s.water_age.node_overrides)``
     - Number of override rows.
   * - ``s.water_age.node_overrides[row]``
     - One :class:`WaterAgeOverride` by row index.
   * - ``.set(source, node, hours)``
     - Add **or update** the row for ``(source, node)``.
   * - ``.remove(source, node)``
     - Delete by key, not by row index.

.. warning::

   Only ``WaterAgeSource.DWF`` and ``WaterAgeSource.EXTERNAL_INFLOW``
   accept node scope — the parser's own rule. Any other source is
   **refused**, not silently folded into the global value.

Setting the same ``(source, node)`` pair twice is an update, so an editor
can change a value it just wrote. Negative hours are legal here too.

----

Saving
======

:meth:`WaterAge.save` writes the current table as a
``[WATER_AGE_SOURCES]`` component file — the ``model.age`` format the
water-age component parses on the next open. It accepts ``str`` or any
:class:`os.PathLike`.

.. code-block:: python

    from pathlib import Path

    s.water_age.save(Path("scenarios") / "baseline.age")

----

See also
========

* :doc:`heat` — the same source/override table shape, in degC.
* :doc:`initial_quality` — seeding ``__WATER_AGE__`` per element.
* :doc:`inflows` — the DWF and ``[INFLOWS]`` pathways that take node
  scope.
* :doc:`process_components` — registering the ``model.age`` config path.
* :doc:`error_handling` — what a refused write raises.
