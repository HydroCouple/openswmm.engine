=====
Links
=====

.. note::

   **Engine:** OpenSWMM 6 — refactored. This page documents the
   :class:`openswmm.engine.Links` collection and the
   :class:`openswmm.engine._links.Link` wrapper. Legacy SWMM 5 users
   access links through the enum-driven ``getValue`` / ``setValue`` API
   on :class:`openswmm.legacy.engine.Solver` — see
   :doc:`../legacy/solver`.

.. currentmodule:: openswmm.engine

Conduits, pumps, orifices, weirs, and outlets — every connection
between two nodes. The shape mirrors :doc:`nodes`:

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        s.links             # → Links collection
        s.links["C1"]       # → Link wrapper
        s.links[0]          # → Link wrapper (by index)

Reference: ``openswmm_links.h``.

----

Quickstart
==========

.. code-block:: python

    # Indexing.
    c1 = s.links["C1"]

    # Iteration.
    for link in s.links:
        print(link.id, link.type.name, link.length)

    # Per-object property access.
    print(c1.flow, c1.depth, c1.velocity)
    c1.control_setting = 0.5
    c1.closed = True

    # Topology — yields Node wrappers, not bare indices.
    print(c1.from_node.id, "->", c1.to_node.id)

    # Bulk numpy access.
    flows = s.links.flows               # np.ndarray
    s.links.flows = flows * 0.95

    # Cross-section.
    c1.xsect = (XSectShape.CIRCULAR, 1.0, 0.0, 0.0, 0.0)
    print(c1.xsect.shape, c1.xsect.g1)

    # Type-specific sub-views — raise AttributeError on wrong type.
    s.links["P1"].pump.curve = 0
    s.links["W1"].weir.type = WeirType.TRANSVERSE
    s.links["OR1"].orifice.type = OrificeType.BOTTOM
    s.links["OUT1"].outlet.rating_type = OutletRatingType.FUNCTIONAL_HEAD

    # Stats.
    print(s.links["C1"].stats.max_flow, s.links["C1"].stats.max_velocity)

----

Collection: :class:`Links`
==========================

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Operation
     - What it does
   * - ``len(s.links)``
     - Current link count.
   * - ``s.links[key]``
     - Returns a :class:`Link`. ``key`` is ``int`` or ``str``.
   * - ``for l in s.links:``
     - Yields a fresh :class:`Link` per index.
   * - ``s.links.get_index(id)``
     - String → int. Raises :exc:`KeyError`.
   * - ``s.links.get_id(idx)``
     - Int → string. Raises :exc:`IndexError`.
   * - ``s.links.add(id, type)``
     - Append a link. Returns the :class:`Link` and bumps generation.
   * - ``s.links.pop_last(id)``
     - Remove the most recently added link.
   * - ``s.links.rename(key, new_id)``
     - Rename in place. Invalidates wrappers.

Bulk numpy properties
---------------------

.. list-table::
   :header-rows: 1
   :widths: 24 14 62

   * - Property
     - Mode
     - What it carries
   * - ``flows``
     - read/write
     - Per-link flow rate.
   * - ``depths``
     - read-only
     - Water depth.
   * - ``velocities``
     - read-only
     -
   * - ``capacities``
     - read-only
     - Flow / full-flow ratio.
   * - ``volumes``
     - read-only
     -
   * - ``control_settings``
     - read-only
     -
   * - ``target_settings``
     - read-only
     -
   * - ``hyd_powers``
     - read-only
     - Hydraulic power (for pumps).
   * - ``ids``
     - read-only
     - String ids as ``numpy.ndarray`` (``dtype=object``).

For per-pollutant concentrations:

.. code-block:: python

    s.links.qualities("TSS")     # str id or int index

For pump statistics in bulk:

.. code-block:: python

    cycles, on_time, volume = s.links.pump_stats()

----

Wrapper: :class:`Link`
======================

.. list-table::
   :header-rows: 1
   :widths: 22 14 14 50

   * - Property
     - Type
     - Mode
     - Meaning
   * - ``id``
     - ``str``
     - read-only
     -
   * - ``index``
     - ``int``
     - read-only
     -
   * - ``type``
     - :class:`LinkType`
     - read-only
     - CONDUIT / PUMP / ORIFICE / WEIR / OUTLET.
   * - ``from_node``
     - :class:`Node`
     - read-only
     - Upstream node wrapper.
   * - ``to_node``
     - :class:`Node`
     - read-only
     - Downstream node wrapper.
   * - ``length``
     - ``float``
     - read/write
     - Conduit length (other types use slot for type-specific data).
   * - ``roughness``
     - ``float``
     - read/write
     -
   * - ``slope``
     - ``float``
     - read-only
     - Computed from invert elevations.
   * - ``offset_up`` / ``offset_dn``
     - ``float``
     - read/write
     -
   * - ``initial_flow`` / ``max_flow``
     - ``float``
     - read/write
     -
   * - ``xsect``
     - :class:`XSection`
     - read/write
     - Assign ``(shape, g1, g2, g3, g4)`` to write through.
   * - ``flow``
     - ``float``
     - read/write
     -
   * - ``depth`` / ``velocity`` / ``capacity`` / ``volume``
     - ``float``
     - read-only
     -
   * - ``slot_volume``
     - ``float``
     - read-only
     - Water in the Preissmann slot — the part of ``volume`` above the
       crown. Finite-volume routing only; ``0.0`` under dynamic wave.
   * - ``hyd_power``
     - ``float``
     - read-only
     -
   * - ``control_setting`` / ``target_setting``
     - ``float``
     - read/write
     -
   * - ``closed``
     - ``bool``
     - read/write
     -
   * - ``loss_coeff``
     - ``(inlet, outlet, avg)``
     - read/write
     -
   * - ``flap_gate`` / ``seep_rate`` / ``culvert_code`` / ``barrels``
     - mixed
     - read/write
     - Conduit-flavoured knobs.

