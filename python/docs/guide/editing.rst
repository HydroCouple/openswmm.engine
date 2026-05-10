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
  would affect": kind, id, description.
* :class:`ConversionResult` — outcome of a type conversion: success,
  warnings, fields that had to be defaulted.

Both are plain data classes with ``__repr__`` so they display well
during interactive exploration.

----

Key methods
===========

Counts
------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Returns
   * - :meth:`node_count` / :meth:`link_count` / :meth:`subcatch_count` / :meth:`gage_count` / :meth:`table_count`
     - Number of objects of each kind currently in the model.

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
    print(f"before: {editor.node_count()} nodes, {editor.link_count()} links")

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

    s.initialize()
    s.start()
    while s.step():
        pass
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
