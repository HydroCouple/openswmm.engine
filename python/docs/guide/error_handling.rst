======================================
Error handling, edge cases & debugging
======================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

This page is a cross-cutting reference for the exception model, the
:class:`EngineState` rules that govern when each method is callable,
and the patterns we recommend for robust scripts.

For the underlying lifecycle see :doc:`concepts`.

----

The exception model in one paragraph
====================================

Every Cython binding checks the C return code.  Anything non-zero
raises an :class:`EngineError`, and the message is filled in by the C
API — you don't construct one yourself.  Pure-Python checks
(wrong-type argument, out-of-range index, missing id) raise
:exc:`TypeError` / :exc:`IndexError` / :exc:`KeyError` *before* the C
call dispatches, so they don't carry an engine error code.

.. code-block:: python

    from openswmm.engine import Solver, Nodes, EngineError

    with Solver("model.inp", "model.rpt", "model.out") as s:
        nodes = Nodes(s)
        try:
            nodes.get_depth("does-not-exist")
        except EngineError as e:
            print(f"engine returned {e.code}: {e.message}")
        except KeyError as e:
            print(f"missing id: {e}")

----

EngineState reference (cheat-sheet)
====================================

The Solver moves through these states in strict order:

.. list-table::
   :header-rows: 1
   :widths: 18 12 70

   * - State
     - Value
     - Meaning
   * - ``CREATED``
     - 1
     - Engine handle allocated; no input parsed.
   * - ``OPENED``
     - 2
     - ``.inp`` parsed; objects accessible for inspection / editing.
   * - ``INITIALIZED``
     - 3
     - Initial conditions applied; arrays allocated.
   * - ``STARTED``
     - 4
     - Routing started; no time has elapsed.
   * - ``RUNNING``
     - 5
     - At least one step has been taken.
   * - ``ENDED``
     - 6
     - ``end()`` called; cumulative results available.
   * - ``CLOSED``
     - 7
     - ``close()`` called; ``.rpt`` / ``.out`` flushed.

What you can do in each state
-----------------------------

.. list-table::
   :header-rows: 1
   :widths: 25 25 50

   * - You want to …
     - Required state(s)
     - See
   * - Add objects (``ModelBuilder``)
     - pre-Solver
     - :doc:`model_builder`
   * - Edit / delete / convert objects
     - ``OPENED``
     - :doc:`editing`
   * - Read object identity / topology
     - ``OPENED`` …
     - :doc:`nodes`, :doc:`links`, :doc:`subcatchments`
   * - Read geometry (invert, length, …)
     - ``OPENED`` …
     - :doc:`nodes`, :doc:`links`
   * - Set geometry / parameters (invert, n, …)
     - ``OPENED``
     - same
   * - Apply initial conditions
     - ``INITIALIZED``
     - :doc:`solver`
   * - Read hydraulic state (depth, flow, …)
     - ``RUNNING`` or ``ENDED``
     - :doc:`nodes`, :doc:`links`
   * - One-shot per-step setters (``set_depth``, ``set_flow``, …)
     - ``RUNNING``
     - :doc:`nodes`, :doc:`links`
   * - Persistent runtime forcing
     - ``RUNNING``
     - :doc:`forcing`
   * - Add / clear control rules
     - ``OPENED`` or ``RUNNING``
     - :doc:`controls`
   * - Read continuity / mass-balance
     - ``ENDED`` (final), ``RUNNING`` (partial)
     - :doc:`massbalance`
   * - Read accumulated statistics
     - ``RUNNING`` or ``ENDED``
     - :doc:`statistics`
   * - Save a hot-start
     - ``RUNNING`` or ``ENDED``
     - :doc:`hotstart`
   * - Apply a hot-start
     - ``OPENED`` or ``INITIALIZED``
     - :doc:`hotstart`
   * - Persist edits to ``.inp``
     - ``OPENED`` …
     - :doc:`solver`
       (:meth:`Solver.model_write`)

A method called outside its state envelope raises
:class:`EngineError`.  The most common codes:

.. list-table::
   :header-rows: 1
   :widths: 12 22 66

   * - Code
     - Name
     - Meaning
   * - ``20``
     - ``ERR_API_NOT_OPEN``
     - Method needs ``OPENED``; solver still ``CREATED``.
   * - ``21``
     - ``ERR_API_NOT_STARTED``
     - Called ``step()`` before ``start()``.
   * - ``22``
     - ``ERR_API_NOT_ENDED``
     - Called ``report()`` before ``end()``.
   * - ``23``
     - ``ERR_API_INVALID_TYPE``
     - Wrong object kind (e.g. setting a pump curve on a conduit).

