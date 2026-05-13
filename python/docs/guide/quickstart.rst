==========
Quickstart
==========

.. note::

   **Engine:** OpenSWMM 6 — refactored.  All examples on this page use
   :mod:`openswmm.engine`.  For the legacy SWMM 5 API see
   :doc:`../legacy/index`.

A 10-minute walkthrough that runs a small SWMM model end-to-end.

If you have not yet installed the package, see :doc:`install`.

----

1. Run a model from a ``.inp`` file
====================================

The :class:`~openswmm.engine.Solver` is the entry point.  It owns the
engine handle, loads the model, and drives the simulation forward in time.

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp", "model.rpt", "model.out") as s:
        while s.step():
            pass            # advance one routing step

The context manager:

* Calls ``open()`` → ``initialize()`` → ``start()`` on entry.
* Calls ``end()`` → ``report()`` → ``close()`` → ``destroy()`` on exit,
  so the ``.rpt`` and ``.out`` files are flushed and the engine handle
  released even if your loop raises.

If you do not need a binary output file, pass an empty string for the
``out`` argument: ``Solver("model.inp", "model.rpt", "")``.

----

2. Query node and link state during the run
============================================

Domain classes are instantiated against an open solver.  They never
duplicate state — every accessor is a thin call into the engine.

.. code-block:: python

    from openswmm.engine import Solver, Nodes, Links

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes = Nodes(s)
        links = Links(s)

        j1 = nodes.get_index("J1")          # name → integer index
        c1 = links.get_index("C1")

        while s.step():
            depth_at_j1 = nodes.get_depth(j1)
            flow_in_c1  = links.get_flow(c1)
            print(f"t={s.elapsed:.4f} d  J1.depth={depth_at_j1:.3f}  C1.flow={flow_in_c1:.3f}")

The accessors accept **either** an integer index or a string id, so you
can write ``nodes.get_depth("J1")`` directly.  The integer form is
faster in tight loops because it skips the name lookup.

----

3. Vectorise with NumPy bulk methods
=====================================

Per-element accessors cross the Python/C boundary once per call.  When
you want every node's depth in one go, use the ``*_bulk`` family — they
return a contiguous :class:`numpy.ndarray` filled in one C call:

.. code-block:: python

    import numpy as np
    from openswmm.engine import Solver, Nodes, Links

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes = Nodes(s)
        links = Links(s)

        depths_history = []
        flows_history  = []

        while s.step():
            depths_history.append(nodes.get_depths_bulk().copy())
            flows_history.append(links.get_flows_bulk().copy())

        depths = np.stack(depths_history)   # shape (T, n_nodes)
        flows  = np.stack(flows_history)    # shape (T, n_links)

The ``.copy()`` is intentional — the returned array shares memory with
an internal scratch buffer that the engine reuses on the next call.
If you only read it once and discard it, the copy is unnecessary.

----

4. Inject runtime forcings
==========================

To override an inflow, rainfall, or rule action without editing the
``.inp`` file, use the per-domain setters or the
:class:`~openswmm.engine.Forcing` API.

Lateral inflow (one-shot — overwritten by the engine on the next step)::

    nodes.set_lateral_inflow("J1", 1.5)     # cfs (or m³/s, per FLOW_UNITS)

Sticky override that survives every step until you clear it::

    from openswmm.engine import Forcing, ForcingMode

    forcing = Forcing(s)
    forcing.node_lat_inflow("J1", 1.5, ForcingMode.REPLACE, persist=True)
    # … run …
    forcing.clear_all()

See :doc:`forcing` for the full forcing model.

----

5. Read the binary ``.out`` file after the run
==============================================

After the run completes, point an :class:`~openswmm.engine.OutputReader`
at the ``.out`` file to query time-series data:

.. code-block:: python

    from openswmm.engine import OutputReader, OutNodeVar

    reader = OutputReader("model.out")
    times = reader.times()                              # list[datetime]
    depth_series = reader.node_series("J1", OutNodeVar.DEPTH)
    print(len(times), "steps, depth peak =", max(depth_series))

The :class:`OutputReader` is independent of any running solver — you
can use it on a file produced by any past run.

----

6. Build a model in Python (no ``.inp`` file required)
======================================================

The :class:`~openswmm.engine.ModelBuilder` constructs a complete model
programmatically:

.. code-block:: python

    from openswmm.engine import ModelBuilder, NodeType, LinkType, XSectShape

    m = ModelBuilder()
    m.add_node("J1", NodeType.JUNCTION)
    m.add_node("OUT1", NodeType.OUTFALL)
    m.add_link("C1", LinkType.CONDUIT)
    m.set_link_nodes(0, 0, 1)              # link 0 from node 0 to node 1
    m.set_link_length(0, 300.0)
    m.set_link_roughness(0, 0.013)
    m.set_link_xsect(0, XSectShape.CIRCULAR, 1.0)
    m.validate()
    m.finalize()

    solver = m.to_solver()                 # ready to run
    solver.start()
    while solver.step():
        pass
    solver.end()
    solver.destroy()

See :doc:`model_builder` for the full builder API and :doc:`editing`
for in-place modification of an existing model.

----

Where to next?
==============

* :doc:`concepts` — solver lifecycle, EngineState transitions, and the
  exception model.  Read this if any of your calls raise
  :class:`~openswmm.engine.EngineError`.
* :doc:`solver` — every solver method explained, including callbacks
  and progress reporting.
* The per-domain pages (:doc:`nodes`, :doc:`links`, …) — every
  ``get_*`` / ``set_*`` accessor with worked examples.
* :doc:`../migration/swmm5_to_swmm6` — translating existing SWMM 5
  Python code to the v6.0 engine.
