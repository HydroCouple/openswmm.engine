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

    from datetime import timedelta
    from openswmm.engine import Solver

    with Solver("model.inp", "model.rpt", "model.out") as s:
        for elapsed in s.steps():
            # elapsed is a datetime.timedelta after the most recent step.
            if elapsed >= timedelta(hours=24):
                break

Failures raise :class:`~openswmm.engine.EngineError` (or a subclass like
:class:`~openswmm.engine.LifecycleError`) — no integer return codes to
check.

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

    from openswmm.engine import Solver

    with Solver("model.inp", "model.rpt", "model.out") as s:
        j1 = s.nodes.get_index("J1")           # name → integer index
        c1 = s.links.get_index("C1")

        for elapsed in s.steps():
            depth_at_j1 = s.nodes.get_depth(j1)
            flow_in_c1  = s.links.get_flow(c1)
            print(f"t={s.current_datetime}  J1.depth={depth_at_j1:.3f}  C1.flow={flow_in_c1:.3f}")

The accessors accept **either** an integer index or a string id, so
``s.nodes.get_depth("J1")`` works too.  The integer form is faster in
tight loops because it skips the name lookup.

.. note::

   The full object-wrapper surface (``s.nodes["J1"].depth`` etc.) lands
   in later phases. For now the collection attributes return the legacy
   ``Nodes`` / ``Links`` helpers.

----

3. Vectorise with NumPy bulk methods
=====================================

Per-element accessors cross the Python/C boundary once per call.  When
you want every node's depth in one go, use the ``*_bulk`` family — they
return a contiguous :class:`numpy.ndarray` filled in one C call:

.. code-block:: python

    import numpy as np
    from openswmm.engine import Solver

    with Solver("model.inp", "model.rpt", "model.out") as s:
        depths_history = []
        flows_history  = []

        for _ in s.steps():
            depths_history.append(s.nodes.get_depths_bulk().copy())
            flows_history.append(s.links.get_flows_bulk().copy())

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

    s.nodes.set_lateral_inflow("J1", 1.5)     # cfs (or m³/s, per FLOW_UNITS)

Sticky override that survives every step until you clear it::

    from openswmm.engine import ForcingMode

    s.forcing.node_lat_inflow("J1", 1.5, ForcingMode.REPLACE, persist=True)
    # … run …
    s.forcing.clear_all()

See :doc:`forcing` for the full forcing model.

----

5. Read the binary ``.out`` file after the run
==============================================

After the run completes, point an :class:`~openswmm.engine.OutputReader`
at the ``.out`` file to query time-series data:

.. code-block:: python

    from openswmm.engine import OutputReader, OutNodeVar

    with OutputReader("model.out") as reader:
        n = reader.get_period_count()
        # Resolve "J1" to its zero-based index by scanning ids
        j1 = next(
            i for i in range(reader.get_node_count())
            if reader.get_node_id(i) == "J1"
        )
        depth_series = reader.get_node_series(
            j1, OutNodeVar.DEPTH, start=0, end=n - 1,
        )
        print(n, "steps, depth peak =", float(depth_series.max()))

The :class:`OutputReader` is independent of any running solver — you
can use it on a file produced by any past run.

----

6. Build a model in Python (no ``.inp`` file required)
======================================================

The :class:`~openswmm.engine.ModelBuilder` constructs a complete model
programmatically:

.. code-block:: python

    from openswmm.engine import (
        ModelBuilder, NodeType, LinkType, XSectShape, EngineState,
    )

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
    for _ in solver.steps():
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
