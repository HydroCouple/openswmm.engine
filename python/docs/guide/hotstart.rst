=========
Hot start
=========

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

Hot start files capture the hydraulic + quality state of a simulation
at a moment in time so a follow-up run can resume from there.

Reference: ``openswmm_hotstart.h``.

----

Save and apply
==============

.. code-block:: python

    from pathlib import Path
    from openswmm.engine import Solver, HotStart

    # ---- save ----
    with Solver("warmup.inp") as s:
        for _ in s.steps():
            pass
        HotStart.save_from(s, Path("warmup.hs"))

    # ---- apply ----
    with HotStart.open("warmup.hs") as hs:
        print(hs.sim_datetime, hs.warnings)
        with Solver("storm.inp") as s2:
            # Apply in INITIALIZED state, before start().
            s2.open()
            s2.initialize()
            hs.apply(s2)
            s2.start()
            for _ in s2.steps():
                pass
            s2.end()

----

:class:`HotStart` — lifecycle
=============================

.. list-table::
   :header-rows: 1
   :widths: 36 64

   * - Operation
     - What it does
   * - ``HotStart.open(path)``
     - Classmethod. Opens an existing hot start file.
   * - ``HotStart.open_legacy(solver, path)``
     - Classmethod. Opens a legacy EPA SWMM 5 file, read-only, against the
       model in an opened ``solver`` (see `Inspecting a file`_).
   * - ``HotStart.save_from(solver, path)``
     - Static method. Save current solver state to ``path``.
   * - ``hs.close()`` / ``with HotStart.open(p) as hs:``
     - Context manager closes automatically.
   * - ``hs.apply(solver)``
     - Apply the captured state to a Solver in ``INITIALIZED`` state.

----

:class:`HotStart` — metadata
============================

.. list-table::
   :header-rows: 1
   :widths: 26 28 46

   * - Property
     - Type
     - Meaning
   * - ``sim_datetime``
     - :class:`~datetime.datetime`
     - Moment the state was captured.
   * - ``crs``
     - ``str | None``
     - CRS string from the file (or ``None``).
   * - ``warnings``
     - ``list[str]``
     - Warnings emitted during open / apply.
   * - ``node_count`` / ``link_count`` / ``subcatchment_count``
     - ``int``
     - Counts inside the hot-start blob.
   * - ``contents``
     - ``tuple[str, ...]``
     - State blocks the file holds: ``"SPECIES"``, ``"GROUNDWATER"``,
       ``"GW_TRANSPORT"``, ``"SNOW"``, ``"LID"``, ``"SURFACE"``,
       ``"ENGINE_CHECKPOINT"``, ``"LEGACY"``.
   * - ``version``
     - ``int``
     - File format version (1-4 for a legacy file).
   * - ``saved_at``
     - :class:`~datetime.datetime` ``| None``
     - When a native file was saved (UTC); ``None`` for a legacy file.
   * - ``end_datetime``
     - :class:`~datetime.datetime` ``| None``
     - End of the saving run; ``None`` for a legacy file.
   * - ``species``
     - ``list[str]``
     - Species whose node and link concentrations the file holds.

----

Inspecting a file
=================

``node_records()``, ``link_records()`` and ``subcatchment_records()``
return every stored record, in file order and internal units (ft, ft3,
ft3/s), as named tuples. Node and link records carry one concentration
per entry of ``species``:

.. code-block:: python

    with HotStart.open("warmup.hs") as hs:
        for r in hs.node_records():
            print(r.id, r.depth, r.head, r.volume, r.concentrations)
        for r in hs.link_records():
            print(r.id, r.flow, r.depth)

A value the file does not hold is NaN. A legacy SWMM 5 file stores values
in object order without names, so it is read against the model it was
written for; its counts must match, and records take the model's names.
It holds no node head or volume and no link volume:

.. code-block:: python

    s = Solver("model.inp", "model.rpt", "model.out")
    s.open()
    with HotStart.open_legacy(s, "model.hsf") as hs:
        print(hs.version, hs.contents)   # e.g. 4 ("SPECIES", "LEGACY")
        depths = {r.id: r.depth for r in hs.node_records()}

A legacy handle is read-only: the setters and :meth:`HotStart.apply`
refuse it, and closing it writes nothing.

----

Direct state edits
==================

The C API stores hot-start state by id, not by index, so the editing
methods are id-keyed:

.. code-block:: python

    with HotStart.open("warmup.hs") as hs:
        hs.set_node_depth("J1", 1.2)
        hs.set_node_head("J1", 102.3)
        hs.set_link_flow("C1", 0.4)
        hs.set_link_depth("C1", 0.6)
        hs.set_subcatchment_runoff("S1", 0.0)

Edits made on a :class:`HotStart` are applied when :meth:`apply` runs,
and written back to the file when the handle is closed.

``set_node_state(node_id, depth)`` and ``set_link_state(link_id, flow,
depth)`` keep a restart consistent: they store the depth (and flow) and
leave the head and volume to be derived from the restarting model's
geometry, so a depth edit cannot leave a stale head or volume behind.
They refuse a negative or non-finite value (:class:`BadParamError`) and a
file holding a full engine checkpoint, whose restart would not read the
records (:class:`HotStartError`).

----

``solver.save_schedule`` — recurring saves
==========================================

The engine's ``[SAVE HOTSTART]`` block is exposed as a
:class:`MutableSequence` of :class:`SaveScheduleEntry` records on the
Solver:

.. code-block:: python

    from datetime import datetime
    from openswmm.engine._hotstart import SaveScheduleEntry

    solver.save_schedule.append(SaveScheduleEntry(
        when=datetime(2024, 6, 15, 12, 0),
        path="midday.hs",
    ))
    for entry in solver.save_schedule:
        print(entry.when, entry.path)

    del solver.save_schedule[0]
    solver.save_schedule.clear()

Each entry is a ``NamedTuple`` so ``entry.when`` (a
:class:`~datetime.datetime`) and ``entry.path`` (``str``) are stable
attributes. The C API supports only ``append``; ``insert`` is emulated
by clearing and re-adding (just like ``solver.events``).

Assignments accept either a :class:`SaveScheduleEntry`, a ``(when,
path)`` tuple, or a ``{"when": ..., "path": ...}`` dict.

----

See also
========

* :doc:`solver` — :attr:`Solver.save_schedule`.
* :doc:`concepts` — the ``INITIALIZED`` state is the only legal place
  to apply a hot start.
* :doc:`error_handling`.
