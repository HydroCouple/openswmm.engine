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
:class:`HeatFluxModule` to ``bool``; the four modules —
``SURFACE_EXCHANGE`` (latent + sensible), ``RADIATIVE_EXCHANGE``
(shortwave + longwave), ``LAYER_CONDUCTION`` (LID vertical conduction),
and ``SEDIMENT_EXCHANGE`` (channel bed and hyporheic exchange) — toggle
independently.

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

Timeseries names
================

``heat.shortwave_timeseries`` and ``heat.cloud.timeseries`` return the assigned
series name, or an empty string when no series is bound. Use the corresponding
``set_shortwave_timeseries(name)`` and ``cloud.set_timeseries(name)`` methods
to assign an existing series. Shortwave values are W/m²; cloud values are
fractions in [0, 1].


Pre-run authoring and transport previews
========================================

``heat.dry_policy`` uses ``0`` (HOLD), ``1`` (AIR), or ``2`` (DEFAULT).
``heat.lid_material`` returns read-only water/soil conductivity in W/m/K,
soil density in kg/m³, and soil specific heat in J/kg/K.
``heat.authoring_flags`` records configured heat (bit 1), a shared climate
site (bit 2), and explicit latitude/longitude/timezone/elevation (bits
4/8/16/32). ``heat.solar.clear()`` clears that site before initialization;
COMPUTED shortwave must first be changed to another mode.

``heat.sediment.is_configured(param)`` and ``heat.sediment.clear(param)``
apply to ``GROUND_TEMPERATURE`` and ``INITIAL_TEMPERATURE``. Clearing ground
temperature also clears its timeseries and restores inheritance.
``heat.effective_element(attr, scope, name)`` resolves NODE or LINK
configuration precedence independently of whether the process is active.
Its ``HeatElementValue`` contains ``value``, ``source``, and ``source_name``.
Source codes are GLOBAL=0, TAG=1, NODE=2, LINK=3, air temperature=4, and ground
series=5. Dynamic temperatures return NaN with their source name.

``heat.snapshot()`` captures heat configuration, its shared site, and
component registrations before initialization. The snapshot owns its native
storage and retains the solver. Use it as a context manager or call
``close()``; releasing it is safe even after destroying the solver.
Call ``restore()`` explicitly to undo scalar heat changes. Closing the solver,
reopening it, or making structural edits invalidates the snapshot; capture a
new one after those changes. Restoring also invalidates other structural
views. A snapshot does not permit restoring after initialization.

``heat.edit_atomic(callback)`` calls ``callback(solver)`` before initialization
and returns the callback's result. If the callback raises, native state is
rolled back before the original exception is re-raised. The callback may edit
only climate, snow and heat configuration, ``HEAT_TRANSPORT``, component
registrations, and climate-file paths. Topology edits, lifecycle calls and
nested transactions are forbidden. Reacquire structural views after editing.

.. code-block:: python

    s = Solver("model.inp")
    s.open()  # Authoring only: do not initialize yet.
    try:
        with s.heat.snapshot() as saved:
            s.heat.dry_policy = 1
            saved.restore()

        def configure_heat(model):
            model.heat.radiative[HeatRadiativeParam.ALBEDO] = 0.08
            model.heat.dry_policy = 1

        s.heat.edit_atomic(configure_heat)
        planned = s.transport.preview_matrix(
            ignore_quality=False, heat=True, age=False, ignore_2d=False,
            surface_classes=(True, True, True, True), groundwater=-1,
        )
    finally:
        s.destroy()

``transport.preview_matrix`` returns the same domain/class mapping as
``solver.transport_matrix`` without changing live options. Its required
Boolean fields and the four ``surface_classes`` entries accept only 0/1;
classes are ordered POLLUTANTS, MSX, AGE, TEMPERATURE. ``groundwater`` is
-1 (automatic), 0 (off), or 1 (on). This is a plan for transport, rather than
proof of initialized runtime activity or local heat-flux support.

Complete snapshots and mapping previews
=======================================

``heat.snapshot(complete=True)`` additionally captures climate, snow,
hydrothermal options and 2D/ground thermal authoring configuration. Its
ownership, explicit ``restore()``, lifecycle and structural invalidation rules
match the default heat snapshot described above.

``heat.preview_channel_ground_map(link, first=None, limit=256, progress=None)``
returns pre-run XY coverage for a link ID or index. Omit ``first`` to return
all contacts, or provide a zero-based offset and a page limit (zero is allowed
for a summary query). ``required`` and summary counts describe the complete
mapping; ``contacts`` contains the requested rows. Rows have ``layer=-1``.
``valid`` means complete XY coverage; soil ownership, invert and thermal
configuration still require validation during initialization.

An optional ``progress(stage, percent)`` callback runs synchronously with
stages 1 (mesh) and 2 (mapping), and percentages in 0..100. Return true to cancel;
this raises an ``EngineError`` with ``ErrorCode.CANCELLED``. Callback exceptions
are re-raised unchanged. Callbacks must not mutate or close the engine.

For initialized 2D thermal arrays, ``Heat2DValue`` names the field codes accepted
by ``heat.mesh_values``. With 2D support, ``surface2d.initial_bed_quality`` and
``set_initial_bed_quality(species, conc, tag=None, cell=None)`` expose authored
bed-porewater concentrations using the same scope and species units as surface
initial quality. ``remove_initial_bed_quality(index)`` removes one row.
``surface2d.set_auto_backend_cpu(enabled=True)`` sets a pre-run session policy
for unoverridden AUTO; explicit backend/environment choices take precedence.
The policy is not serialized and must be reapplied after opening a model.

``tables.rename(key, new_id)`` preserves table references and enforces
case-insensitive uniqueness within each curve/timeseries namespace.
``tables.set_type(key, TableType.CURVE_RATING)`` changes a curve's type while
preserving points and references; conversions to or from TIMESERIES are
refused. Both edits require an authoring state and advance solver generation.
