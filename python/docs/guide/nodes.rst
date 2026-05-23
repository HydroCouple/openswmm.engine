=====
Nodes
=====

.. note::

   **Engine:** OpenSWMM 6 — refactored.  This page documents the
   :class:`openswmm.engine.Nodes` class.  Legacy SWMM 5 users access
   nodes through the enum-driven ``getValue`` /
   ``setValue`` API on :class:`openswmm.legacy.engine.Solver` — see
   :doc:`../legacy/solver`.

.. currentmodule:: openswmm.engine

Junctions, outfalls, storage units, and dividers — every point where
flow enters, leaves, or is stored in the network.  The :class:`Nodes`
class is the single entry point for reading and writing node state at
configure time AND during a running simulation.

Reference: ``openswmm_nodes.h``.

----

Class signature
===============

.. code-block:: python

    class Nodes:
        def __init__(self, solver: Solver) -> None: ...

* ``solver`` — an active :class:`Solver`.  The Nodes object holds a
  reference; its lifetime is tied to the solver's.

A single :class:`Nodes` instance covers **every** node in the model.
There is no per-node Python wrapper — work via integer indices or
string ids.

----

Key methods
===========

Identity & enumeration
----------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`count`
     - Number of nodes in the model (``int``).
   * - :meth:`get_index(id)`
     - Integer index for a string id.
   * - :meth:`get_id(idx)`
     - String id for an integer index.
   * - :meth:`get_type(idx)`
     - :class:`NodeType` of the node.

Geometry & basic properties
---------------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`get_invert_elev`
     - Invert (bottom) elevation, model length units.
   * - :meth:`get_max_depth`
     - Maximum depth above invert.
   * - :meth:`get_initial_depth`
     - Initial depth at simulation start.
   * - :meth:`get_surcharge_depth`
     - Surcharge depth above ``max_depth``.
   * - :meth:`get_ponded_area`
     - Surface ponded area when surcharged.
   * - :meth:`get_crown_elev`
     - Top-of-pipe elevation at the node.
   * - :meth:`get_full_volume`
     - Volume at ``max_depth`` (storage nodes).

Each ``get_*`` has a matching ``set_*`` that requires the solver to be in
``OPENED`` or later.

Hydraulic state (during the run)
--------------------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`get_depth`
     - Current water depth above invert.
   * - :meth:`get_head`
     - Total hydraulic head (invert + depth).
   * - :meth:`get_volume`
     - Current volume in the node.
   * - :meth:`get_inflow`
     - Total inflow at this step.
   * - :meth:`get_outflow`
     - Total outflow at this step.
   * - :meth:`get_lateral_inflow`
     - External (lateral) inflow.
   * - :meth:`get_overflow`
     - Overflow (flooding) flow.
   * - :meth:`get_losses`
     - Evaporation + seepage losses.
   * - :meth:`get_degree`
     - Number of links connected.

Forcing — overrides during a run
--------------------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Action
   * - :meth:`set_depth`
     - Force the depth this step.
   * - :meth:`set_lateral_inflow`
     - Inject a lateral inflow this step.
   * - :meth:`set_head_boundary`
     - Force a head boundary (outfalls).

These are **one-shot** — overwritten by the engine on the next step.
For sticky overrides that survive every step until cleared, use the
:class:`Forcing` API (:doc:`forcing`).

Water quality
-------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Action
   * - :meth:`get_quality`
     - Concentration of pollutant ``p`` at the node.
   * - :meth:`set_quality_mass_flux`
     - Inject a mass flux for pollutant ``p``.

Storage-node configuration
--------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action
   * - :meth:`get_storage_curve` / :meth:`set_storage_curve`
     - Storage curve (depth → area) by index.
   * - :meth:`get_storage_functional` / :meth:`set_storage_functional`
     - ``A·h^B + C`` storage-function coefficients.
   * - :meth:`get_storage_seep_rate` / :meth:`set_storage_seep_rate`
     - Constant seepage out the bottom.
   * - :meth:`get_exfil_params` / :meth:`set_exfil_params`
     - Green-Ampt exfiltration parameters.

Outfall-node configuration
--------------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action
   * - :meth:`get_outfall_type` / :meth:`set_outfall_type`
     - Free / fixed / tidal / time-series / normal.
   * - :meth:`set_outfall_stage`
     - Constant stage (for ``FIXED`` outfalls).
   * - :meth:`set_outfall_tidal`
     - Tidal curve index.
   * - :meth:`set_outfall_timeseries`
     - Time-series index.
   * - :meth:`get_outfall_param`
     - Generic outfall parameter (varies by type).
   * - :meth:`set_outfall_flap_gate`
     - Flap-gate present?

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, Nodes, NodeType

    with Solver("site_drainage.inp", "site_drainage.rpt", "site_drainage.out") as s:
        nodes = Nodes(s)
        print(f"Model has {nodes.count()} nodes")

        # enumerate junctions vs. outfalls
        for i in range(nodes.count()):
            kind = NodeType(nodes.get_type(i)).name
            print(f"  {i:3d}  {nodes.get_id(i):<12}  {kind}")

        # pick one and watch it
        j1 = nodes.get_index("J1")
        peak = 0.0
        while s.step():
            d = nodes.get_depth(j1)
            if d > peak:
                peak = d
                t_peak = s.elapsed
        print(f"J1 peak depth = {peak:.3f} at t={t_peak*24:.2f} h")

