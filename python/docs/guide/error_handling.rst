======================================
Error handling, edge cases & debugging
======================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

This page is a cross-cutting reference for the :class:`EngineError`
hierarchy, the :class:`EngineState` rules that govern when each method
is callable, and the idioms we recommend for robust scripts.

For the underlying lifecycle see :doc:`concepts`.

----

The exception model in one paragraph
====================================

Every binding checks the C return code. A non-zero code raises an
:class:`EngineError` — but *which* :class:`EngineError` depends on the
underlying ``SWMM_ERR_*``. Each subclass **also** inherits from a
standard library exception so existing ``except IndexError:`` /
``except ValueError:`` handlers do the right thing without any
engine-specific imports.

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        try:
            depth = s.nodes["DOES_NOT_EXIST"].depth
        except KeyError as e:                    # also an EngineError subclass
            print(f"missing node: {e}")
        try:
            depth = s.nodes[10_000].depth
        except IndexError as e:                  # likewise
            print(f"out of range: {e}")

----

The :class:`EngineError` hierarchy
==================================

The :func:`~openswmm.engine._exceptions.raise_for_code` dispatcher maps
every documented ``SWMM_ERR_*`` to the corresponding subclass:

.. list-table::
   :header-rows: 1
   :widths: 24 32 44

   * - Subclass
     - Also inherits from
     - Raised when …
   * - :class:`BadHandleError`
     - :exc:`RuntimeError`
     - Engine handle is ``NULL`` or invalid.
   * - :class:`BadIndexError`
     - :exc:`IndexError`
     - Integer index is out of range.
   * - :class:`BadParamError`
     - :exc:`ValueError`
     - Argument fails range / shape / domain check.
   * - :class:`LifecycleError`
     - :exc:`RuntimeError`
     - Method called in the wrong :class:`EngineState`.
   * - :class:`HotStartError`
     - :exc:`RuntimeError`
     - Hot start file is missing, corrupt, or schema-mismatched.
   * - :class:`PluginError`
     - :exc:`RuntimeError`
     - A loaded plugin returned a failure or refused to initialise.
   * - :class:`FileError`
     - :exc:`IOError`
     - Any of input / report / output file I/O failures.
   * - :class:`ParseError`
     - :exc:`ValueError`
     - Input file has a syntactically invalid token or section.
   * - :class:`NumericalError`
     - :exc:`RuntimeError`
     - Solver divergence, NaN propagation, instability.
   * - :class:`CRSError`
     - :exc:`ValueError`
     - Coordinate reference system string is missing or invalid.
   * - :class:`DependencyError`
     - :exc:`RuntimeError`
     - Object can't be deleted / converted because other objects still
       reference it.
   * - :class:`EngineError`
     - :exc:`Exception`
     - Base class. Catches everything above; also raised directly for
       :attr:`ErrorCode.NOMEM` and :attr:`ErrorCode.INTERNAL`.

Every instance carries three attributes:

* ``.code`` — the raw integer (``SWMM_ERR_*`` value).
* ``.code_enum`` — the same value as an :class:`ErrorCode` enum member,
  or :attr:`ErrorCode.INTERNAL` if the code is unrecognised.
* ``.message`` — human-readable description filled from
  ``swmm_error_message()`` when not given explicitly.

.. code-block:: python

    from openswmm.engine import EngineError, ErrorCode

    try:
        s.nodes[-1].depth = 0.0
    except EngineError as e:
        print(e.code, e.code_enum, e.message)
        # 8 ErrorCode.BADINDEX 'index out of range'

----

EngineState reference
=====================

The Solver moves through these states in strict order:

.. list-table::
   :header-rows: 1
   :widths: 18 10 72

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
     - ``start()`` returned; ready for the first ``step()``.
   * - ``RUNNING``
     - 5
     - Inside the routing loop, ``step()`` / ``stride()`` callable.
   * - ``ENDED``
     - 6
     - ``end()`` called; cumulative statistics & mass balance available.
   * - ``CLOSED``
     - 7
     - ``close()`` called; files flushed.
   * - ``BUILDING``
     - 8
     - Programmatic construction in progress (no ``.inp`` involved).

Calling a method in the wrong state raises :class:`LifecycleError`
(which is also a :exc:`RuntimeError`):

.. code-block:: python

    from openswmm.engine import Solver, LifecycleError

    s = Solver("model.inp")
    try:
        s.step()                                 # not started yet
    except LifecycleError as e:
        print(f"can't step in state {s.state.name}: {e}")

----

Recommended patterns
====================

Catch the broadest sensible base
--------------------------------

If you don't care which specific failure occurred, catch
:class:`EngineError`:

.. code-block:: python

    try:
        s.run()
    except EngineError as e:
        log.error("simulation failed: %s (code=%s)", e.message, e.code_enum)

Catch the stdlib base when interop matters
------------------------------------------

When the engine call is part of a larger pipeline that uses standard
exceptions, the stdlib base classes give you uniform handlers:

.. code-block:: python

    def lookup(coll, key):
        try:
            return coll[key]
        except (IndexError, KeyError):
            return None

Use ``code_enum`` for decision logic
------------------------------------

When you do need to distinguish failures, compare against
:class:`ErrorCode` — never against raw integers:

.. code-block:: python

    from openswmm.engine import ErrorCode

    try:
        s.hotstart.apply(other)
    except EngineError as e:
        if e.code_enum is ErrorCode.HOTSTART:
            log.warning("hotstart skipped: %s", e.message)
        else:
            raise

----

Common pitfalls
===============

Mutating during iteration
-------------------------

Adding or deleting objects through ``solver.nodes`` / ``solver.links``
invalidates any wrapper objects currently held by Python code via a
generation counter — accessing a stale wrapper raises a
:class:`LifecycleError` with a message naming the operation that
invalidated it. Re-look up by id after a structural change.

Reading state too early
-----------------------

``Node.depth`` / ``Link.flow`` / etc. only have meaningful values once
the engine is at :attr:`EngineState.RUNNING` (or later). Accessing them
before ``start()`` raises :class:`LifecycleError`.

Confusing ``KeyError`` and :class:`BadIndexError`
-------------------------------------------------

* ``coll["NO_SUCH_ID"]`` → :exc:`KeyError` (str didn't match any object).
* ``coll[10_000]`` → :class:`BadIndexError` (also catchable as
  :exc:`IndexError`).
* ``coll[None]`` → :exc:`TypeError`.

This split lets you write ``try / except KeyError:`` for the
"check-if-name-exists" idiom without accidentally swallowing
out-of-range integer accesses.

----

See also
========

* :doc:`concepts` — engine lifecycle and the :class:`EngineState` machine.
* :doc:`datetime` — the SWMM DateTime double and Python ``datetime`` interop.
* :doc:`../api` — generated reference for every symbol surfaced above.
