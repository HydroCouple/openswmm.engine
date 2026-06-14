================
External inflows
================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

External inflows, dry-weather flows, RDII, unit hydrographs, and
inflow-area decay all live behind ``solver.inflows``.

Reference: ``openswmm_inflows.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        # External inflow — node accepts id or index.
        s.inflows.add_external("J1", "FLOW", ts_name="rain1")

        # Dry-weather flow.
        s.inflows.add_dwf(
            "J1", "FLOW",
            avg_value=0.5,
            hourly_pattern="DLY1",
        )

        # RDII inflow with unit hydrograph.
        s.inflows.add_rdii("J1", uh_name="UH1", area=2.5)

        # Inspect counts.
        print(s.inflows.external_count, s.inflows.dwf_count, s.inflows.rdii_count)

        # Per-row read (RDII, hydrographs, decay — C API supports get).
        rdii = s.inflows.get_rdii(0)
        print(rdii.node_index, rdii.uh_name, rdii.area)

----

Methods
=======

External inflows (``[INFLOWS]``)
--------------------------------

* ``add_external(node, constituent, *, ts_name, type, m_factor, s_factor, baseline, pattern)``
* ``external_count`` property

Dry-weather flow (``[DWF]``)
----------------------------

* ``add_dwf(node, constituent, *, avg_value, monthly_pattern, daily_pattern, hourly_pattern, weekend_pattern)``
* ``dwf_count`` property

RDII (``[RDII]``)
-----------------

* ``add_rdii(node, uh_name, area)``
* ``get_rdii(idx) -> RDIIEntry``
* ``rdii_count`` property

Unit hydrographs (``[HYDROGRAPHS]``)
------------------------------------

* ``add_hydrograph(uh_name, month, response, r, t, k, *, dmax, drecov, dinit)``
* ``get_hydrograph(idx) -> HydrographEntry``
* ``hydrograph_count`` property
* ``add_hydrograph_gage(uh_name, gage_name)`` / ``get_hydrograph_gage(idx) -> HydrographGageEntry``
* ``hydrograph_gage_count`` / ``hydrograph_group_count`` properties
* ``get_hydrograph_group_id(idx)``

RDII decay (``[RDII_DECAY]``)
-----------------------------

* ``add_rdii_decay(uh_name, response, k_dep, k_0, k_T, T_ref, theta_rec, T_freeze)``
* ``get_rdii_decay(idx) -> RDIIDecayEntry``
* ``rdii_decay_count`` property

All ``node``/``link``/``subcatchment``/``gage`` arguments accept
``int | str``.

----

C API constraint
================

The C side only exposes ``add`` + ``count`` for external inflows and
DWF — there is no per-row ``delete`` / ``set`` / ``get``. The Python
view doesn't pretend to be a :class:`MutableSequence` for those
families. RDII, hydrographs, and RDII decay all have ``get`` accessors
so per-row reading works.

----

See also
========

* :doc:`tables` — :meth:`Tables.add_timeseries` and
  :class:`Pattern` referenced by ``ts_name`` / ``*_pattern`` args.
* :doc:`forcing` — runtime override of inflows (``node_lat_inflow``).
* :doc:`error_handling`.
