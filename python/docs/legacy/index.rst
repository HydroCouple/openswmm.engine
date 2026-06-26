==========================================
Legacy SWMM 5 — Compatibility Layer
==========================================

.. note::

   **Engine:** SWMM 5.x — *legacy*.  The pages in this section document
   :mod:`openswmm.legacy.engine` and :mod:`openswmm.legacy.output`.  For
   the modern, recommended OpenSWMM 6 engine see
   :doc:`../guide/index`.

The :mod:`openswmm.legacy` namespace preserves the original EPA SWMM 5.x
C solver and its binary-output reader **verbatim**, with thin Cython
wrappers, so existing scripts that target SWMM 5 continue to run
unchanged.

This is a **compatibility layer**, not a fork:

* The legacy code is **the same source** that EPA released; we apply
  bug fixes only.
* No new features — every new feature lands in
  :mod:`openswmm.engine` instead.
* The two layers can coexist in a single Python process and both can
  read each other's binary ``.out`` files.

When to use which
=================

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Use :mod:`openswmm.legacy.engine` when …
     - Use :mod:`openswmm.engine` when …
   * - You have working SWMM 5 Python code and want it to keep
       running unchanged.
     - You're starting a new project, or you've decided to migrate.
   * - You depend on a specific SWMM 5 numerical detail (e.g. a third-
       party validation against EPA SWMM 5 outputs).
     - You want bulk numpy accessors, programmatic model construction,
       in-place editing, or the plugin SDK.
   * - You need the exact ``.rpt`` text formatting that SWMM 5 produces.
     - You want multiple independent simulations in one process
       (reentrant engine).

If you're sitting on a pile of SWMM 5 Python code today, see
:doc:`../migration/swmm5_to_swmm6` for a side-by-side translation.

Layout
======

.. toctree::
   :maxdepth: 1

   solver
   output

* :doc:`solver` — the legacy :class:`Solver` class:
  lifecycle, the enum-driven ``getValue`` / ``setValue`` API, progress
  callbacks, exceptions.
* :doc:`output` — the legacy :class:`Output` reader for the
  binary ``.out`` file.

For the API reference (every public class and method) see the
:ref:`Legacy section of the API page <api_documentation>`.
