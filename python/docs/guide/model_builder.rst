===============================
Programmatic model construction
===============================

.. note::

   **Engine:** OpenSWMM 6 — refactored.  Documents
   :class:`openswmm.engine.ModelBuilder`.

.. currentmodule:: openswmm.engine

The :class:`ModelBuilder` constructs a complete SWMM model in Python,
without needing a ``.inp`` file.  Use it for:

* Generating models from a database or GeoJSON.
* Synthetic models for testing and benchmarking.
* Optimisation loops where the network topology is the decision
  variable.
* Templates: build a base model in code, then ``write()`` the
  resulting ``.inp`` file.

Reference: ``openswmm_model.h``.

----

Class signature
===============

.. code-block:: python

    class ModelBuilder:
        def __init__(self) -> None: ...

The builder is **standalone** — it does not need a Solver.  Once
populated and validated, call :meth:`to_solver` to obtain a Solver
ready to run, or :meth:`write(path)` to dump an ``.inp`` file.

----

Key methods
===========

Adding objects
--------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Returns
   * - :meth:`add_node(id, type)`
     - Append a node with :class:`NodeType` ``type``.
   * - :meth:`add_link(id, type)`
     - Append a link with :class:`LinkType` ``type``.
   * - :meth:`add_subcatchment(id)` / :meth:`add_subcatch(id)`
     - Append a subcatchment.
   * - :meth:`add_gage(id)`
     - Append a rain gage.
   * - :meth:`pop_last_node(id)` / :meth:`pop_last_link(id)`
     - Pop the most recently added node / link (by id sanity check).

Setting node properties
-----------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action
   * - :meth:`set_node_invert(idx, elev)`
     - Invert elevation.
   * - :meth:`set_node_max_depth(idx, depth)`
     - Maximum depth above invert.

Setting link properties
-----------------------

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Method
     - Action
   * - :meth:`set_link_nodes(idx, from_node, to_node)`
     - Connect link ``idx`` between two node indices.
   * - :meth:`set_link_length(idx, length)`
     - Conduit length.
   * - :meth:`set_link_roughness(idx, n)`
     - Manning's *n*.
   * - :meth:`set_link_xsect(idx, shape, geom1, geom2, geom3, geom4)`
     - Cross-section: :class:`XSectShape` and up to four geometry parameters.

Finalising
----------

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Method
     - Action
   * - :meth:`validate()`
     - Check the model for connectivity / consistency.  Raises on issues.
   * - :meth:`finalize()`
     - Lock the model — no more topology changes after this.
   * - :meth:`write(path)`
     - Write the assembled model to a SWMM ``.inp`` file.
   * - :meth:`handle()`
     - Opaque handle (mostly for plugin authors).
   * - :meth:`to_solver()`
     - Convert the builder into a runnable :class:`Solver`.

----

End-to-end example
==================

.. code-block:: python

    from openswmm.engine import (
        ModelBuilder, NodeType, LinkType, XSectShape,
    )

    m = ModelBuilder()

    # nodes
    j1   = m.add_node("J1",   NodeType.JUNCTION)
    out1 = m.add_node("OUT1", NodeType.OUTFALL)
    m.set_node_invert(j1,   100.0)
    m.set_node_invert(out1, 99.0)
    m.set_node_max_depth(j1, 5.0)

    # link
    c1 = m.add_link("C1", LinkType.CONDUIT)
    m.set_link_nodes(c1, j1, out1)
    m.set_link_length(c1, 300.0)
    m.set_link_roughness(c1, 0.013)
    m.set_link_xsect(c1, XSectShape.CIRCULAR, 1.0, 0.0, 0.0, 0.0)

    # validate and run
    m.validate()
    m.finalize()
    m.write("synthetic.inp")               # optional: dump for inspection

    solver = m.to_solver()
    solver.start()
    while solver.step():
        pass
    solver.end()
    solver.report()
    solver.close()
    solver.destroy()

The :meth:`to_solver` Solver behaves identically to one constructed
from a parsed ``.inp`` — every domain class, forcing, control rule,
and output reader works.

----

Common recipes
==============

Build a simple grid topology
----------------------------

.. code-block:: python

    m = ModelBuilder()

    # 4×4 grid of junctions, single outfall in the corner
    grid = {}
    for r in range(4):
        for c in range(4):
            nid = f"J_{r}_{c}"
            grid[(r, c)] = m.add_node(nid, NodeType.JUNCTION)
            m.set_node_invert(grid[(r, c)], 100.0 - 0.5 * (r + c))
            m.set_node_max_depth(grid[(r, c)], 5.0)
    out = m.add_node("OUT", NodeType.OUTFALL)
    m.set_node_invert(out, 90.0)

    # Conduits along every row and column
    cid = 0
    for r in range(4):
        for c in range(3):
            link = m.add_link(f"R_{r}_{c}", LinkType.CONDUIT)
            m.set_link_nodes(link, grid[(r, c)], grid[(r, c + 1)])
            m.set_link_length(link, 100.0)
            m.set_link_roughness(link, 0.013)
            m.set_link_xsect(link, XSectShape.CIRCULAR, 0.6, 0, 0, 0)
            cid += 1

    # Bottom-right corner connects to the outfall
    final = m.add_link("FINAL", LinkType.CONDUIT)
    m.set_link_nodes(final, grid[(3, 3)], out)
    m.set_link_length(final, 50.0)
    m.set_link_roughness(final, 0.013)
    m.set_link_xsect(final, XSectShape.CIRCULAR, 1.5, 0, 0, 0)

    m.validate()
    m.finalize()

Pop a misconfigured node
------------------------

If you've appended an object and immediately realise it's wrong, pop
it before it's referenced anywhere:

.. code-block:: python

    bad = m.add_node("typo_id", NodeType.JUNCTION)
    m.pop_last_node("typo_id")           # rolls back the last add

Build, dump, then handoff to a colleague
----------------------------------------

.. code-block:: python

    m = ModelBuilder()
    # ... build ...
    m.validate()
    m.finalize()
    m.write("scenario_a.inp")
    # Send scenario_a.inp anywhere — runs in any SWMM-compatible engine.

----

EngineState requirements & exceptions
=====================================

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Method group
     - Required state
     - Notes
   * - ``add_*``
     - pre-``validate``
     - Order matters: links reference nodes by index.
   * - ``set_*``
     - pre-``finalize``
     - After ``finalize`` topology is locked.
   * - ``validate``
     - any
     - Raises :class:`EngineError` with a descriptive message on
       inconsistency (e.g. dangling node, missing xsect).
   * - ``write`` / ``finalize`` / ``to_solver``
     - after ``validate``
     - n/a

----

See also
========

* :doc:`editing` — modify an existing parsed model in place.
* :doc:`solver` — run the model returned by :meth:`to_solver`.
* :doc:`nodes`, :doc:`links`, :doc:`subcatchments` — query / mutate
  state once the Solver is open.