The full enum is :class:`ErrorCode`.

----

Defensive patterns
==================

Always use the context manager for the Solver
---------------------------------------------

.. code-block:: python

    with Solver("model.inp", "model.rpt", "model.out") as s:
        ...                # raises here are still cleaned up

The context manager runs ``end → report → close → destroy`` even if
your loop body raises.  Skipping it leaks the engine handle on error.

Resolve names once, outside the loop
------------------------------------

.. code-block:: python

    j1 = nodes.get_index("J1")     # raises KeyError if not in model
    while s.step():
        d = nodes.get_depth(j1)    # no per-step name-lookup overhead

This catches typos at startup rather than after a long run.

Validate model state before running
-----------------------------------

.. code-block:: python

    s.open()
    if nodes.count() == 0:
        raise RuntimeError("model has no nodes")
    if links.count() == 0:
        raise RuntimeError("model has no links")

    # Programmatic edits: catch issues now, not at step()
    s.initialize()

Verify continuity after the run
-------------------------------

.. code-block:: python

    from openswmm.engine import MassBalance

    with Solver("model.inp", "model.rpt", "model.out") as s:
        while s.step():
            pass
        mb = MassBalance(s)
        if abs(mb.get_routing_continuity_error()) > 2.0:
            raise RuntimeError(
                f"routing continuity {mb.get_routing_continuity_error():+.4f}% > 2%"
            )

Wrap third-party callbacks
--------------------------

If you register progress / step callbacks that call back into your
own Python code, isolate exceptions so a callback bug doesn't tear
down the run mid-way:

.. code-block:: python

    def _safe(fn):
        def wrapper(*a, **kw):
            try:
                return fn(*a, **kw)
            except Exception as e:
                import traceback; traceback.print_exc()
                # swallow — never propagate into the C engine
        return wrapper

    run_with_callback("model.inp", "model.rpt", "model.out",
                      callback=_safe(my_progress_handler))

----

Edge cases & gotchas
====================

* **Bulk-array memory aliasing.**  ``*_bulk()`` methods return arrays
  that share memory with an internal scratch buffer.  Read-once is
  fine; if you keep the array, ``.copy()`` it.
* **Index stability.**  Integer indices are stable for the lifetime
  of a single Solver but not across runs of different models.  Always
  re-resolve via :meth:`get_index` after :meth:`Solver.open`.
* **Float precision in `set_*` / `get_*` round-trips.**  The C engine
  stores most values in double precision; a few legacy code paths
  use single precision internally.  ``set_x(v); get_x() == v`` is
  not guaranteed to be exact for those paths.
* **String ids are bytes-encoded UTF-8 on the C side.**  Non-ASCII
  ids work, but the C engine truncates at the first NUL byte and
  caps at ~80 chars.
* **Threading.**  One Solver per thread is supported; a single
  Solver is not thread-safe.  See :doc:`concepts` for the full
  contract.

----

Debugging tips
==============

Build with ``DEBUG=1``
----------------------

.. code-block:: bash

    DEBUG=1 pip install -e . --no-build-isolation

Produces unoptimised binaries with full debug symbols, suitable for
``lldb`` / ``gdb`` / IDE step-through into the C engine.  See
:doc:`install` for the rest of the env-var matrix.

Inspect the parsed model before stepping
----------------------------------------

.. code-block:: python

    s = Solver("model.inp", "", "")
    s.create()
    s.open()
    print(f"nodes: {Nodes(s).count()}, links: {Links(s).count()}")
    # … check every domain class you care about …
    s.destroy()         # without initializing / starting

This is the fastest way to confirm a parse without paying for the
full simulation.

Print the report file
---------------------

.. code-block:: python

    with Solver("model.inp", "model.rpt", "model.out") as s:
        while s.step():
            pass
    print(open("model.rpt").read())

The ``.rpt`` file contains the engine's own warnings / errors and
continuity summary — read it before debugging the Python side.

----

See also
========

* :doc:`concepts` — the conceptual model behind the rules above.
* :doc:`solver` — the methods whose state requirements this page
  cross-references.
* :doc:`massbalance` — continuity diagnostics worth gating CI on.
