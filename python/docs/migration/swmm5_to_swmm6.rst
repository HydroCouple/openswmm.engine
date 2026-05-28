===========================
Migrating from SWMM 5 to v6
===========================

If you have existing Python code calling the legacy SWMM 5 solver
(:mod:`openswmm.legacy.engine` or any other SWMM 5 binding), this page
shows the v6.0 equivalent of every common pattern.

The legacy path **continues to work** — the SWMM 5 solver is preserved
verbatim under :mod:`openswmm.legacy.engine` and your existing code
imports unchanged.  Migrate at your pace, module by module.

----

Why migrate?
============

The v6.0 engine is the future of OpenSWMM.  Compared to SWMM 5, it gives
you:

* **Reentrancy.**  Multiple independent simulations in the same
  process — useful for ensembles, parameter sweeps, optimisers.
* **Domain-split API.**  Instead of one ``getValue(SUBCATCH, idx,
  attr)`` god-method, you call ``Subcatchments(s).get_runoff(idx)`` —
  IDE auto-complete, type checking, and discoverability all work.
* **Bulk numpy accessors.**  ``Nodes(s).get_depths_bulk()`` returns a
  contiguous ``np.ndarray`` in one C call instead of a Python loop.
* **Programmatic model construction.**  Build a model in Python via
  :class:`~openswmm.engine.ModelBuilder`, no ``.inp`` file required.
* **In-place editing.**  Add, delete, or convert objects via
  :class:`~openswmm.engine.ModelEditor`.
* **Plugin SDK.**  Bring your own input format (GeoPackage, HDF5, …)
  and report writer.
* **New physics.**  Semi-implicit node continuity, Anderson
  acceleration on Picard, dynamic Preissmann slot (in-progress).

----

Side-by-side translation
========================

Run a model start to finish
---------------------------

.. tab-set::

    .. tab-item:: SWMM 5 (legacy)

        .. code-block:: python

            from openswmm.legacy.engine import Solver

            with Solver("model.inp", "model.rpt", "model.out") as s:
                while True:
                    s.step()
                    if s.elapsed >= s.duration:
                        break

    .. tab-item:: v6.0 (new)

        .. code-block:: python

            from openswmm.engine import Solver, EngineState

            with Solver("model.inp", "model.rpt", "model.out") as s:
                while s.step():        # returns False at end-of-sim
                    pass

* In v6, :meth:`Solver.step` **returns** a bool: ``True`` while there
  is more time to simulate, ``False`` when the simulation reaches its
  end time.  No need to track ``elapsed`` against ``duration`` yourself.

Read a node depth at every step
-------------------------------

.. tab-set::

    .. tab-item:: SWMM 5 (legacy)

        .. code-block:: python

            from openswmm.legacy.engine import Solver, SWMMObjects, SWMMNodeProperties

            with Solver("model.inp", "model.rpt", "model.out") as s:
                while s.state == EngineState.RUNNING:
                    if s.step() != 0:
                        break
                    d = s.getValue(SWMMObjects.NODE,
                                   s.getObjectIndex(SWMMObjects.NODE, "J1"),
                                   SWMMNodeProperties.DEPTH)

    .. tab-item:: v6.0 (new)

        .. code-block:: python

            from openswmm.engine import Solver, Nodes

            with Solver("model.inp", "model.rpt", "model.out") as s:
                nodes = Nodes(s)
                j1 = nodes.get_index("J1")     # resolve once
                while s.state == EngineState.RUNNING:
                    if s.step() != 0:
                        break
                    d = nodes.get_depth(j1)

* Domain class :class:`Nodes`, not enum-driven ``getValue``.
* String → integer index resolution happens once, outside the loop.

Inject a lateral inflow
-----------------------

.. tab-set::

    .. tab-item:: SWMM 5 (legacy)

        .. code-block:: python

            j1 = s.getObjectIndex(SWMMObjects.NODE, "J1")
            while s.state == EngineState.RUNNING:
                if s.step() != 0:
                    break
                s.setValue(SWMMObjects.NODE, j1,
                           SWMMNodeProperties.LATERAL_INFLOW, 1.5)

    .. tab-item:: v6.0 (new — one-shot)

        .. code-block:: python

            nodes = Nodes(s)
            j1 = nodes.get_index("J1")
            while s.state == EngineState.RUNNING:
                if s.step() != 0:
                    break
                nodes.set_lateral_inflow(j1, 1.5)

    .. tab-item:: v6.0 (sticky / forcing)

        .. code-block:: python

            from openswmm.engine import Forcing, ForcingMode

            forcing = Forcing(s)
            j1 = nodes.get_index("J1")
            forcing.node_lat_inflow(j1, 1.5, ForcingMode.REPLACE, persist=True)
            while s.state == EngineState.RUNNING:
                if s.step() != 0:
                    break
            forcing.clear_all()

