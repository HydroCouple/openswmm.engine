===========================================
Model editing  (deletion + type conversion)
===========================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.ModelEditor`.  This is unique to the v6
   engine — there is no SWMM 5 equivalent.

.. currentmodule:: openswmm.engine

The :class:`ModelEditor` mutates an **already-parsed** model in place:
delete objects, convert object types, and analyse the cascading
impact those operations would have on the rest of the network.

Use it for:

* Cleaning up auto-generated networks (drop orphan nodes, dangling
  conduits).
* Converting nodes between types (junction → storage, junction →
  outfall).
* Pre-flighting a destructive change ("show me what would break if I
  delete this conduit").

Reference: ``openswmm_edit.h``.

----

Class signature
===============

.. code-block:: python

    class ModelEditor:
        def __init__(self, engine: object) -> None: ...

* ``engine`` — pass the live :class:`Solver` (or its underlying
  engine handle).

The editor must be applied to a Solver in the ``OPENED`` state, so
that the model is parsed and addressable but routing has not yet
started.

----

Helper return types
===================

* :class:`ImpactEntry` — one row in the report of "what this change
  would affect": kind, id, description.  Its ``obj_type_name`` covers
  all 22 referenceable object categories (node, link, subcatchment,
  gage, table, transect, inlet_usage, ext_inflow, dwf_inflow,
  rdii_assign, treatment, lid_usage, snowpack, hydrograph, pollutant,
  pattern, aquifer, lid_control, street, inlet_design, landuse, and
  control_rule).
* :class:`ConversionResult` — outcome of a type conversion: success,
  warnings, fields that had to be defaulted.

Both are plain data classes with ``__repr__`` so they display well
during interactive exploration.

----

Key methods
===========

Counts  (read-only properties — no parentheses)
-----------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Property
     - Returns
   * - :attr:`node_count` / :attr:`link_count` / :attr:`subcatch_count` / :attr:`gage_count` / :attr:`table_count`
     - Number of objects of each kind currently in the model.  Access
       as attributes — e.g. ``editor.node_count``, *not*
       ``editor.node_count()``.

Time control  (typed datetime properties)
-----------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Property
     - Value
   * - :attr:`start_datetime`
     - Simulation start as :class:`datetime.datetime` (read/write).
   * - :attr:`end_datetime`
     - Simulation end as :class:`datetime.datetime` (read/write).
   * - :attr:`report_start_datetime`
     - Report start as :class:`datetime.datetime` (read/write).

Impact analysis  (read-only — no mutation)
------------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Returns
   * - :meth:`analyze_node_impact(id_or_idx)`
     - List of :class:`ImpactEntry` rows that would be affected by
       deletion.
   * - :meth:`analyze_link_impact(id_or_idx)`
     - Same, for a link.
   * - :meth:`analyze_subcatch_impact(id_or_idx)`
     - Same, for a subcatchment.
   * - :meth:`analyze_gage_impact(id_or_idx)`
     - Same, for a rain gage.
   * - :meth:`analyze_table_impact(id_or_idx)`
     - Same, for a table / curve.
   * - :meth:`analyze_transect_impact(idx)`
     - Same, for a transect.
   * - :meth:`analyze_pollutant_impact(id_or_idx)`
     - Same, for a pollutant (per-pollutant matrices are re-packed on
       deletion).
   * - :meth:`analyze_pattern_impact(id_or_idx)`
     - Same, for a time pattern.
   * - :meth:`analyze_aquifer_impact(id_or_idx)`
     - Same, for an aquifer (referencing subcatchments).
   * - :meth:`analyze_snowpack_impact(id_or_idx)`
     - Same, for a snowpack (referencing subcatchments).
   * - :meth:`analyze_lid_impact(id_or_idx)`
     - Same, for a LID control (referencing LID-usage rows).
   * - :meth:`analyze_street_impact(id_or_idx)`
     - Same, for a street (referencing inlet-usage rows).
   * - :meth:`analyze_inlet_impact(id_or_idx)`
     - Same, for an inlet design (referencing inlet-usage rows).
   * - :meth:`analyze_landuse_impact(id_or_idx)`
     - Same, for a land use.
   * - :meth:`analyze_hydrograph_impact(uh_name)`
     - Same, for a unit-hydrograph group (keyed by name).

Deletion
--------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Returns
   * - :meth:`delete_node(id_or_idx)`
     - List of :class:`ImpactEntry` for what was actually deleted /
       reconnected.
   * - :meth:`delete_link(id_or_idx)`
     - Same, for a link.
   * - :meth:`delete_subcatch(id_or_idx)`
     - Same, for a subcatchment.
   * - :meth:`delete_gage(id_or_idx)`
     - Same, for a rain gage.
   * - :meth:`delete_table(id_or_idx)`
     - Same, for a table / curve.
   * - :meth:`delete_transect(idx)`
     - Same, for a transect.
   * - :meth:`delete_pollutant(id_or_idx)`
     - Delete a pollutant and re-pack every per-pollutant matrix.
   * - :meth:`delete_pattern(id_or_idx)`
     - Delete a time pattern and clear all name-based references.
   * - :meth:`delete_aquifer(id_or_idx)`
     - Delete an aquifer; referencing subcatchments lose their
       groundwater.
   * - :meth:`delete_snowpack(id_or_idx)`
     - Delete a snowpack; clear referencing subcatchments and renumber.
   * - :meth:`delete_lid(id_or_idx)`
     - Delete a LID control; cascade-delete referencing LID-usage rows.
   * - :meth:`delete_street(id_or_idx)`
     - Delete a street; cascade-delete referencing inlet-usage rows.
   * - :meth:`delete_inlet(id_or_idx)`
     - Delete an inlet design; cascade-delete referencing inlet-usage
       rows.
   * - :meth:`delete_landuse(id_or_idx)`
     - Delete a land use; re-pack buildup/washoff and coverage columns.
   * - :meth:`delete_hydrograph(uh_name)`
     - Delete a unit-hydrograph group (keyed by name).

Type conversion
---------------

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Method
     - Returns
   * - :meth:`convert_node(id_or_idx, new_type)`
     - :class:`ConversionResult` summarising the conversion.
   * - :meth:`convert_link(id_or_idx, new_type)`
     - Same, for a link.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import Solver, ModelEditor, NodeType

    s = Solver("model.inp", "edited.rpt", "edited.out")
    s.create()
    s.open()                                  # state == OPENED

    editor = ModelEditor(s)
    print(f"before: {editor.node_count} nodes, {editor.link_count} links")

    # Pre-flight: what would happen if we deleted node X?
    impacts = editor.analyze_node_impact("X")
    for entry in impacts:
        print(f"  would affect {entry}")

    # Actually delete:
    actual = editor.delete_node("X")
    print(f"deleted {len(actual)} dependent items")

    # Convert J5 from a junction to a storage node:
    result = editor.convert_node("J5", NodeType.STORAGE)
    print("conversion:", result)

    from openswmm.engine import EngineState

    s.initialize()
    s.start()
    while s.state == EngineState.RUNNING:
        if s.step() != 0:
            break
    s.end()
    s.report()
    s.close()
    s.destroy()

----

Common recipes
==============

Pre-flight a deletion before committing
---------------------------------------

.. code-block:: python

    impacts = editor.analyze_node_impact("STORM_INLET_42")
    if any(e.kind == "outlet" for e in impacts):
        print("Refusing to delete — node is the outlet for a subcatchment")
    else:
        editor.delete_node("STORM_INLET_42")

Bulk-delete every dangling node
-------------------------------

.. code-block:: python

    from openswmm.engine import Nodes, Links

    nodes = Nodes(s)
    links = Links(s)
    referenced = set()
    for i in range(links.count()):
        referenced.add(links.get_from_node(i))
        referenced.add(links.get_to_node(i))

    dangling = [
        nodes.get_id(i) for i in range(nodes.count())
        if i not in referenced
    ]
    for nid in dangling:
        editor.delete_node(nid)
    print(f"deleted {len(dangling)} dangling nodes")

Convert a junction to a storage node
------------------------------------

.. code-block:: python

    from openswmm.engine import NodeType

    result = editor.convert_node("J5", NodeType.STORAGE)
    if result.warnings:
        print("warnings:", result.warnings)
    # Storage parameters default to placeholder values — set them now:
    nodes = Nodes(s)
    nodes.set_storage_functional("J5", a=10.0, b=0.0, c=0.0)

Save the edited model
---------------------

.. code-block:: python

    # ModelEditor mutates the in-memory model; the on-disk .inp is
    # unchanged.  Persist via Solver.model_write:
    s.model_write("model_edited.inp")

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method group
     - Required state
     - Notes
   * - all editor methods
     - solver in ``OPENED``
     - The editor refuses to mutate a running solver.
   * - ``analyze_*``
     - any state
     - Read-only; safe to call at any time.

Common :class:`EngineError` codes:

* ``NOT_FOUND``     — object id not in the model.
* ``INVALID_INDEX`` — integer index out of range.
* ``INVALID_TYPE``  — :meth:`convert_node` to / from an unsupported
  combination (e.g. converting a divider to an outfall when the
  topology forbids it).

----

See also
========

* :doc:`model_builder` — build a model from scratch.
* :doc:`solver` — :meth:`Solver.model_write` to persist your edits
  back to ``.inp``.
* :doc:`nodes`, :doc:`links` — populate the new objects' parameters
  after conversion.
