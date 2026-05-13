===========================
Concepts & engine lifecycle
===========================

.. note::

   **Engine:** OpenSWMM 6 — refactored.  This page describes the
   reentrant :mod:`openswmm.engine` engine.  The legacy SWMM 5
   solver has its own (simpler, non-reentrant) lifecycle — see
   :doc:`../legacy/solver`.

Most user-visible behaviour follows from three concepts:

1. The :class:`~openswmm.engine.Solver` owns the engine handle and
   transitions through a deterministic sequence of states.
2. Every domain class (:class:`Nodes`, :class:`Links`, …) holds a
   reference to a Solver and is only valid in certain states.
3. C-API errors surface as :class:`~openswmm.engine.EngineError`
   exceptions; nothing fails silently.

Get these three right and the rest of the API maps directly onto the
underlying C headers.

----

The engine is reentrant
=======================

Unlike SWMM 5, OpenSWMM 6 has **no global state**.  Every Solver instance
owns an opaque ``SWMM_Engine`` handle, so multiple independent
simulations can run side-by-side in the same process — useful for
ensemble forecasting, parameter sweeps, or driving SWMM from a multi-
process optimiser.

.. code-block:: python

    from openswmm.engine import Solver

    s1 = Solver("scenario_a.inp", "a.rpt", "a.out")
    s2 = Solver("scenario_b.inp", "b.rpt", "b.out")
    # s1 and s2 share no state — safe to interleave or run in threads
    # (one thread per Solver; a single Solver is not thread-safe internally).

The cost is that **every** API call must be associated with a Solver —
either implicitly (``with Solver(...) as s: ...``) or by passing it to a
domain class (``Nodes(s)``).

----

EngineState — what's legal when
================================

The Solver moves through a strict sequence of states.  The current
state is exposed via :attr:`Solver.state`:

.. list-table::
   :header-rows: 1
   :widths: 20 12 68

   * - State
     - Value
     - Meaning
   * - ``CREATED``
     - 1
     - Engine handle allocated, no input file parsed yet.
   * - ``OPENED``
     - 2
     - ``.inp`` parsed; objects are accessible for inspection or editing.
   * - ``INITIALIZED``
     - 3
     - Initial conditions applied; arrays allocated.
   * - ``STARTED``
     - 4
     - Routing started; no time has elapsed yet.
   * - ``RUNNING``
     - 5
     - At least one step has been taken.
   * - ``ENDED``
     - 6
     - ``end()`` called; cumulative results are available but no more
       steps can be taken.
   * - ``CLOSED``
     - 7
     - ``close()`` called; ``.rpt`` / ``.out`` files flushed.

Methods can require the Solver to be in:

* a **specific** state (e.g. ``Solver.step()`` requires ``STARTED`` or
  ``RUNNING``),
* a **range** (e.g. node setter methods are valid in ``OPENED`` …
  ``RUNNING`` — anywhere except ``CREATED`` / ``CLOSED``), or
* **any** state (e.g. ``Solver.state`` itself).

When a method is called in the wrong state, the C engine returns a
non-zero error code which the Cython binding raises as
:class:`~openswmm.engine.EngineError`.

The "happy path" via the context manager
-----------------------------------------

Calling :class:`Solver` as a context manager hides the state machine:

.. code-block:: python

    with Solver("model.inp", "model.rpt", "model.out") as s:
        # state == STARTED on entry
        while s.step():       # state moves STARTED → RUNNING after first step
            ...
    # state == CLOSED on exit; engine handle has been freed

The context manager runs::

    on entry:  create() → open() → initialize() → start()
    on exit:   end() → report() → close() → destroy()

so the only thing you usually write is the inner loop.

The manual lifecycle
--------------------

For finer control (e.g. inspecting parsed objects between ``open()`` and
``initialize()``, or saving results selectively):

