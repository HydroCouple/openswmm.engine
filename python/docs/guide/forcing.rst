================
Advanced forcing
================

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.Forcing`.

.. currentmodule:: openswmm.engine

The :class:`Forcing` class is OpenSWMM 6's purpose-built API for
**runtime overrides**.  Per-step setters on the domain classes
(:meth:`Nodes.set_lateral_inflow`, :meth:`Gages.set_rainfall`, …) are
**one-shot** — overwritten by the engine on the next step.  Forcing
overrides are **persistent** until cleared, and they support multiple
*modes* (replace, add, scale).

The Forcing API is the right tool for:

* Driving a model from a Python source (ML controller, telemetry,
  user-supplied hyetograph) without re-applying the value every step.
* Combining multiple forcings on the same target (e.g. baseline +
  perturbation).
* Sweeping scenarios where a forcing differs only in one parameter.

Reference: ``openswmm_forcing.h``.

----

Class signature
===============

.. code-block:: python

    class Forcing:
        def __init__(self, solver: Solver) -> None: ...

----

Forcing modes
=============

Every setter takes a :class:`ForcingMode`:

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Mode
     - Effect on the engine's value at each step
   * - ``REPLACE``
     - Overwrite the engine value with the forcing value.
   * - ``ADD``
     - Add the forcing value to the engine value.
   * - ``SCALE``
     - Multiply the engine value by the forcing value.

The ``persist`` argument is a boolean: ``True`` keeps the forcing
active across steps until you :meth:`clear` it; ``False`` applies it
once and lets it lapse.

----

Key methods
===========

Per-target setters
------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Targets
   * - :meth:`node_lat_inflow(idx, value, mode, persist)`
     - Lateral inflow at a node.
   * - :meth:`node_head_boundary(idx, value, mode, persist)`
     - Head boundary at an outfall.
   * - :meth:`node_quality(idx, p, value, mode, persist)`
     - Pollutant ``p`` mass flux at a node.
   * - :meth:`link_flow(idx, value, mode, persist)`
     - Force a flow on a link.
   * - :meth:`link_setting(idx, value, mode, persist)`
     - Force a control setting on a link (orifice, weir, pump).
   * - :meth:`subcatch_rainfall(idx, value, mode, persist)`
     - Override rainfall on a subcatchment.
   * - :meth:`subcatch_evap(idx, value, mode, persist)`
     - Override evaporation on a subcatchment.
   * - :meth:`gage_rainfall(idx, value, mode, persist)`
     - Override rainfall at a gage.

All methods accept either a string id or an integer index for ``idx``.

Clearing
--------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Action
   * - :meth:`clear(target_type, idx)`
     - Remove all forcings on the given (target type, index).
   * - :meth:`clear_all()`
     - Remove every active forcing.

The ``target_type`` argument is a :class:`ForcingTarget` enum value.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import (
        Solver, Nodes, Forcing, ForcingMode,
    )

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes = Nodes(s)
        forcing = Forcing(s)

        j1 = nodes.get_index("J1")

        # Persistent baseline lateral inflow
        forcing.node_lat_inflow(j1, 0.5, ForcingMode.REPLACE, persist=True)

        # … plus a 2-hour pulse around the storm peak
        applied_pulse = False
        while s.step():
            h = s.elapsed * 24.0
            if 5.5 <= h <= 7.5 and not applied_pulse:
                forcing.node_lat_inflow(j1, 1.0, ForcingMode.ADD, persist=True)
                applied_pulse = True
            elif h > 7.5 and applied_pulse:
                # release just the additive pulse, keep the baseline
                forcing.clear(ForcingTarget.NODE_LAT_INFLOW, j1)
                forcing.node_lat_inflow(j1, 0.5, ForcingMode.REPLACE, persist=True)
                applied_pulse = False

        forcing.clear_all()

----

Common recipes
==============

Replace mode  (override the engine value)
------------------------------------------

.. code-block:: python

    forcing.node_lat_inflow("J1", 1.5, ForcingMode.REPLACE, persist=True)

After this call, every step will see lateral inflow = 1.5 at J1
regardless of what's in the ``.inp``.  Clear with :meth:`clear` or
:meth:`clear_all`.

Add mode  (perturb the engine value)
------------------------------------

.. code-block:: python

    # Add 0.3 cfs to whatever the engine already computes
    forcing.node_lat_inflow("J1", 0.3, ForcingMode.ADD, persist=True)

Scale mode  (proportional adjustment)
-------------------------------------

.. code-block:: python

    # 80% of nominal rainfall on every gage during the run
    for i in range(gages.count()):
        forcing.gage_rainfall(i, 0.8, ForcingMode.SCALE, persist=True)

Time-varying ML controller driving lateral inflows
--------------------------------------------------

.. code-block:: python

    while s.step():
        action = my_ml_controller.predict_inflow(observe(s))
        # one-shot per step is fine here — we re-apply each step anyway
        forcing.node_lat_inflow("J1", action, ForcingMode.REPLACE, persist=False)

Use ``persist=False`` when you intend to re-apply the forcing every
step — it's slightly cheaper than installing and clearing.

Two stacked forcings on the same target
---------------------------------------

.. code-block:: python

    # Engine value gets REPLACE'd to 0.0, then ADD'd to 1.5
    # → final value 1.5
    forcing.node_lat_inflow("J1", 0.0, ForcingMode.REPLACE, persist=True)
    forcing.node_lat_inflow("J1", 1.5, ForcingMode.ADD,     persist=True)

The order of evaluation is the order of registration.  ``clear()``
removes both at once; to remove only one, ``clear_all()`` and
re-register the survivor.

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method group
     - Required state
     - Notes
   * - per-target setters
     - ``RUNNING`` (preferred) or ``STARTED``
     - Persistence is honoured from registration onward.
   * - ``clear`` / ``clear_all``
     - any state where the solver is alive
     - Idempotent.

Common :class:`EngineError` codes:

* ``INVALID_INDEX`` — index out of range for the given target.
* ``INVALID_TYPE``  — calling a node-only setter on a link, etc.

----

See also
========

* :doc:`nodes` — one-shot per-step setters
  (:meth:`set_lateral_inflow` etc.) for callers that re-apply
  every step.
* :doc:`controls` — declarative rules that fire conditionally; use
  these instead of forcing when the trigger is internal to the model.
* :doc:`inflows` — static external-inflow registration in the model.
