=====
Nodes
=====

.. note::

   **Engine:** OpenSWMM 6 — refactored. This page documents the
   :class:`openswmm.engine.Nodes` collection and the
   :class:`openswmm.engine._nodes.Node` wrapper objects it produces.
   Legacy SWMM 5 users access nodes through the enum-driven
   ``getValue`` / ``setValue`` API on
   :class:`openswmm.legacy.engine.Solver` — see :doc:`../legacy/solver`.

.. currentmodule:: openswmm.engine

Junctions, outfalls, storage units, and dividers — every point where
flow enters, leaves, or is stored in the network. The :class:`Nodes`
collection hangs off the Solver as a lazy attribute:

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        s.nodes               # → Nodes (the collection)
        s.nodes["J1"]         # → Node  (a wrapper for one node)
        s.nodes[0]            # → Node  (same shape; by integer index)

Reference: ``openswmm_nodes.h``.

----

Quickstart
==========

Five idioms cover most use cases.

.. code-block:: python

    # 1. Indexing — both ``int`` and ``str`` work.
    j1 = s.nodes["J1"]
    same = s.nodes[0]                  # same Node identity (different wrapper)
    assert j1 == same

    # 2. Iteration.
    for node in s.nodes:
        print(node.id, node.invert_elev)

    # 3. Per-object property access.
    print(s.nodes["J1"].depth)
    s.nodes["J1"].lateral_inflow = 0.5

    # 4. Bulk vectorised access.
    depths = s.nodes.depths            # numpy float64 array
    s.nodes.depths = depths * 1.05

    # 5. Type-specific sub-views.
    s.nodes["OUT1"].outfall.type = OutfallType.FIXED
    s.nodes["S1"].storage.functional = (0.0, 0.0, 100.0)
    s.nodes["J1"].stats.max_depth      # cumulative stats

----

Collection: :class:`Nodes`
==========================

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Operation
     - What it does
   * - ``len(s.nodes)``
     - Current node count.
   * - ``s.nodes[key]``
     - Returns a :class:`Node`. ``key`` is ``int`` or ``str``; unknown
       string raises :exc:`KeyError`, out-of-range int raises
       :exc:`IndexError`.
   * - ``for n in s.nodes:``
     - Yields a fresh :class:`Node` per index.
   * - ``key in s.nodes``
     - Membership by ``int`` or ``str``.
   * - ``s.nodes.get_index(id)``
     - String → int. Raises :exc:`KeyError` if absent.
   * - ``s.nodes.get_id(idx)``
     - Int → string. Raises :exc:`IndexError` if out of range.
   * - ``s.nodes.add(id, type)``
     - Append a node (``BUILDING`` / ``OPENED`` only). Returns the
       :class:`Node` and bumps the generation counter.
   * - ``s.nodes.pop_last(id)``
     - Remove the most recently added node.
   * - ``s.nodes.rename(key, new_id)``
     - Rename in place. Invalidates any :class:`Node` wrappers held by
       Python code.

Bulk numpy properties
---------------------

Every per-node array is exposed as a property whose getter returns a
fresh ``float64`` array of shape ``(n_nodes,)`` and whose setter (if
present) accepts the same shape:

.. list-table::
   :header-rows: 1
   :widths: 22 14 64

   * - Property
     - Mode
     - What it carries
   * - ``depths``
     - read/write
     - Water depth above invert.
   * - ``heads``
     - read-only
     - Hydraulic head.
   * - ``inflows``
     - read-only
     - Total inflow.
   * - ``overflows``
     - read-only
     - Flooding / overflow rate.
   * - ``volumes``
     - read-only
     - Stored volume.
   * - ``outflows``
     - read-only
     - Total outflow.
   * - ``losses``
     - read-only
     - Evaporation + seepage losses.
   * - ``lateral_inflows``
     - read-only
     - Per-node lateral inflows.
   * - ``ids``
     - read-only
     - String ids as a ``numpy.ndarray`` of ``dtype=object``.

For lateral-inflow forcing, use the explicit method:

.. code-block:: python

    s.nodes.set_lateral_inflows(arr)         # float64, shape (n_nodes,)

For per-pollutant concentrations, use:

.. code-block:: python

    concs = s.nodes.qualities("TSS")         # or by pollutant index

----

Wrapper: :class:`Node`
======================

