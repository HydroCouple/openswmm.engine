=====
Links
=====

.. note::

   **Engine:** OpenSWMM 6 — refactored.  This page documents the
   :class:`openswmm.engine.Links` class.  Legacy SWMM 5 users access
   links through the enum-driven ``getValue`` /
   ``setValue`` API on :class:`openswmm.legacy.engine.Solver` — see
   :doc:`../legacy/solver`.

.. currentmodule:: openswmm.engine

Conduits, pumps, orifices, weirs, and outlets — every connection
between two nodes.  The :class:`Links` class is the single entry point
for reading link properties at configure time and link state during a
running simulation.

Reference: ``openswmm_links.h``.

----

Class signature
===============

.. code-block:: python

    class Links:
        def __init__(self, solver: Solver) -> None: ...

* ``solver`` — an active :class:`Solver`.

A single :class:`Links` instance covers **every** link in the model.
Work via integer indices (fast) or string ids (convenient).

----

Key methods
===========

Identity & topology
-------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`count`
     - Number of links.
   * - :meth:`get_index(id)`
     - Integer index for a string id.
   * - :meth:`get_id(idx)`
     - String id for an integer index.
   * - :meth:`get_type(idx)`
     - :class:`LinkType` (CONDUIT, PUMP, …).
   * - :meth:`get_from_node(idx)`
     - Upstream node integer index.
   * - :meth:`get_to_node(idx)`
     - Downstream node integer index.

Geometry
--------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`get_length`
     - Conduit length, model length units.
   * - :meth:`get_roughness`
     - Manning's *n*.
   * - :meth:`get_slope`
     - Slope (computed from invert offsets).
   * - :meth:`get_offset_up`
     - Upstream offset above the from-node invert.
   * - :meth:`get_offset_dn`
     - Downstream offset above the to-node invert.
   * - :meth:`get_xsect`
     - ``(shape, geom1, geom2, geom3, geom4)``.

Each ``get_*`` has a matching ``set_*`` valid in ``OPENED``.  The
:meth:`set_link_xsect` flavour from :class:`ModelBuilder` is preferred
when constructing a model from scratch.

Hydraulic state
---------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`get_flow`
     - Current flow.
   * - :meth:`get_depth`
     - Current depth at the link midpoint.
   * - :meth:`get_velocity`
     - Current cross-sectional velocity.
   * - :meth:`get_capacity`
     - Fraction of full-depth capacity used.
   * - :meth:`get_volume`
     - Current volume in the link.

Control settings
----------------

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Method
     - Action
   * - :meth:`set_control_setting` / :meth:`get_control_setting`
     - Active control setting (0–1).
   * - :meth:`set_target_setting` / :meth:`get_target_setting`
     - Target setting (controls ramp toward).
   * - :meth:`set_closed` / :meth:`get_closed`
     - Force link fully closed / open.

Pump-specific
-------------

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Method
     - Action / returns
   * - :meth:`get_pump_curve` / :meth:`set_pump_curve`
     - Pump curve index.
   * - :meth:`get_pump_init_state` / :meth:`set_pump_init_state`
     - Initial on/off state.

Weir / orifice
--------------

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Method
     - Action / returns
   * - :meth:`get_crest_height` / :meth:`set_crest_height`
     - Crest height above invert.
   * - :meth:`get_discharge_coeff` / :meth:`set_discharge_coeff`
     - Discharge coefficient.
   * - :meth:`get_end_contractions` / :meth:`set_end_contractions`
     - Number of end contractions (rect weirs).
   * - :meth:`get_loss_coeff` / :meth:`set_loss_coeff`
     - Entrance / mid / exit loss coefficients.
   * - :meth:`get_flap_gate` / :meth:`set_flap_gate`
     - Has flap gate?

Other
-----

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Method
     - Action / returns
   * - :meth:`set_initial_flow`
     - Initial flow at start of simulation.
   * - :meth:`set_max_flow`
     - Cap on link flow magnitude.
   * - :meth:`set_seep_rate` / :meth:`get_seep_rate`
     - Seepage rate (conduits).
   * - :meth:`set_culvert_code`
     - HDS-5 culvert geometry code.
   * - :meth:`set_flow`
     - Force flow this step (one-shot).

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, Links, Nodes, LinkType

    with Solver("site_drainage.inp", "site_drainage.rpt", "site_drainage.out") as s:
        links = Links(s)
        nodes = Nodes(s)
        print(f"Model has {links.count()} links")

        # describe the network
        for i in range(links.count()):
            kind = LinkType(links.get_type(i)).name
            up = nodes.get_id(links.get_from_node(i))
            dn = nodes.get_id(links.get_to_node(i))
            print(f"  {links.get_id(i):<12}  {kind:<10}  {up} → {dn}")

        # watch a single conduit
        c1 = links.get_index("C1")
        peak_q = 0.0
        while s.step():
            q = links.get_flow(c1)
            if q > peak_q:
                peak_q, t_peak = q, s.elapsed
        print(f"C1 peak flow = {peak_q:.3f} cfs at t={t_peak*24:.2f} h")

