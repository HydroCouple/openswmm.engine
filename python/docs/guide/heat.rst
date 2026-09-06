==========================================================
Heat transport  (fluxes, solar, cloud, source temperature)
==========================================================

.. note::

   **Engine:** OpenSWMM 6 — refactored.

.. currentmodule:: openswmm.engine

``solver.heat`` is the editable view over the ``model.heat`` component:
the ``[HEAT_FLUXES]`` module toggles, the ``[RADIATIVE_FLUXES]`` scalar
parameters, H6a's ``[SOLAR_RADIATION]`` and ``[CLOUD_COVER]`` sections,
and the ``[HEAT_SOURCES]`` inlet-temperature table. Every refusal below
mirrors the deck parser exactly, so a value the ``.inp`` rejects is a
value this API rejects.

Reference: ``openswmm_heat.h``.

.. warning::

   **Out-of-range values are REFUSED, not clamped**, and a refused write
   does not take effect. Read back after a write only if you want to
   confirm; do not assume a clamp happened.

----

Quickstart
==========

.. code-block:: python

    from openswmm.engine import (
        Solver, HeatFluxModule, HeatShortwaveMode,
        HeatRadiativeParam, HeatSolarParam, HeatCloudParam, HeatSourceKind,
    )

    with Solver("model.inp") as s:
        print(s.heat.enabled)                       # [OPTIONS] HEAT_TRANSPORT

        s.heat.modules[HeatFluxModule.RADIATIVE_EXCHANGE] = True
        s.heat.radiative[HeatRadiativeParam.ALBEDO] = 0.06

        # COMPUTED needs an explicit site first — gate on solar_sited.
        s.heat.solar[HeatSolarParam.LATITUDE] = 40.76
        s.heat.solar[HeatSolarParam.LONGITUDE] = -111.89
        assert s.heat.solar_sited
        s.heat.shortwave_mode = HeatShortwaveMode.COMPUTED

        s.heat.cloud[HeatCloudParam.FRACTION] = 0.4   # fraction, not percent

        s.heat.sources[HeatSourceKind.DWF] = 18.0     # degC
        s.heat.node_overrides.set(HeatSourceKind.DWF, "J1", 21.5)

        s.run()
        print(s.heat.current_shortwave, s.heat.cloud.current)

.. note::

   Edits are **live**: the flux modules re-read the configuration every
   step, so a mid-run write takes effect on the next routing step.

----

Flux modules
============

:attr:`Heat.modules` is a keyed, iterable mapping of
:class:`HeatFluxModule` to ``bool``; the three modules —
``SURFACE_EXCHANGE`` (latent + sensible), ``RADIATIVE_EXCHANGE``
(shortwave + longwave) and ``LAYER_CONDUCTION`` (LID vertical
conduction) — toggle independently.

----

Radiative parameters and the shortwave mode
===========================================

:attr:`Heat.radiative` maps :class:`HeatRadiativeParam` to ``float``.
Every entry except ``SHORTWAVE`` is a fraction restricted to ``[0, 1]``.

.. list-table::
   :header-rows: 1
   :widths: 44 56

   * - Parameter
     - Units / range
   * - ``SHORTWAVE``
     - Incoming shortwave, W/m². Non-negative.
   * - ``ALBEDO``
     - **Water** reflectance R\ :sub:`s`.
   * - ``SHADE_FACTOR`` / ``SKY_VIEW`` / ``EMISS_WATER`` /
       ``EMISS_LANDCOVER`` / ``ATM_EMISS_COEFF`` / ``LW_REFLECTION``
     - Shading, sky view, the two emissivities, the Brunt coefficient
       and longwave reflection.

:attr:`Heat.shortwave_mode` selects one of three
:class:`HeatShortwaveMode` spellings, which are **mutually exclusive in
effect** — exactly one is read, and there is no precedence ladder.

* ``CONSTANT`` reads ``HeatRadiativeParam.SHORTWAVE``.
* ``TIMESERIES`` reads the record bound by
  :meth:`Heat.set_shortwave_timeseries` (which also switches the mode).
* ``COMPUTED`` runs a Spencer/NOAA solar position through a Bird
  clear-sky model, using ``[SOLAR_RADIATION]``.

Two refusals matter here:

* Writing ``HeatRadiativeParam.SHORTWAVE`` while the mode is **not**
  ``CONSTANT`` is refused. A constant is not read in the other two modes,
  so storing one there would look configured while changing nothing.
  Switch the mode first.
