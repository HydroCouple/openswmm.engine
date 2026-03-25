.. _api_documentation:

=================
API Documentation
=================

.. contents:: Modules
   :local:
   :depth: 2

openswmm
--------

.. automodule:: openswmm
   :members:
   :no-undoc-members:

Legacy Solver (v5.1 API)
-------------------------

The :mod:`openswmm.legacy.engine` subpackage wraps the legacy SWMM 5.1 C
solver library via Cython. Type information is provided by the bundled
``.pyi`` stub file so that Sphinx autodoc can extract signatures even when
the compiled extension is not available.

.. note::

   For backward compatibility, ``openswmm.solver`` is a thin shim that
   re-exports everything from ``openswmm.legacy.engine``.

.. automodule:: openswmm.legacy.engine
   :members:
   :undoc-members:
   :imported-members:
   :show-inheritance:

Legacy Output
--------------

The :mod:`openswmm.legacy.output` subpackage reads binary SWMM ``.out``
result files.

.. note::

   For backward compatibility, ``openswmm.output`` is a thin shim that
   re-exports everything from ``openswmm.legacy.output``.

.. automodule:: openswmm.legacy.output
   :members:
   :undoc-members:
   :imported-members:
   :show-inheritance:

Engine (v6.0 API)
-----------------

The :mod:`openswmm.engine` subpackage exposes the new OpenSWMM v6.0 engine
API. All domain-object access is class-based: each class takes a
:class:`~openswmm.engine._solver.Solver` context in its constructor.

.. automodule:: openswmm.engine
   :no-members:

Solver Lifecycle
^^^^^^^^^^^^^^^^

.. automodule:: openswmm.engine._solver
   :members:
   :undoc-members:
   :show-inheritance:

Model Builder
^^^^^^^^^^^^^

.. automodule:: openswmm.engine._model
   :members:
   :undoc-members:
   :show-inheritance:

Nodes
^^^^^

.. automodule:: openswmm.engine._nodes
   :members:
   :undoc-members:
   :show-inheritance:

Links
^^^^^

.. automodule:: openswmm.engine._links
   :members:
   :undoc-members:
   :show-inheritance:

Subcatchments
^^^^^^^^^^^^^

.. automodule:: openswmm.engine._subcatchments
   :members:
   :undoc-members:
   :show-inheritance:

Gages
^^^^^

.. automodule:: openswmm.engine._gages
   :members:
   :undoc-members:
   :show-inheritance:

Hot Start
^^^^^^^^^

.. automodule:: openswmm.engine._hotstart
   :members:
   :undoc-members:
   :show-inheritance:

Mass Balance
^^^^^^^^^^^^

.. automodule:: openswmm.engine._massbalance
   :members:
   :undoc-members:
   :show-inheritance:

Enumerations
^^^^^^^^^^^^

.. automodule:: openswmm.engine._enums
   :members:
   :undoc-members:
   :show-inheritance:
   :no-index:
