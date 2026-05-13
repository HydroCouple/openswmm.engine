.. _api_documentation:

=================
API Reference
=================

.. contents:: Module Index
   :local:
   :depth: 2

This page is generated from the Cython type-stub files (``.pyi``) so
signatures and docstrings are always in sync with the compiled extensions.
Each entry below lists the public class for the module; private helpers
prefixed with ``_`` are intentionally hidden.

For task-oriented documentation see the :doc:`User Guide <guide/index>`.

openswmm
========

The top-level package.  Importing :mod:`openswmm` configures the
platform-specific shared-library search path and re-exports the legacy
SWMM 5 symbols for backward compatibility.

.. automodule:: openswmm
   :members:
   :no-undoc-members:

----

OpenSWMM 6 — refactored engine
==============================

The :mod:`openswmm.engine` subpackage exposes the **new, refactored
OpenSWMM 6 engine** — the recommended API for all new work.

All domain access is class-based: each class takes an active
:class:`~openswmm.engine._solver.Solver` in its constructor and may only be
called when the solver is in an appropriate :class:`EngineState`.

.. seealso::

   :doc:`guide/index` — task-oriented user guide for this engine.

.. automodule:: openswmm.engine
   :no-members:

Solver lifecycle
----------------

.. automodule:: openswmm.engine._solver
   :members:
   :undoc-members:
   :show-inheritance:

Model construction
------------------

.. automodule:: openswmm.engine._model
   :members:
   :undoc-members:
   :show-inheritance:

Model editing  (deletion + type conversion)
--------------------------------------------

.. automodule:: openswmm.engine._edit
   :members:
   :undoc-members:
   :show-inheritance:

Nodes
-----

.. automodule:: openswmm.engine._nodes
   :members:
   :undoc-members:
   :show-inheritance:

Links
-----

.. automodule:: openswmm.engine._links
   :members:
   :undoc-members:
   :show-inheritance:

Subcatchments
-------------

.. automodule:: openswmm.engine._subcatchments
   :members:
   :undoc-members:
   :show-inheritance:

Rain gages
----------

.. automodule:: openswmm.engine._gages
   :members:
   :undoc-members:
   :show-inheritance:

External inflows  (DWF, RDII, time-series)
------------------------------------------

.. automodule:: openswmm.engine._inflows
   :members:
   :undoc-members:
   :show-inheritance:

Advanced forcing  (mode + persistence)
--------------------------------------

.. automodule:: openswmm.engine._forcing
   :members:
   :undoc-members:
   :show-inheritance:

Control rules
-------------

.. automodule:: openswmm.engine._controls
   :members:
   :undoc-members:
   :show-inheritance:

Pollutants
----------

.. automodule:: openswmm.engine._pollutants
   :members:
   :undoc-members:
   :show-inheritance:

Water quality  (landuse, buildup, washoff, treatment)
-----------------------------------------------------

.. automodule:: openswmm.engine._quality
   :members:
   :undoc-members:
   :show-inheritance:

Tables  (time series, curves, patterns)
---------------------------------------

.. automodule:: openswmm.engine._tables
   :members:
   :undoc-members:
   :show-inheritance:

Infrastructure  (transects, streets, inlets, LIDs)
--------------------------------------------------

.. automodule:: openswmm.engine._infrastructure
   :members:
   :undoc-members:
   :show-inheritance:

Hot start
---------

.. automodule:: openswmm.engine._hotstart
   :members:
   :undoc-members:
   :show-inheritance:

Mass balance
------------

.. automodule:: openswmm.engine._massbalance
   :members:
   :undoc-members:
   :show-inheritance:

Statistics
----------

.. automodule:: openswmm.engine._statistics
   :members:
   :undoc-members:
   :show-inheritance:

Output reader  (binary ``.out`` file)
-------------------------------------

.. automodule:: openswmm.engine._output_reader
   :members:
   :undoc-members:
   :show-inheritance:

Spatial  (CRS, coordinates, vertices, polygons)
-----------------------------------------------

.. automodule:: openswmm.engine._spatial
   :members:
   :undoc-members:
   :show-inheritance:

Enumerations
------------

.. automodule:: openswmm.engine._enums
   :members:
   :undoc-members:
   :show-inheritance:
   :no-index:

Optional modules
----------------

These extensions are only built when the matching CMake flag is enabled
on the parent C/C++ engine.  They expose ``HAS_*`` flags at runtime so
client code can degrade gracefully.

GeoPackage I/O  (``OPENSWMM_WITH_GEOPACKAGE=ON``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. automodule:: openswmm.engine._geopackage
   :members:
   :undoc-members:
   :show-inheritance:

2-D surface routing  (``OPENSWMM_BUILD_2D=ON``)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

.. automodule:: openswmm.engine._2d
   :members:
   :undoc-members:
   :show-inheritance:

Legacy SWMM 5 — compatibility layer
====================================

The :mod:`openswmm.legacy.engine` and :mod:`openswmm.legacy.output`
subpackages preserve the original EPA SWMM 5.x C solver and binary-output
reader **verbatim** for backward compatibility with existing scripts.
No new development happens here; new projects should prefer
:mod:`openswmm.engine` (above).

.. seealso::

   :doc:`legacy/index` — task-oriented guide for the legacy layer.

   :doc:`migration/swmm5_to_swmm6` — translating legacy patterns to
   the OpenSWMM 6 API.

openswmm.legacy.engine — SWMM 5.x solver
-----------------------------------------

.. automodule:: openswmm.legacy.engine
   :members:
   :undoc-members:
   :imported-members:
   :show-inheritance:

----

openswmm.legacy.output — SWMM 5.x binary output reader
-------------------------------------------------------

The :mod:`openswmm.legacy.output` subpackage reads the binary ``.out`` file
produced by either the legacy SWMM 5.x solver or the v6.0 engine.

.. automodule:: openswmm.legacy.output
   :members:
   :undoc-members:
   :imported-members:
   :show-inheritance:
