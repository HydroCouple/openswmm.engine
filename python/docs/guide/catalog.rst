==============
Engine catalog
==============

:mod:`openswmm.engine.catalog` is a machine-readable map of the Python API,
shipped in the wheel as ``catalog.json``. It lists every service, element
kind, sub-view and standalone class, and for each member:

* a **property**'s type, access (``r`` or ``rw``), unit kind and the
  lifecycle phases it can be read in;
* a **method**'s parameters, defaults and return type;
* the C API functions behind it;
* the bulk array that reads a whole collection at once, where there is one.

The OpenSWMM MCP server and ``openswmm.gymnasium`` use it to reach engine
fields and methods by path. Existing generic dispatchers can adopt new scalar
fields and compatible method signatures, but availability, lifecycle, argument
marshalling and domain-specific behavior still require consumer tests.

Paths
=====

A **target** is a class reachable from a :class:`~openswmm.engine.Solver`:
a service (``"forcing"``, ``"groundwater2d"``), an element kind
(``"node"``, ``"link"``), a sub-view of one (``"node.stats"``,
``"link.xsect"``) or a standalone class (``"xsect"``, ``"output"``). A
**member path** is a target plus a member name: ``"node.depth"``,
``"link.stats.max_filling"``, ``"forcing.node_lat_inflow"``.

An alternate route to the same service is catalogued as a property with a
``view_target`` pointing to its canonical target. For example,
``catalog.lookup("surface2d.groundwater")["view_target"]`` is
``"groundwater2d"``. Use that target to discover the shared view's members.

.. code-block:: python

   from openswmm.engine import Solver, catalog

   entry = catalog.lookup("link.stats.max_filling")
   entry["type"], entry["access"], entry["units"]      # ('float', 'r', 'fraction')

   [m["name"] for m in catalog.members("node.stats")]  # max_depth, max_overflow, ...

   call = catalog.lookup("forcing.node_lat_inflow")
   [(p["name"], p["required"]) for p in call["params"]]
   # [('node', True), ('value', True), ('mode', False), ('persist', False)]

   with Solver("model.inp", "model.rpt", "model.out") as solver:
       stats = catalog.resolve(solver, "node.stats", "J1")   # the live NodeStatsView
       catalog.lookup("node.depth")["bulk"]                  # 'nodes.depths'

Functions
=========

``load()``
   The whole catalog as a dict (``targets``, ``members``, ``functions``,
   ``records``, ``enums`` and the C exports).
``targets()``
   Every target by name, with its class, module and, for element kinds, the
   collection that holds them.
``members(target)``
   The members of one target (or of all targets).
``lookup(path)``
   One member by path; raises :class:`KeyError` naming the path.
``resolve(solver, target, key)``
   The live object for a target on an open solver; ``key`` is an element ID
   or index for element targets.
``unit_label(kind, unit_system, flow_units)``
   The label for a unit kind in a model's units.

Units
=====

Ordinary 1D fields use model units; fixed-unit fields, including many 2D
quantities and temperature values, retain the units documented by their APIs.
The catalog records a **unit
kind** per float field: a system-dependent kind (``length``, ``volume``,
``flow``, ``velocity``, ...), a fixed one (``fraction``, ``percent``,
``dimensionless``, ``temperature``, ...) or a literal unit (``m/s``).
``unit_label`` turns a kind into the label for a model:

.. code-block:: python

   with Solver("model.inp", "model.rpt", "model.out") as solver:
       kind = catalog.lookup("node.depth")["units"]                      # 'length'
       catalog.unit_label(kind, solver.unit_system, solver.flow_units)   # 'ft'
       catalog.unit_label("flow", solver.unit_system, solver.flow_units) # 'CFS'

``UNIT_KINDS`` is the set of symbolic kinds that have a label.

Keeping the catalog current
===========================

``python/scripts/gen_catalog.py`` generates ``catalog.json`` from the type
stubs, the Cython sources and the C header documentation. Units, exclusions
and member corrections the sources cannot express live in
``python/scripts/catalog_overrides.json``. After changing a stub, a ``.pyx``
file or a header, regenerate it:

.. code-block:: bash

   python python/scripts/gen_catalog.py          # rewrite catalog.json
   python python/scripts/gen_catalog.py --check  # what CI runs

``--check`` fails when the committed catalog is stale, when a public class is
neither reachable nor excluded, or when a float field has no unit kind (mark
a true ratio ``dimensionless``). ``python/tests/test_catalog.py`` adds the
runtime contract: every catalogued member exists on its compiled class,
every public attribute is catalogued or excluded, and every target resolves
on an opened model.


Consumer compatibility
======================

* Gymnasium ``add_field`` consumes scalar numeric element properties; its
  per-cell collector supports selected 2D service methods. Writable metadata
  does not guarantee that a field can be actuated during a running episode.
* MCP dispatches JSON-compatible catalog operations. Session-owned lifecycle
  methods and Python callbacks are deliberately restricted; new standalone
  classes require constructor handling.
* WASM generates a raw C API from its **pinned engine submodule**, plus numeric
  enums from the Python source. This is independent of the Python catalog.
  It builds the 2D module without its HDF5 results writer
  (``OPENSWMM_2D_HDF5_OUTPUT=OFF``) and mirrors ``Surface2D`` and its
  infiltration view in TypeScript; mesh groundwater, surface quality and the
  rest of the Python object API are reached through its raw layer.

After adding native exports or changing a stub, rebuild the compiled extensions,
regenerate the catalog, and run both engine and consumer contract tests. An
unchanged package version does not prove that two development builds expose the
same catalog or native symbols. A missing newly added extension (for example
``_transport``) requires a complete rebuild/reinstall, not an import fallback.