* Selecting ``HeatShortwaveMode.COMPUTED`` is refused unless **both**
  latitude and longitude have been set explicitly. The engine will not
  borrow the ``[TEMPERATURE]`` snowmelt latitude, which defaults to 0 and
  would silently model equatorial noon.

**Switching modes does not erase the other modes' stored settings.** A
constant stays stored while a timeseries is active, and vice versa, so an
editor can offer three radio buttons without destroying what the user
typed under the other two.

:attr:`Heat.current_shortwave` is the resolved incoming shortwave at the
current step in W/m², cloud already applied. It is **read-only state, not
configuration**: it is ``0.0`` before the first step and whenever
radiative exchange is off.

----

Solar siting
============

:attr:`Heat.solar` maps :class:`HeatSolarParam` to ``float`` and is
consulted under ``COMPUTED`` only. ``LATITUDE`` is degrees ``[-90, 90]``,
``LONGITUDE`` degrees ``[-180, 180]``, ``ELEVATION`` metres
``[-500, 9000]`` (below sea level is legal), plus the Bird atmosphere
terms ``TURBIDITY_380``, ``TURBIDITY_500``, ``PRECIP_WATER`` and
``OZONE``.

.. warning::

   ``HeatSolarParam.GROUND_ALBEDO`` is the **land** albedo used by the
   Bird model. It is *not* the water reflectance — that is
   ``HeatRadiativeParam.ALBEDO``. They are separate values with separate
   meanings, and writing one does not change the other.

:attr:`Heat.solar_sited` is ``True`` once latitude **and** longitude have
both been written explicitly. Gate a ``COMPUTED`` control on this rather
than discovering the refusal after the fact.

----

Cloud cover
===========

:attr:`Heat.cloud` maps :class:`HeatCloudParam` to ``float``. Writing any
of them marks cloud cover configured, which ``cloud.configured``
reports. ``FRACTION`` is a fraction in ``[0, 1]`` — not a percent — and
the Kasten–Czeplak / Bolz coefficients must be non-negative.

.. code-block:: python

    s.heat.cloud.set_timeseries("CLOUD_OBS")   # bind a [TIMESERIES]
    print(s.heat.cloud.configured, s.heat.cloud.current)
    s.heat.cloud.clear()                       # back to clear sky

``cloud.clear()`` restores the exact clear-sky longwave path, not an
approximation of it. ``cloud.current`` is the cloud fraction in effect at
the current step — read-only state, zero before the first step.

----

Heat sources and node overrides
===============================

:attr:`Heat.sources` maps :class:`HeatSourceKind` to a **global** inlet
temperature in degC. The table is a fixed enum extent of seven entries,
so ``len(s.heat.sources)`` never depends on what the model configured.

.. code-block:: python

    s.heat.sources[HeatSourceKind.RDII] = 12.0
    s.heat.sources.is_configured(HeatSourceKind.RDII)   # True
    s.heat.sources.clear(HeatSourceKind.RDII)           # back to the 20 degC default

    # Resolution the engine itself uses: override if present, else global.
    s.heat.sources.effective(HeatSourceKind.DWF, "J1")

Temperatures outside ``[-50, 100]`` degC are refused — the parser's own
range. An unconfigured source reads the 20 degC default;
``sources.is_configured()`` is what tells the two apart, and
``sources.clear()`` returns a source to the default so the writer emits
no row for it (node overrides are left alone).

:attr:`Heat.node_overrides` is a row-indexed sequence of
:class:`HeatNodeOverride` named tuples:

.. code-block:: python

    s.heat.node_overrides.set(HeatSourceKind.EXTERNAL_INFLOW, "J1", 25.0)
    for row in s.heat.node_overrides:
        print(row.source, row.node_index, row.temp_c)
    s.heat.node_overrides.remove(0)

.. warning::

   Only ``HeatSourceKind.DWF`` and ``HeatSourceKind.EXTERNAL_INFLOW``
   accept node scope. Any other source is **refused**, not silently
   deferred to the global value.

Setting the same ``(source, node)`` pair twice is an **update**, not a
duplicate row. ``node_overrides.remove()`` shifts later rows down,
so re-read ``len()`` when iterating by index.

----

See also
========

* :doc:`water_age` — the same source/override table shape, in hours.
* :doc:`initial_quality` — seeding ``__TEMPERATURE__`` per element.
* :doc:`climate` — the temperature and wind the surface-exchange module
  consumes.
* :doc:`process_components` — where ``model.heat``'s config path lives.
* :doc:`error_handling` — what a refused write raises.
