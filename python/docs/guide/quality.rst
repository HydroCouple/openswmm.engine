=====================================================
Water quality  (landuse, buildup, washoff, treatment)
=====================================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

``solver.quality`` configures landuses and their pollutant
buildup/washoff functions, plus node-level treatment expressions.

Reference: ``openswmm_quality.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver, BuildupFunc, WashoffFunc

    with Solver("model.inp") as s:
        s.quality.landuses.add("RESIDENTIAL")

        s.quality.set_buildup(
            "RESIDENTIAL", "TSS",
            func=BuildupFunc.POW, c1=10.0, c2=0.1, c3=2.0)
        info = s.quality.get_buildup("RESIDENTIAL", "TSS")
        print(info["func"], info["c1"])

        s.quality.set_washoff(
            "RESIDENTIAL", "TSS",
            func=WashoffFunc.EXP, coeff=0.5, expon=2.0)

        # Per-node treatment expression.
        s.quality.set_treatment("J1", "TSS", "R = 0.5*C")

----

Landuses sub-collection
=======================

.. code-block:: python

    lu = s.quality.landuses["RESIDENTIAL"]
    lu.sweep_interval = 7.0       # days
    lu.sweep_removal = 0.6

    for lu in s.quality.landuses:
        print(lu.id, lu.sweep_interval)

    s.quality.landuses.add("COMMERCIAL")

``solver.quality.landuses`` is indexable by ``int | str`` and iterable;
items are :class:`Landuse` wrappers with typed properties.

----

Buildup / washoff
=================

Both APIs accept ``int | str`` for the landuse and pollutant
selectors. The function-type argument is an enum
(:class:`BuildupFunc` / :class:`WashoffFunc`) — passing an integer
still works.

``set_buildup(landuse, pollutant, *, func, c1, c2, c3, normalizer=0)``
``get_buildup(landuse, pollutant) -> Dict[str, ...]`` returns
``{"func": BuildupFunc, "c1": float, "c2": float, "c3": float,
"normalizer": int}``.

``set_washoff(landuse, pollutant, *, func, coeff, expon,
sweep_effic=0.0, bmp_effic=0.0)``
``get_washoff(landuse, pollutant) -> Dict[str, ...]`` returns
``{"func": WashoffFunc, "coeff": float, "expon": float,
"sweep_effic": float, "bmp_effic": float}``.

----

Treatment
=========

.. code-block:: python

    s.quality.set_treatment("J1", "TSS", "R = 0.5*C")
    print(s.quality.get_treatment("J1", "TSS"))
    s.quality.clear_treatment("J1", "TSS")

Treatment expressions follow SWMM's standard syntax (see the SWMM 5
reference manual).

----

See also
========

* :doc:`pollutants` — pollutant configuration.
* :doc:`subcatchments` — :attr:`Subcatchment.coverage` connects
  landuses to subcatchments.
* :doc:`massbalance` — :meth:`MassBalance.quality_continuity_error`.
* :doc:`error_handling`.