.. code-block:: python

    s = Solver("model.inp", "model.rpt", "model.out")
    s.create()
    s.open()           # state == OPENED — inspect / edit the model here
    s.initialize()
    s.start(save_results=True)
    while s.step():
        ...
    s.end()
    s.report()
    s.close()
    s.destroy()        # state == CLOSED, handle freed

You **must** call :meth:`Solver.destroy` (or use the context manager)
to release the C engine handle.  Forgetting to do so leaks memory and
keeps file handles open.

----

The exception model
===================

Every Cython binding checks the C return code.  Anything non-zero
becomes an :class:`~openswmm.engine.EngineError`:

.. code-block:: python

    from openswmm.engine import Solver, Nodes, EngineError

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes = Nodes(s)
        try:
            nodes.get_depth("does-not-exist")
        except EngineError as e:
            print(f"engine returned {e.code}: {e.message}")

The full table of error codes is in the C header
``openswmm_engine.h`` (``SWMM_ERR_*`` constants); the Python enum
:class:`~openswmm.engine.ErrorCode` lifts the most common ones.  The
``message`` attribute is filled by the C API — never construct one
manually.

Common Python-side exceptions worth knowing:

* :exc:`KeyError`         — passed a string id that isn't in the model.
* :exc:`IndexError`       — passed an integer index that's out of range.
* :exc:`TypeError`        — passed the wrong argument type to a setter.
* :exc:`ValueError`       — passed a value the engine rejects (e.g.
  negative max-depth on a junction).

These are raised before the C call dispatches, so they do **not** carry
an engine error code.

----

Working with names vs. integer indices
=======================================

Every object (node, link, subcatchment, gage, pollutant, table, …) has
both a **string id** (from the ``.inp``) and an **integer index** (the
position in the engine's internal array).  Almost every accessor takes
either form:

.. code-block:: python

    nodes.get_depth("J1")    # by name — looks up the index each call
    nodes.get_depth(0)       # by index — direct array access

In hot loops, prefer the integer form:

.. code-block:: python

    j1 = nodes.get_index("J1")
    while s.step():
        d = nodes.get_depth(j1)   # no name lookup overhead

Indices are stable for the lifetime of the Solver (they correspond to
positions in the C arrays).  They are **not** stable across runs of
different models — always re-resolve via :meth:`get_index` after
opening a new Solver.

----

Bulk arrays
===========

Where the engine exposes a homogeneous array of values across all
nodes / links / subcatchments, the Python layer offers a vectorised
companion accessor with the suffix ``_bulk``:

.. code-block:: python

    nodes.get_depths_bulk()      # → np.ndarray[float64], shape (n_nodes,)
    nodes.get_heads_bulk()
    nodes.set_depths_bulk(arr)   # arr must be float64, shape (n_nodes,)

    links.get_flows_bulk()       # → np.ndarray[float64], shape (n_links,)

The returned array **shares memory with an internal scratch buffer**
that the engine reuses on the next call.  Read-once-and-discard usage
is safe; if you keep the array around (e.g. across a step), call
``.copy()`` first.

----

Threading & multiprocessing
===========================

* **Multiple processes**: fully supported.  Each child process gets its
  own engine handle.  Use :mod:`multiprocessing` /
  :mod:`concurrent.futures.ProcessPoolExecutor` for ensemble runs.
* **Multiple threads, one Solver per thread**: supported.  The C
  engine is reentrant; two threads each holding their own Solver do
  not interact.
* **Multiple threads, one shared Solver**: **not** supported.  The
  Solver and its domain classes assume a single-threaded caller.  If
  you need shared state, drive a single Solver from one thread and
  feed work to it via a queue.

----

Backward compatibility with SWMM 5
==================================

The legacy SWMM 5.x solver remains available via
:mod:`openswmm.legacy.engine`.  Existing code that does

.. code-block:: python

    from openswmm.legacy.engine import Solver
    Solver.run("model.inp", "model.rpt", "model.out")

continues to work unchanged.  See :doc:`../migration/swmm5_to_swmm6`
for translating SWMM 5 patterns to the new v6.0 engine.
