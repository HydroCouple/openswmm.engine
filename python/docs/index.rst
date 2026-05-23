.. image:: images/hydrocouple_logo.png
   :alt: HydroCouple Logo
   :width: 200px

========
OpenSWMM
========

**Open Storm Water Management Model — Python bindings.**

OpenSWMM is a community-driven, open-source continuation of the EPA Storm
Water Management Model (SWMM) — a dynamic hydrology / hydraulics / water
quality simulation model used for single-event and long-term simulation of
runoff quantity and quality from primarily urban areas.

The :mod:`openswmm` package ships **two engines** that share the same
package namespace:

.. grid:: 2

    .. grid-item-card::  OpenSWMM 6 — Refactored Engine
        :link: guide/index
        :link-type: doc

        :mod:`openswmm.engine`

        The new reentrant C++20 engine.  Domain-split API (Nodes, Links,
        Forcing, Controls, …), bulk numpy accessors, programmatic model
        construction, in-place editing, plugin SDK.  **Recommended for
        all new work.**

    .. grid-item-card::  Legacy SWMM 5 — Compatibility Layer
        :link: legacy/index
        :link-type: doc

        :mod:`openswmm.legacy.engine` and :mod:`openswmm.legacy.output`

        The original EPA SWMM 5.x solver and binary-output reader, preserved
        verbatim for backward compatibility with existing scripts.  No new
        development; bug fixes only.

If you are coming from a SWMM 5 codebase, see
:doc:`migration/swmm5_to_swmm6` for a side-by-side translation.

----

Where to start
==============

.. grid:: 2

    .. grid-item-card:: New to OpenSWMM
        :link: guide/quickstart
        :link-type: doc

        A 10-minute walkthrough using the v6 engine: install, run, query,
        plot.

    .. grid-item-card:: Coming from SWMM 5?
        :link: migration/swmm5_to_swmm6
        :link-type: doc

        Side-by-side translation guide.

    .. grid-item-card:: User Guide
        :link: guide/index
        :link-type: doc

        Topic-by-topic guides for every domain class in the v6 engine.

    .. grid-item-card:: API Reference
        :link: api
        :link-type: doc

        Full class & method reference, generated from the Cython type
        stubs.  Both engines.

----

.. toctree::
   :maxdepth: 2
   :hidden:
   :caption: Getting started

   guide/install
   guide/quickstart
   guide/concepts

.. toctree::
   :maxdepth: 2
   :hidden:
   :caption: OpenSWMM 6 — Refactored Engine

   guide/index

.. toctree::
   :maxdepth: 2
   :hidden:
   :caption: Legacy SWMM 5 — Compatibility

   legacy/index
   legacy/solver
   legacy/output

.. toctree::
   :maxdepth: 1
   :hidden:
   :caption: Migration

   migration/swmm5_to_swmm6

.. toctree::
   :maxdepth: 2
   :hidden:
   :caption: Reference

   api
   license

----

Project at a glance
===================

.. list-table::
   :widths: 30 70

   * - **Audience**
     - Hydrologists, stormwater engineers, researchers, app developers.
   * - **Engines**
     - OpenSWMM 6 (reentrant C++20, ``openswmm.engine``) and the
       preserved SWMM 5.x solver (``openswmm.legacy.engine``).
   * - **Language**
     - Python ≥ 3.9 with NumPy ≥ 1.21 (NumPy 2.x supported).
   * - **Platforms**
     - macOS (arm64 + x86_64), Linux (x86_64 + aarch64), Windows (x64).
   * - **Build system**
     - scikit-build-core + CMake + Cython.
   * - **License**
     - MIT.

Related documentation
=====================

* `OpenSWMM C/C++ engine docs <https://hydrocouple.org/openswmm.engine/>`_ —
  the underlying engine these Python bindings wrap.  Full C API
  reference, technical reference manuals (Hydrology, Hydraulics, Water
  Quality), user manual, architecture & design notes.  *(The "C/C++
  Engine Docs" link in the top navigation also goes here.)*
* `Source repository on GitHub <https://github.com/HydroCouple/openswmm.engine>`_ —
  source, build instructions, contributing guide, issue tracker.