* The SWMM 5 ``setValue`` is **one-shot** (overwritten by the engine
  on the next step).  v6.0 :meth:`Nodes.set_lateral_inflow` is the
  same one-shot semantic — same code shape.
* For overrides that survive across steps without re-applying every
  loop iteration, use the new :class:`Forcing` API (no SWMM 5
  equivalent).

Read every node's depth at once
-------------------------------

.. tab-set::

    .. tab-item:: SWMM 5 (legacy)

        .. code-block:: python

            n = s.getCount(SWMMObjects.NODE)
            depths = [
                s.getValue(SWMMObjects.NODE, i, SWMMNodeProperties.DEPTH)
                for i in range(n)
            ]   # Python loop crosses C boundary n times

    .. tab-item:: v6.0 (new)

        .. code-block:: python

            depths = nodes.get_depths_bulk()       # one C call, returns np.ndarray

* The ``*_bulk`` family is **dramatically** faster for read-many
  patterns (model, post-process, plot).

Run multiple scenarios in parallel
----------------------------------

.. tab-set::

    .. tab-item:: SWMM 5 (legacy)

        .. code-block:: python

            # Not safe — SWMM 5 is not reentrant.
            # Multiple Solver instances in one process share state.

    .. tab-item:: v6.0 (new)

        .. code-block:: python

            from concurrent.futures import ThreadPoolExecutor
            from openswmm.engine import Solver

            def run(inp):
                with Solver(inp, inp.replace(".inp", ".rpt"),
                            inp.replace(".inp", ".out")) as s:
                    while s.state == EngineState.RUNNING:
                        if s.step() != 0:
                            break

            inputs = ["a.inp", "b.inp", "c.inp"]
            with ThreadPoolExecutor(max_workers=4) as pool:
                list(pool.map(run, inputs))

* v6 is reentrant: one thread per Solver, **multiple Solvers per
  process** is fully supported.
* SWMM 5's global state means you must drop to multiprocessing.

Build a model from scratch
--------------------------

.. tab-set::

    .. tab-item:: SWMM 5 (legacy)

        Not possible without writing an ``.inp`` text file by hand and
        feeding it to ``Solver(inp_path, …)``.

    .. tab-item:: v6.0 (new)

        .. code-block:: python

            from openswmm.engine import (
                ModelBuilder, NodeType, LinkType, XSectShape
            )

            m = ModelBuilder()
            m.add_node("J1", NodeType.JUNCTION)
            m.add_node("OUT1", NodeType.OUTFALL)
            m.add_link("C1", LinkType.CONDUIT)
            m.set_link_nodes(0, 0, 1)
            m.set_link_length(0, 300.0)
            m.set_link_roughness(0, 0.013)
            m.set_link_xsect(0, XSectShape.CIRCULAR, 1.0)
            m.validate()
            m.finalize()

            solver = m.to_solver()
            solver.start()
            while solver.state == EngineState.RUNNING:
                if solver.step() != 0:
                    break
            solver.end()
            solver.destroy()

Read a binary ``.out`` file
---------------------------

.. tab-set::

    .. tab-item:: SWMM 5 (legacy)

        .. code-block:: python

            from openswmm.legacy.output import Output, ElementType, NodeAttribute

            out = Output("model.out")
            depth = out.getNodeSeries("J1", NodeAttribute.DEPTH)

    .. tab-item:: v6.0 (new)

        .. code-block:: python

            from openswmm.engine import OutputReader, OutNodeVar

            reader = OutputReader("model.out")
            depth  = reader.node_series("J1", OutNodeVar.DEPTH)