Methods:

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Method
     - What it does
   * - ``set_nodes(from_node, to_node)``
     - Reconnect this link. Accepts indices, ids, or :class:`Node` wrappers.
   * - ``quality(pollutant)``
     - Concentration of ``pollutant`` (id or int).

----

Cross-section: :class:`XSection`
================================

``link.xsect`` returns an :class:`XSection` view. Access individual
fields by attribute, the snapshot tuple via ``as_tuple()``, or rewrite
all five fields by assigning a tuple:

.. code-block:: python

    x = c1.xsect
    print(x.shape, x.g1, x.g2, x.g3, x.g4)
    shape, g1, g2, g3, g4 = x.as_tuple()

    c1.xsect = (XSectShape.CIRCULAR, 1.0, 0.0, 0.0, 0.0)

The shape is a :class:`XSectShape` enum; the four ``g`` parameters are
shape-specific (diameter, width, height, depth, …).

Two companions save you from decoding the ``g`` values by hand.
``x.info()`` (or :meth:`Links.get_xsect_info`) labels them, and
``x.geometry()`` returns an :class:`XSectionGeometry` that can answer
hydraulic questions about the section:

.. code-block:: python

    c1.xsect.info().geom_labels     # {'diameter': 1.0}

    g = c1.xsect.geometry()
    g.area(0.5), g.full_area, g.is_open

See :doc:`xsect_geometry` for the full geometry surface.

----

Type-specific sub-views
=======================

Each link type exposes a small sub-namespace; accessing the wrong one
raises :exc:`AttributeError`:

.. code-block:: python

    # PUMP only.
    p = s.links["P1"]
    p.pump.curve = 0                       # curve index
    p.pump.init_state = True               # starts on
    p.pump.startup_depth = 1.0
    p.pump.shutoff_depth = 0.1

    # WEIR only.
    w = s.links["W1"]
    w.weir.type = WeirType.TRANSVERSE
    w.weir.crest_height = 0.5
    w.weir.discharge_coeff = 3.33
    w.weir.end_contractions = 2

    # ORIFICE only.
    o = s.links["OR1"]
    o.orifice.type = OrificeType.BOTTOM
    o.orifice.open_close_rate = 0.5        # 1 / second

    # OUTLET only.
    out = s.links["OUT1"]
    out.outlet.rating_type = OutletRatingType.FUNCTIONAL_HEAD
    out.outlet.expon = 0.5

----

Statistics sub-view
===================

Every link carries a ``.stats`` view; pump-specific entries raise
:class:`BadParamError` (which is also a :exc:`ValueError`) on non-pump
links:

.. code-block:: python

    c1 = s.links["C1"]
    print(c1.stats.max_flow)
    print(c1.stats.max_velocity)
    print(c1.stats.max_filling)
    print(c1.stats.vol_flow)
    print(c1.stats.surcharge_time)
    print(c1.stats.peak_slot_share)
    print(c1.stats.slot_share)

    p1 = s.links["P1"]
    print(p1.stats.pump_cycles)
    print(p1.stats.pump_on_time)
    print(p1.stats.pump_volume)

These values are only meaningful after :meth:`Solver.end`.

``peak_slot_share`` is the peak instantaneous ``slot_volume / volume``
over the run; ``slot_share`` is the run-level
``(∫ slot_volume dt) / (∫ volume dt)`` — a ratio of time integrals, never
an average of instantaneous ratios. Both are ``0..1``, and both are
finite-volume-routing only: under the dynamic-wave router they read
``0.0``, which is indistinguishable from "no slot flow occurred".

----

Staleness, equality, repr
=========================

Same contract as :class:`Node`:

* Adding, removing, renaming, or converting links invalidates every
  :class:`Link` minted before. Access raises :class:`StaleObjectError`.
* ``a == b`` is ``True`` when ``a`` and ``b`` wrap the same ``(solver,
  index)`` pair; hashing is consistent.
* ``repr(link)`` includes the captured id and index.

----

See also
========

* :doc:`solver` — where ``s.links`` comes from.
* :doc:`nodes` — :attr:`Link.from_node` / :attr:`Link.to_node` return :class:`Node` wrappers.
* :doc:`xsect_geometry` — ``link.xsect.geometry()`` and standalone
  cross-section geometry.
* :doc:`controls` — runtime control of link ``control_setting`` and
  ``target_setting``.
* :doc:`error_handling` — every exception type referenced on this page.
