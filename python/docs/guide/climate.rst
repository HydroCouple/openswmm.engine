=======
Climate
=======

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

The :class:`Climate` view, reached via ``solver.climate``, reads and
edits the model's climate *configuration* — the data parsed from the
``[TEMPERATURE]``, ``[EVAPORATION]``, ``[WINDSPEED]`` and
``[ADJUSTMENTS]`` sections, plus the snowmelt globals and
areal-depletion curves.

It complements :doc:`forcing`: ``solver.forcing`` overrides climate
*state* while a simulation is RUNNING; ``solver.climate`` edits the
*inputs* before a run. Values use the project's display units, exactly
as in the ``.inp`` file. Setters require an editable lifecycle state
(before ``initialize()``) and raise :class:`EngineError` otherwise.

Reference: ``openswmm_climate.h``.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        s.climate.evap_type = 1                       # MONTHLY
        s.climate.evap_monthly = [0.1] * 12
        s.climate.latitude = 41.5
        s.climate.adc_impervious = [1.0, 0.9, 0.8, 0.7, 0.6,
                                    0.5, 0.4, 0.3, 0.2, 0.1]

What's on the view
==================

* **Temperature** — ``temp_source``, ``temp_timeseries``,
  ``temp_file_start``, ``temp_units``, ``elevation``, ``latitude``,
  ``longitude_correction``.
* **Evaporation** — ``evap_type``, ``evap_monthly``,
  ``evap_timeseries``, ``pan_coeff``, ``evap_recovery``.
* **Wind speed** — ``wind_type``, ``wind_monthly``.
* **Snowmelt globals** — ``snow_temp``, ``ati_weight``,
  ``neg_melt_ratio``.
* **Areal-depletion curves** — ``adc_impervious``, ``adc_pervious``
  (10 values each).
* **Monthly adjustments** — ``adjust_temperature``,
  ``adjust_evaporation``, ``adjust_rainfall``,
  ``adjust_conductivity`` (12 values each; the ``[ADJUSTMENTS]``
  section).

Monthly tables are plain Python lists — read, modify, assign back:

.. code-block:: python

    adj = s.climate.adjust_rainfall
    adj[5] *= 1.10                       # +10 % June rainfall
    s.climate.adjust_rainfall = adj

API reference
=============

:class:`Climate` in the :doc:`full API listing </api>`.