* Both APIs read the same on-disk format (the v6 engine writes a
  binary ``.out`` that's compatible with the legacy reader).
* The new :class:`OutputReader` adds bulk ``*_array`` methods for
  vectorised reads of every node / link.

----

Concept-mapping cheat sheet
============================

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - SWMM 5 / legacy
     - OpenSWMM 6 equivalent
   * - ``Solver.run(inp, rpt, out)``
     - ``with Solver(inp, rpt, out) as s: while s.step(): pass``
   * - ``s.getCount(SWMMObjects.NODE)``
     - ``Nodes(s).count()``
   * - ``s.getObjectIndex(SWMMObjects.NODE, "J1")``
     - ``Nodes(s).get_index("J1")``
   * - ``s.getValue(SWMMObjects.NODE, i, SWMMNodeProperties.DEPTH)``
     - ``Nodes(s).get_depth(i)``
   * - ``s.setValue(SWMMObjects.NODE, i, SWMMNodeProperties.LATERAL_INFLOW, q)``
     - ``Nodes(s).set_lateral_inflow(i, q)``
   * - ``s.getValue(SWMMObjects.LINK, i, SWMMLinkProperties.FLOW)``
     - ``Links(s).get_flow(i)``
   * - ``s.getValue(SWMMObjects.SUBCATCH, i, SWMMSubcatchmentProperties.RAINFALL)``
     - ``Subcatchments(s).get_rainfall(i)``
   * - ``s.setValue(SWMMObjects.RAINGAGE, i, SWMMRainGageProperties.RAINFALL, r)``
     - ``Gages(s).set_rainfall(i, r)``
   * - n/a
     - :class:`~openswmm.engine.Forcing` (cross-step persistent overrides)
   * - n/a
     - :class:`~openswmm.engine.ModelBuilder`
       (programmatic construction)
   * - n/a
     - :class:`~openswmm.engine.ModelEditor`
       (in-place add / delete / convert)
   * - n/a
     - :class:`~openswmm.engine.Statistics`
       (accumulated peak flow, surcharge hours, etc.)
   * - ``Output.getNodeSeries("J1", NodeAttribute.DEPTH)``
     - ``OutputReader(...).node_series("J1", OutNodeVar.DEPTH)``

----

Compatibility notes
===================

* The legacy ``Solver`` and ``Output`` classes remain exposed
  unchanged at :mod:`openswmm.legacy.engine` and
  :mod:`openswmm.legacy.output`.  Importing
  :mod:`openswmm` re-exports them at the top level for code that
  pre-dates the namespace split.
* The two engines **share** the binary ``.out`` format, so a v6 run can
  be post-processed with the legacy ``Output`` reader and vice-versa.
* The two engines do **not** share the ``.inp`` extension keys.  v6
  introduces several new sections (e.g. ``[OPTIONS] CRS``,
  ``[USER_FLAGS]``, semi-implicit / Anderson knobs) that the legacy
  parser will warn about and ignore — your file will still run on
  legacy with degraded behaviour.

----

Where to next?
==============

* Walk through :doc:`../guide/quickstart` with v6 idioms.
* Pick the domain pages that matter most to your existing code:
  :doc:`../guide/nodes`, :doc:`../guide/links`,
  :doc:`../guide/subcatchments`, :doc:`../guide/output_reader`.
* If you're dynamically modifying ``.inp`` files via string
  manipulation today, replace that with
  :doc:`../guide/model_builder` and :doc:`../guide/editing`.

----

OpenSWMM 6 — Pythonic bindings v0 → v1
======================================

OpenSWMM 6 pre-release shipped a thin, mechanical Cython surface
("v0"). v1 hard-replaces it with a property-style API: collections,
wrapper objects, typed enums, ``int | str`` selectors, and
:class:`~datetime.datetime` / :class:`~datetime.timedelta` everywhere
instead of raw doubles.

This section is a side-by-side cheat sheet for porting v0 scripts to
v1. The package import paths
(``from openswmm.engine import Solver, Nodes, ...``) are unchanged.

.. currentmodule:: openswmm.engine

Solver lifecycle
----------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - v0
     - v1
   * - ``rc = s.open(); if rc: ...``
     - ``s.open()  # raises EngineError on failure``
   * - ``while s.state == EngineState.RUNNING: if s.step() != 0: break``
     - ``for elapsed in s.steps(): ...``
   * - ``s.get_start_time() -> float (decimal days)``
     - ``s.start_datetime -> datetime``
   * - ``s.get_current_time(), get_end_time(), get_routing_step()``
     - ``s.current_datetime, s.end_datetime, s.routing_step (timedelta)``
   * - ``s.state -> int``
     - ``s.state -> EngineState``
   * - ``s.elapsed -> float (decimal days)``
     - ``s.elapsed -> timedelta``
   * - ``s.get_option(key)``, ``s.set_option(key, v)``
     - ``s.options[key]``, ``s.options[key] = v``
   * - ``s.userflag_get_bool(name) / userflag_set_bool(name, v)`` (and ``_int``/``_real``)
     - ``s.userflags[name]`` (auto-typed)
   * - ``s.events_count(), events_get(i), events_add(start, end)``
     - ``len(s.events), s.events[i], s.events.append(Event(start, end))``

Nodes
-----

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - v0
     - v1
   * - ``Nodes(s).get_depth("J1")``
     - ``s.nodes["J1"].depth``
   * - ``Nodes(s).set_lateral_inflow("J1", 0.5)``
     - ``s.nodes["J1"].lateral_inflow = 0.5``
   * - ``Nodes(s).get_invert_elev(idx)``
     - ``s.nodes[idx].invert_elev``
   * - ``Nodes(s).get_depths_bulk() / set_depths_bulk(arr)``
     - ``s.nodes.depths`` (read/write property)
   * - ``Nodes(s).get_stat_max_depth(idx)``
     - ``s.nodes[idx].stats.max_depth``
   * - ``Nodes(s).get_outfall_type(idx)``
     - ``s.nodes[idx].outfall.type`` (raises if not OUTFALL)
   * - ``Nodes(s).set_storage_functional(idx, a, b, c)``
     - ``s.nodes[idx].storage.functional = (a, b, c)``

Links
-----

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - v0
     - v1
   * - ``Links(s).get_flow("C1")``
     - ``s.links["C1"].flow``
   * - ``Links(s).get_from_node(idx)``
     - ``s.links[idx].from_node`` → :class:`Node` wrapper
   * - ``Links(s).set_xsect(idx, shape, g1, g2, g3, g4)``
     - ``s.links[idx].xsect = (shape, g1, g2, g3, g4)``
   * - ``Links(s).set_pump_curve(idx, c)``
     - ``s.links[idx].pump.curve = c`` (raises if not PUMP)
   * - ``Links(s).get_stat_max_flow(idx)``
     - ``s.links[idx].stats.max_flow``
   * - ``Links(s).get_flows_bulk()``
     - ``s.links.flows``

Subcatchments and gages
-----------------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - v0
     - v1
   * - ``Subcatchments(s).get_area("S1")``
     - ``s.subcatchments["S1"].area``
   * - ``Subcatchments(s).set_infil_horton(idx, f0, fmin, decay, dry)``
     - ``s.subcatchments[idx].infiltration.set_horton(f0, fmin, decay, dry)``
   * - ``Subcatchments(s).set_coverage(idx, lu_idx, frac)``
     - ``s.subcatchments[idx].coverage["RESIDENTIAL"] = frac``
   * - ``Gages(s).get_rainfall(idx)``
     - ``s.gages[idx].rainfall``
   * - ``Gages(s).set_rain_type(idx, t)``
     - ``s.gages[idx].rain_type = GageRainType.INTENSITY``

OutputReader
------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - v0
     - v1
   * - ``out.get_start_date() -> float``
     - ``out.start_datetime -> datetime``
   * - ``out.get_report_step() -> int``
     - ``out.report_step -> timedelta``
   * - ``out.get_period_count() -> int``
     - ``out.period_count`` (property)
   * - ``out.get_node_series(idx, var, start, end)``
     - ``out.node_series("J1", OutNodeVar.DEPTH, start=..., end=...)``
   * - ``out.get_node_attribute(idx, period) -> np.ndarray``
     - ``out.node_attributes("J1", period) -> Dict[OutNodeVar, float]``
   * - ``out.get_node_stat_max_depth(idx)``
     - ``out.node_stats("J1").max_depth``

Pollutants, controls, forcing, hot-start
----------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - v0
     - v1
   * - ``Pollutants(s).get_kdecay(idx)``
     - ``s.pollutants["TSS"].kdecay``
   * - ``Controls(s).add_rule(text)``
     - ``s.controls.append(text)``
   * - ``Controls(s).count() / get_rule(i)``
     - ``len(s.controls) / s.controls[i].text``
   * - ``Forcing(s).node_lat_inflow(idx, v, mode=0, persist=1)``
     - ``s.forcing.node_lat_inflow("J1", v, mode=ForcingMode.REPLACE, persist=True)``
   * - ``HotStart.save(s, path)`` / ``HotStart.open(path)``
     - ``HotStart.save_from(s, path)`` / ``HotStart.open(path)`` (unchanged)
   * - ``hs.get_sim_time() -> float`` / ``hs.warning_count() / get_warning(i)``
     - ``hs.sim_datetime -> datetime`` / ``hs.warnings -> list[str]``
   * - ``HotStart.saves_add(s, path, when)``
     - ``s.save_schedule.append(SaveScheduleEntry(when=dt, path=p))``

Exceptions
----------

v1's :class:`EngineError` is now a hierarchy where each subclass also
inherits from the closest stdlib exception:

.. code-block:: python

    # v0 — only EngineError available; you had to check e.code yourself.
    try:
        s.nodes.get_depth("NO_SUCH_NODE")
    except EngineError as e:
        if e.code == ErrorCode.BADINDEX:
            ...

    # v1 — write the idiomatic handler:
    try:
        depth = s.nodes["NO_SUCH_NODE"].depth
    except KeyError:                     # also an EngineError subclass
        ...

See :doc:`../guide/error_handling` for the full subclass table.