----

Common recipes
==============

Open / close a regulator on schedule
------------------------------------

.. code-block:: python

    gate = links.get_index("G1")
    while s.step():
        h = s.elapsed * 24.0
        # close the gate during peak rainfall (hour 6-9)
        links.set_target_setting(gate, 0.0 if 6.0 <= h <= 9.0 else 1.0)

(For declarative / rule-based control, prefer :doc:`controls`.)

Pump on / off based on upstream depth
-------------------------------------

.. code-block:: python

    p1   = links.get_index("PUMP1")
    sumi = nodes.get_index("WET_WELL")
    on_threshold, off_threshold = 4.0, 1.5
    pumping = False
    while s.step():
        d = nodes.get_depth(sumi)
        if not pumping and d >= on_threshold:
            pumping = True
            links.set_target_setting(p1, 1.0)
        elif pumping and d <= off_threshold:
            pumping = False
            links.set_target_setting(p1, 0.0)

Re-jig a conduit's geometry mid-warm-up
---------------------------------------

.. code-block:: python

    from openswmm.engine import XSectShape

    # Done once after open(), before initialize()
    s.open()
    c1 = links.get_index("C1")
    links.set_link_xsect(c1, XSectShape.CIRCULAR, 1.5)   # bump from 1.0 to 1.5 m
    s.initialize()
    s.start()
    # ... loop ...

Identify pipes that ran over capacity
-------------------------------------

.. code-block:: python

    overcap = []
    while s.step():
        for i in range(links.count()):
            if links.get_capacity(i) >= 0.999 and i not in overcap:
                overcap.append(i)
    print("overcapacity:", [links.get_id(i) for i in overcap])

(For accumulated link statistics, prefer :doc:`statistics` —
``Statistics.link_stats(...)`` exposes this directly.)

----

Bulk arrays
===========

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns / accepts
   * - :meth:`get_flows_bulk`
     - All link flows (``np.ndarray[float64]``, shape ``(n_links,)``).
   * - :meth:`set_flows_bulk(arr)`
     - Force flows for every link.
   * - :meth:`get_depths_bulk`
     - All link mid-point depths.
   * - :meth:`get_quality_bulk(p)`
     - Pollutant ``p`` concentration per link.

Same memory-aliasing rule as :class:`Nodes`: the returned array shares
memory with engine scratch space; ``.copy()`` if you keep it past the
next call.

Vectorised peak detection across all links:

.. code-block:: python

    import numpy as np

    peaks = np.zeros(links.count(), dtype=np.float64)
    while s.step():
        peaks = np.maximum(peaks, np.abs(links.get_flows_bulk()))

    for i, q in enumerate(peaks):
        print(f"  {links.get_id(i):<12}  peak |Q| = {q:.3f}")

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 35 25 40

   * - Method group
     - Required state
     - Notes
   * - identity / topology accessors
     - ``OPENED`` or later
     - Topology is fixed once parsed.
   * - geometry accessors (``get_*``)
     - ``OPENED`` or later
     - n/a
   * - geometry setters (``set_*``)
     - ``OPENED``
     - Modifying mid-run is generally invalid.
   * - hydraulic state accessors
     - ``RUNNING`` or ``ENDED``
     - Need at least one step.
   * - control setters
     - ``RUNNING``
     - One-shot; overwritten if a control rule fires next step.
   * - pump / weir / orifice config
     - ``OPENED`` or ``RUNNING``
     - Some setters check ``LinkType`` and raise
       ``INVALID_TYPE`` for the wrong link kind.
   * - ``*_bulk`` accessors
     - same as scalar
     - n/a

Common :class:`EngineError` codes:

* ``INVALID_INDEX``  — integer index out of range.
* ``INVALID_TYPE``   — calling a pump-only accessor on a conduit, etc.
* ``NOT_FOUND``      — string id not in the model.

----

See also
========

* :doc:`nodes` — the upstream / downstream endpoints.
* :doc:`controls` — declarative rules for open / close / setting changes.
* :doc:`forcing` — sticky cross-step overrides on a link's flow.
* :doc:`statistics` — accumulated peak flow, peak depth, hours
  surcharged, etc., per link.
* :doc:`output_reader` — link time-series from a finished ``.out`` file.