The wrapper is the natural place to read and write a single node. All
properties round-trip directly through the C API — no Python-side
caching:

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
     - Node identifier.
   * - ``index``
     - ``int``
     - read-only
     - Position in the engine's node array.
   * - ``type``
     - :class:`NodeType`
     - read-only
     - JUNCTION / OUTFALL / STORAGE / DIVIDER.
   * - ``invert_elev``
     - ``float``
     - read/write
     - Invert elevation.
   * - ``max_depth``
     - ``float``
     - read/write
     -
   * - ``surcharge_depth``
     - ``float``
     - read/write
     -
   * - ``ponded_area``
     - ``float``
     - read/write
     -
   * - ``initial_depth``
     - ``float``
     - read/write
     -
   * - ``crown_elev``
     - ``float``
     - read-only
     - Top-of-crown elevation derived from connected links.
   * - ``full_volume``
     - ``float``
     - read-only
     - Storage volume at ``max_depth``.
   * - ``degree``
     - ``int``
     - read-only
     - Number of links incident to the node.
   * - ``depth``
     - ``float``
     - read/write
     - Runtime depth above invert.
   * - ``head``
     - ``float``
     - read-only
     - Runtime hydraulic head.
   * - ``volume``
     - ``float``
     - read-only
     -
   * - ``lateral_inflow``
     - ``float``
     - read/write
     - One-shot lateral inflow for the next step.
   * - ``overflow``
     - ``float``
     - read-only
     -
   * - ``inflow``
     - ``float``
     - read-only
     - Total inflow.
   * - ``losses``
     - ``float``
     - read-only
     -
   * - ``outflow``
     - ``float``
     - read-only
     -

Methods:

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Method
     - What it does
   * - ``set_head_boundary(head)``
     - One-shot head boundary for the next step.
   * - ``quality(pollutant)``
     - Concentration of ``pollutant`` (id or int).
   * - ``set_quality_mass_flux(pollutant, mass_rate)``
     - Inject a mass flux.
   * - ``depth_from_volume(volume)``
     - Storage-curve inverse lookup.

----

Type-specific sub-views
=======================

Each node type exposes a small sub-namespace. Accessing the wrong one
raises :exc:`AttributeError` — the wrong sub-view is **not** silently
returned:

.. code-block:: python

    # OUTFALL nodes only.
    out = s.nodes["OUT1"]
    out.outfall.type = OutfallType.FIXED
    out.outfall.set_stage(123.4)
    out.outfall.flap_gate = True
    out.outfall.route_to                  # subcatchment index, or -1

    # STORAGE nodes only.
    sto = s.nodes["S1"]
    sto.storage.curve                     # curve index, or -1 if functional
    sto.storage.functional = (a, b, c)    # ax² + bx + c
    sto.storage.seep_rate = 0.01
    sto.storage.exfil_params = (suction, ksat, imd)

    # DIVIDER nodes only.
    div = s.nodes["D1"]
    div.divider.type                       # integer divider type code

----

Statistics sub-view
===================

Every node carries a ``.stats`` sub-view exposing the cumulative
flooding/overflow statistics aggregated by the engine:

.. code-block:: python

    j1 = s.nodes["J1"]
    print(j1.stats.max_depth)
    print(j1.stats.max_overflow)
    print(j1.stats.vol_flooded)            # cubic feet (US) / m³ (SI)
    print(j1.stats.time_flooded)           # seconds; divide by 3600 for hours

These values are only meaningful after :meth:`Solver.end` (or
equivalently, after the simulation loop terminates).

----

Staleness & generation counter
==============================

Adding, removing, renaming, or converting nodes invalidates every
:class:`Node` wrapper minted **before** the change. Touching a stale
wrapper raises :class:`StaleObjectError` (which is also a
:class:`LifecycleError` and therefore a :exc:`RuntimeError`):

.. code-block:: python

    j1 = s.nodes["J1"]
    s.nodes.rename(0, "J1_RENAMED")
    j1.depth                              # raises StaleObjectError

Recovery is to re-look-up by the new id (or by index):

.. code-block:: python

    j1 = s.nodes["J1_RENAMED"]
    j1.depth                              # OK

Per-property reads/writes are cheap, so don't try to cache wrappers
across mutations. The collection itself never goes stale — it always
addresses the current state of the model.

----

Equality, hashing, repr
=======================

* ``a == b`` is ``True`` when ``a`` and ``b`` wrap the same ``(solver,
  index)`` pair, regardless of when each wrapper was minted.
* ``hash(node)`` is consistent with equality, so :class:`Node` is usable
  as a dict key / set element within a single Solver.
* ``repr(node)`` includes the captured id and integer index, e.g.
  ``<Node id='J1' index=0>``.

----

See also
========

* :doc:`solver` — where ``s.nodes`` comes from.
* :doc:`links` — link wrappers follow the same pattern.
* :doc:`output_reader` — node time-series after the run.
* :doc:`error_handling` — every exception type referenced on this page.
