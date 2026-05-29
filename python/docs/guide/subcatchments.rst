=============
Subcatchments
=============

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

Subcatchments are the runoff-generating polygons. The shape mirrors
:doc:`nodes` and :doc:`links` — a collection on
``solver.subcatchments`` that yields :class:`Subcatchment` wrappers.

Reference: ``openswmm_subcatchments.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver, InfilModel

    with Solver("model.inp") as s:
        sc = s.subcatchments["S1"]
        print(sc.area, sc.imperv_pct, sc.slope)

        # Topology — back-references to Gage / Node wrappers.
        print(sc.gage.id, "->", sc.outlet.id)

        # Infiltration as a tagged-union view.
        print(sc.infiltration.model)
        sc.infiltration.set_horton(3.0, 0.5, 4.0, 7.0)

        # Coverage as a MutableMapping.
        sc.coverage["RESIDENTIAL"] = 0.6

        # Runtime state + bulk numpy.
        print(sc.runoff, sc.rainfall)
        runoffs = s.subcatchments.runoffs

----

Collection: :class:`Subcatchments`
==================================

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Operation
     - What it does
   * - ``len(s.subcatchments)``
     - Count.
   * - ``s.subcatchments[key]``
     - Returns a :class:`Subcatchment`. ``key`` is ``int`` or ``str``.
   * - ``for sc in s.subcatchments:``
     - Yields a fresh :class:`Subcatchment` per index.
   * - ``s.subcatchments.add(id)``
     - Append a subcatchment.
   * - ``s.subcatchments.rename(key, new_id)``
     - Rename in place. Invalidates wrappers.

Bulk numpy properties
---------------------

.. list-table::
   :header-rows: 1
   :widths: 22 14 64

   * - Property
     - Mode
     - Meaning
   * - ``runoffs``
     - read-only
     - Per-subcatchment runoff rate.
   * - ``rainfalls``
     - read-only
     -
   * - ``evaps``
     - read-only
     -
   * - ``infils``
     - read-only
     - Infiltration rate.
   * - ``snow_depths``
     - read-only
     -
   * - ``ids``
     - read-only
     - String ids as ``numpy.ndarray`` (``dtype=object``).

Per-pollutant concentrations:

.. code-block:: python

    s.subcatchments.qualities("TSS")    # by str or int

----

Wrapper: :class:`Subcatchment`
==============================

.. list-table::
   :header-rows: 1
   :widths: 22 14 14 50

   * - Property
     - Type
     - Mode
     - Meaning
   * - ``id`` / ``index``
     - ``str`` / ``int``
     - read-only
     -
   * - ``area``
     - ``float``
     - read/write
     -
   * - ``width``
     - ``float``
     - read/write
     -
   * - ``slope``
     - ``float``
     - read/write
     -
   * - ``imperv_pct``
     - ``float``
     - read/write
     - Impervious fraction (%).
   * - ``n_imperv`` / ``n_perv``
     - ``float``
     - read/write
     - Manning's n.
   * - ``ds_imperv`` / ``ds_perv``
     - ``float``
     - read/write
     - Depression storage.
   * - ``gage``
     - :class:`Gage`
     - read/write
     - Rain gage wrapper.
   * - ``outlet``
     - :class:`Node` or :class:`Subcatchment`
     - read-only
     - Whichever outlet target is configured.
   * - ``runoff`` / ``rainfall`` / ``snow_depth`` / ``evap`` / ``infil``
     - ``float``
     - read-only
     - Runtime state.
   * - ``rainfall``
     - ``float``
     - read/write
     - Override rainfall (writer is the same property).
   * - ``groundwater``
     - ``float``
     - read-only
     -

Methods:

.. list-table::
   :header-rows: 1
   :widths: 38 62

   * - Method
     - What it does
   * - ``set_outlet_node(node)``
     - Route runoff to a :class:`Node` (or id / index).
   * - ``set_outlet_subcatchment(sub)``
     - Route runoff to another :class:`Subcatchment`.
   * - ``quality(pollutant)``
     - Runoff concentration of ``pollutant``.
   * - ``ponded_quality(pollutant)`` / ``set_ponded_quality(pollutant, mass)``
     - Ponded mass balance.

----

Infiltration view (tagged union)
================================

``subcatchment.infiltration`` is a tagged-union view exposing whichever
parameter family matches the active model. Reading the wrong family
raises a :class:`BadParamError` (which is also a :exc:`ValueError`):

.. code-block:: python

    sc = s.subcatchments["S1"]

    if sc.infiltration.model in (InfilModel.HORTON, InfilModel.MOD_HORTON):
        f0, fmin, decay, dry_time = sc.infiltration.horton
    elif sc.infiltration.model in (InfilModel.GREEN_AMPT, InfilModel.MOD_GREEN_AMPT):
        suction, ksat, deficit = sc.infiltration.green_ampt
    else:                                       # CURVE_NUMBER
        cn = sc.infiltration.curve_number

Writers force the model to match:

.. code-block:: python

    sc.infiltration.set_horton(3.0, 0.5, 4.0, 7.0)
    sc.infiltration.set_green_ampt(3.5, 0.06, 0.26)
    sc.infiltration.set_curve_number(85.0)

----

Coverage view
=============

``subcatchment.coverage`` is a :class:`MutableMapping` over landuse ids
→ fractions. Iteration yields landuse ids whose fraction is non-zero:

.. code-block:: python

    sc.coverage["RESIDENTIAL"] = 0.6
    sc.coverage["COMMERCIAL"] = 0.4
    sum(sc.coverage.values())          # ≤ 1.0
    for landuse, frac in sc.coverage.items():
        print(landuse, frac)

Deletion is not supported (the C side stores a dense per-landuse
array); set the fraction to ``0.0`` instead.

----

Statistics sub-view
===================

Same shape as nodes/links — meaningful after :meth:`Solver.end`:

.. code-block:: python

    print(sc.stats.precip)            # cumulative precipitation
    print(sc.stats.runoff_vol)        # cumulative runoff volume
    print(sc.stats.max_runoff)        # peak runoff rate

----

Staleness, equality, repr
=========================

Identical contract to :class:`Node` / :class:`Link`. Adding, renaming,
or deleting subcatchments invalidates wrappers minted before the
change.

----

See also
========

* :doc:`solver` — where ``s.subcatchments`` comes from.
* :doc:`gages` — :attr:`Subcatchment.gage` yields a :class:`Gage`.
* :doc:`nodes` — :attr:`Subcatchment.outlet` may yield a :class:`Node`.
* :doc:`error_handling`.
