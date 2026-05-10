==========================================
OpenSWMM 6 User Guide  (refactored engine)
==========================================

.. note::

   These guides cover the **OpenSWMM 6 refactored engine** at
   :mod:`openswmm.engine`.  If you are looking for the original EPA SWMM
   5.x solver — preserved verbatim for backward compatibility — see
   :doc:`../legacy/index`.

The User Guide is the canonical "how do I do X?" reference for the
modern :mod:`openswmm.engine` API.  Every page follows the same layout:

* **Class signature** — the constructor and how the class fits in.
* **Key methods** — grouped by purpose (geometry, state, forcing, …).
* **End-to-end example** — a runnable snippet covering the typical
  workflow.
* **Common recipes** — short snippets for everyday tasks.
* **Bulk arrays** — the numpy-friendly accessors (``*_bulk``).
* **EngineState requirements & exceptions** — what's legal when, and
  what each method raises.

Newcomers, start with :doc:`../guide/install`,
:doc:`../guide/quickstart`, then :doc:`../guide/concepts`.

----

Running a simulation
====================

.. toctree::
   :maxdepth: 1

   solver
   error_handling

Domain access
=============

The "what's the depth of node J1 right now?" surface.  One page per
domain.

.. toctree::
   :maxdepth: 1

   nodes
   links
   subcatchments
   gages

Forcing & control
=================

Inject runtime inflows, override rainfall and time-series, fire
control actions.

.. toctree::
   :maxdepth: 1

   inflows
   forcing
   controls
   tables

Water quality
=============

Pollutants, landuse, buildup/washoff, treatment.

.. toctree::
   :maxdepth: 1

   pollutants
   quality

Outputs
=======

Reading the binary ``.out`` file, hot-start save / restore, mass
balance, accumulated statistics.

.. toctree::
   :maxdepth: 1

   output_reader
   hotstart
   massbalance
   statistics

Specialised topics
==================

Build a model from scratch in Python; mutate an open model in place;
configure complex hydraulic infrastructure; spatial / CRS handling.

.. toctree::
   :maxdepth: 1

   model_builder
   editing
   infrastructure
   spatial
