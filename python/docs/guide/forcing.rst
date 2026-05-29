================
Advanced forcing
================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

The :class:`Forcing` view reached via ``solver.forcing`` overrides
runtime state across node, link, subcatchment, and gage domains. It is
the long-form alternative to the per-object setters
(:attr:`Node.lateral_inflow = ...`) and adds two important knobs:

* **mode** (:class:`ForcingMode`) — ``REPLACE`` overwrites the
  engine-computed value; ``ADD`` adds to it.
* **persist** — ``False`` (default) means one-shot for the next step;
  ``True`` keeps the forcing active every step until cleared.

Reference: ``openswmm_forcing.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver, ForcingMode, ForcingTarget

    with Solver("model.inp") as s:
        # One-shot — overwritten by the engine on the next step.
        s.forcing.node_lat_inflow("J1", 0.5)

        # Sticky add — survives every step until cleared.
        s.forcing.node_lat_inflow(
            "J1", 0.1,
            mode=ForcingMode.ADD,
            persist=True,
        )

        for _ in s.steps():
            pass

        # Clear one forcing or everything.
        s.forcing.clear(ForcingTarget.NODE, "J1")
        s.forcing.clear_all()

----

Methods
=======

Every method accepts an object selector (``int | str``), the new value,
plus keyword-only ``mode`` (defaulting to ``ForcingMode.REPLACE``) and
``persist`` (defaulting to ``False``).

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Forces …
   * - ``node_lat_inflow(node, value)``
     - Lateral inflow at a node.
   * - ``node_head_boundary(node, value)``
     - Head boundary at a node.
   * - ``node_quality(node, pollutant, mass_rate)``
     - Quality mass-flux at a node.
   * - ``link_flow(link, value)``
     - Flow through a link.
   * - ``link_setting(link, value)``
     - Control setting on a link.
   * - ``subcatchment_rainfall(sub, value)``
     - Rainfall on a subcatchment.
   * - ``subcatchment_evap(sub, value)``
     - Evaporation on a subcatchment.
   * - ``gage_rainfall(gage, value)``
     - Rainfall on a gage.

Clearing
--------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - What it does
   * - ``clear(target, key)``
     - Clear forcing on one object. ``target`` is a :class:`ForcingTarget`
       enum (``NODE`` / ``LINK`` / ``SUBCATCH`` / ``GAGE``).
   * - ``clear_all()``
     - Clear every forcing on the engine.

----

See also
========

* :doc:`nodes`, :doc:`links`, :doc:`subcatchments`, :doc:`gages` — the
  per-object setters are equivalent to ``mode=REPLACE, persist=False``
  forcing.
* :doc:`controls` — rule-based overrides.
* :doc:`error_handling`.