----

Common recipes
==============

Inject a constant lateral inflow into a junction
------------------------------------------------

One-shot per step (overwritten at the start of every step):

.. code-block:: python

    j1 = nodes.get_index("J1")
    while s.step():
        nodes.set_lateral_inflow(j1, 1.5)  # cfs (or m³/s, per FLOW_UNITS)

Sticky (preserved by the engine across steps):

.. code-block:: python

    from openswmm.engine import Forcing, ForcingMode

    forcing = Forcing(s)
    j1 = nodes.get_index("J1")
    forcing.node_lat_inflow(j1, 1.5, ForcingMode.REPLACE, persist=True)
    while s.step():
        pass
    forcing.clear_all()

Pulse: ramp inflow up between hours 1-3
---------------------------------------

.. code-block:: python

    j1 = nodes.get_index("J1")
    while s.step():
        h = s.elapsed * 24.0
        if 1.0 <= h <= 3.0:
            ramp = 1.0 - abs((h - 2.0) / 1.0)   # triangular peak at t=2 h
            nodes.set_lateral_inflow(j1, 5.0 * ramp)
        else:
            nodes.set_lateral_inflow(j1, 0.0)

Tide-driven outfall
-------------------

.. code-block:: python

    out1 = nodes.get_index("OUT1")
    nodes.set_outfall_type(out1, OutfallType.FIXED)

    while s.step():
        h = s.elapsed * 24.0
        # 12.42-hour M2 tide, mean stage 0, amplitude 1.5
        stage = 1.5 * math.sin(2 * math.pi * h / 12.42)
        nodes.set_head_boundary(out1, stage)

(For a real model, prefer :meth:`set_outfall_tidal` with a tidal curve
loaded from the ``.inp``.)

Walk every junction's full state in one pass
--------------------------------------------

.. code-block:: python

    for i in range(nodes.count()):
        if nodes.get_type(i) == NodeType.JUNCTION:
            print(
                f"{nodes.get_id(i):<12}  "
                f"invert={nodes.get_invert_elev(i):7.2f}  "
                f"max_d ={nodes.get_max_depth(i):5.2f}  "
                f"depth ={nodes.get_depth(i):5.2f}"
            )

Convert a junction to a storage node mid-edit
---------------------------------------------

See :doc:`editing` — type conversion is owned by :class:`ModelEditor`,
not :class:`Nodes`.

----

Bulk arrays
===========

The bulk methods exist for the **read-heavy** quantities you most often
want vectorised.  Each returns or accepts a contiguous
``np.ndarray[float64]`` of shape ``(n_nodes,)``.

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns / accepts
   * - :meth:`get_depths_bulk`
     - All node depths.
   * - :meth:`get_heads_bulk`
     - All node heads.
   * - :meth:`set_depths_bulk(arr)`
     - Force depths for every node from ``arr``.
   * - :meth:`get_inflows_bulk`
     - Total inflow per node.
   * - :meth:`get_overflows_bulk`
     - Overflow (flooding) per node.
   * - :meth:`set_lat_inflows_bulk(arr)`
     - Lateral inflow per node.
   * - :meth:`get_quality_bulk(p)`
     - Concentration of pollutant ``p`` per node.

Memory-aliasing rule: the array returned by a ``get_*_bulk`` method
shares memory with an internal scratch buffer that the engine reuses
on the **next** call.  Read-once-and-discard usage is safe; if you
keep the array (e.g. across a step), call ``.copy()``:

.. code-block:: python

    import numpy as np

    history = []
    while s.step():
        history.append(nodes.get_depths_bulk().copy())   # detach from scratch
    H = np.stack(history)              # shape (T, n_nodes)

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method
     - Required state
     - Notes
   * - ``count``, ``get_id``, ``get_index``
     - ``OPENED`` or later
     - Identity is fixed once the model is parsed.
   * - ``get_invert_elev``, ``get_max_depth``, …
     - ``OPENED`` or later
     - Geometry; not state.
   * - ``set_invert_elev``, ``set_max_depth``, …
     - ``OPENED`` or ``INITIALIZED``
     - Editing during a run is rarely meaningful; some setters refuse.
   * - ``get_depth``, ``get_head``, ``get_inflow``, …
     - ``RUNNING`` or ``ENDED``
     - Hydraulic state requires at least one step.
   * - ``set_depth``, ``set_lateral_inflow``, ``set_head_boundary``
     - ``RUNNING``
     - One-shot per step.
   * - ``set_quality_mass_flux``
     - ``RUNNING``
     - Requires a pollutant to be defined.
   * - ``*_bulk``
     - same as scalar
     - Same state rules as the per-element form.

Common :class:`EngineError` codes:

* ``INVALID_INDEX`` — integer index out of range.
* ``INVALID_TYPE`` — calling a storage / outfall accessor on a node of
  the wrong type (e.g. ``set_outfall_stage`` on a junction).
* ``NOT_FOUND`` — string id not in the model.

Use :doc:`error_handling` patterns for robust scripts.

----

See also
========

* :doc:`links` — analogous class for conduits / pumps / orifices / weirs / outlets.
* :doc:`forcing` — sticky cross-step forcings.
* :doc:`controls` — programmatic / rule-based actions.
* :doc:`editing` — adding, deleting, converting nodes.
* :doc:`output_reader` — read node time-series from a finished ``.out`` file.
