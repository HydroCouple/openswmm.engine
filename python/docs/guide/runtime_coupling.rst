===================
Runtime 2D coupling
===================

.. currentmodule:: openswmm.engine

The CPU LOCAL_INERTIAL and DIFFUSIVE_WAVE surface marchers and saturated
groundwater solver accept external water,
transported temperature, and species inputs **after start**, through the C API
and the Python bindings. Sources can be replaced, cleared, or given an expiry
time. Hydraulic perimeter boundaries accept incoming quality and temperature.
``Solver.advance_to()`` stops at an elapsed-second exchange time and settles
pending surface and groundwater work before returning.

Use ``solver.coupling.capabilities`` to inspect the active model. Transport rows
must be enabled before initialization. Runtime inputs do not create species,
change the mesh, or enable disabled physics. Groundwater heat here is sensible
heat in water; soil thermal storage/conduction and thermal Dirichlet/Neumann
conditions are future solver work. Groundwater sources address the saturated
zone; layer targeting and top recharge are not exposed by this interface.
FULL_SWE runtime coupling is explicitly unavailable in capabilities and setters.

Direct sources
==============

``Solver``'s context manager opens, initializes and starts the model. Configure
an aquifer and transport options using the explicit ``open`` / ``initialize`` /
``start`` lifecycle when the input file does not already contain them.

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("coupled.inp") as solver:
        solver.advance_to(2.375)
        sw = solver.surface2d
        gw = solver.groundwater2d  # same object as sw.groundwater

        sw.set_water_source(
            0, 0.01, source_id="surface_inlet",
            concentrations={"TSS": 5, "__TEMPERATURE__": 12},
        )
        gw.set_water_source(
            0, -0.001, source_id="well",
        )
        sw.set_heat_source(0, 100, source_id="heater")
        gw.set_species_source(0, {"TSS": 0.003}, source_id="loading")

        solver.advance_to(12.375)
        receipt = sw.source_receipt("surface_inlet", cell=0)
        print(receipt.applied_m3, receipt.applied_species)

        sw.set_water_source(
            0, 0.02, source_id="surface_inlet",
            concentrations={"TSS": 8, "__TEMPERATURE__": 15},
        )
        sw.clear_source("heater")
        solver.advance_to(22.375)

A prescription takes effect at the current model time and holds until replaced,
cleared, or expired. Setters first settle work belonging to the old prescription.
Using the same source ID and cell replaces the entire tuple. Different IDs add.
Omitted concentrations and rates are zero. A groundwater ID matching an authored
well overrides that well in the selected cell; clear/expiry restores it.

Cell source flow is total m³/s **per cell**, positive into the domain. Negative
flow extracts donor quality, limited by available water. Heat is signed W;
negative heat cools. Independent species rates are signed concentration-unit·m³/s;
negative rates remove available species mass. Reserved age/temperature rows
cannot use independent species rates: supply incoming age/temperature with water,
and use ``heat_w`` for independent energy.

For a pollutant in mg/L, a numeric species rate of 0.003 concentration-unit·m³/s
is 3 mg/s. An inflow of 0.01 m³/s at 5 mg/L imports 50 mg/s. This explicit
conversion also applies to native mass receipts; the API does not label those
receipts as kilograms. Temperature is °C, age is seconds, and incoming quality
follows each domain's ``species`` order and concentration getter units.

Scalar and bulk sources
-----------------------

Cell arrays and rate arrays have equal length. A scalar broadcasts to every
selected cell; it is not divided among cells.

.. code-block:: python

    sw.set_water_source(
        [0, 1], [0.01, 0.02], source_id="provider",
        concentrations={"TSS": [5, 8]}, until_seconds=30.125,
    )

For one atomic update across both domains, use ``RuntimeSource`` records. An
invalid batch leaves the clock and all existing prescriptions unchanged.

.. code-block:: python

    from openswmm.engine import RuntimeSource

    solver.coupling.set_sources([
        RuntimeSource("river", "surface", 0, 0.01, {"TSS": 5}),
        RuntimeSource("well", "groundwater", 0, -0.001),
    ])

Atomic frames, providers and forcing
====================================

``CouplingFrame`` combines sources, hydraulic boundaries, rainfall/evaporation,
paired infiltration and source clears into one validated update. Invalid records
or ownership conflicts leave all prescriptions and the clock unchanged.
Boundary ``kind`` is 0 clear, 1 head, 2 total outward flow. Forcing ``cell=-1``
is uniform; rates are m/s. ``mode`` is ``replace``, ``add`` (rain/evap), or
``clear``. A clear restores the underlying native input.

.. code-block:: python

    from openswmm.engine import (
        CouplingFrame, RuntimeSource, RuntimeBoundary, RuntimeForcing, RuntimeClear,
    )

    provider = solver.coupling.for_provider("river")
    provider.apply_frame(CouplingFrame(
        sources=(RuntimeSource("exchange", "groundwater", 0, 0.001),),
        boundaries=(RuntimeBoundary("surface", 0, 0, 2, -0.01, {"TSS": 5}),),
        forcings=(RuntimeForcing("rainfall", rate_m_s=0.00001,
                                concentrations={"TSS": 2, "__TEMPERATURE__": 12}),),
    ))
    provider.groundwater.clear_source("exchange", cell=0)
    receipts = provider.groundwater.source_receipts([0], "exchange")

Each named provider has its own source namespace. Equal source IDs from different
providers add without collision. Boundaries and forcing channels are exclusive;
other providers and legacy setters cannot overwrite them until clear/expiry.
Provider names cannot contain a slash or NUL. The default unowned view preserves
the authored-well name override behavior. Named providers have independent wells.
``surface`` and ``groundwater`` on the provider expose the same bulk source,
boundary, forcing and receipt operations. Clearing a source with ``cell=None``
clears the whole ID; specifying a cell clears only that prescription.

Rainfall may include a full incoming quality/temperature/age tuple. Omitted
``concentrations=None`` preserves native rainfall quality. A ``rain_quality``
forcing updates that tuple without changing rainfall rate. Rain quality and rates
expire at their elapsed-second endpoints. Evaporation removes water, advected
age/temperature, and retains ordinary dissolved species.

``RuntimeForcing("infiltration", cell=0, rate_m_s=...)`` replaces the native
infiltration demand at that cell. It requires an active aquifer and matching
surface/GW transport rows. The solver limits transfer by surface water and GW
headroom, removes donor quality once from the surface and books the same water
and species once into groundwater. It bypasses native soil infiltration-capacity
gating for this external prescription. ``provider.surface.infiltration_receipt(0)``
reports its requested/applied/rejected transfer, positive surface-to-groundwater.
This is a paired transfer, not an additional independent recharge source.
Boundary and paired-infiltration receipt limiter reasons report water or receiver
capacity; they do not mislabel rejected advective heat as independent dry-cell heat.
Source IDs and receipt histories live until the engine is reset/closed. Reuse
stable IDs to keep long-run history bounded.

Receipts and limiting
=====================

Receipts are cumulative for an ID/cell and survive replacement, clear and expiry.
They contain requested/applied/rejected water, independent heat, and species.
Signs are positive into the target domain. ``rejected_species`` and
``limiter_reasons`` identify unfulfilled withdrawals or dry-cell heat.
``last_applied_m3_s`` and ``last_interval_seconds`` describe the owning cell's
last source firing. Use differences of cumulative receipts to account for a
whole exchange window, especially with local time stepping.

``applied_species["__TEMPERATURE__"] * 4186000`` is the signed sensible-energy
transfer in J, including advective heat and independent supplied heat.
``applied_heat_j`` records only the independent ``heat_w`` part; adding these two
quantities would count independent heat twice.

Dry cells reject independent heat when no water can receive it. Ordinary species
loadings remain stored through drying and rewetting. Surface concentration
getters report zero below the configured dry-depth threshold; a zero getter
value on a dry cell does not prove that stored species mass has disappeared.
Groundwater accepts an external inflow before any saturation excess transfers to
the surface. Whole-domain conservation therefore includes that internal transfer.

Hydraulic boundaries
====================

Both views provide ``set_head_boundary(cell, edge, head_m, concentrations=None)``
and ``set_flow_boundary(cell, edge, flow_m3_s, concentrations=None)``. Only
perimeter edges are legal, with local edge indices appropriate to triangles or
quads. Head is metres on the mesh datum. Boundary flow is total m³/s per edge,
**positive outward**, so a negative value imports water.

.. code-block:: python

    sw.set_flow_boundary(0, 0, -0.01, {"TSS": 5, "__TEMPERATURE__": 12})
    gw.set_head_boundary(0, 0, -0.25, {"TSS": 5})
    solver.advance_to(40.125)
    print(sw.boundary_flow(0, 0), gw.boundary_flow(0, 0))
    print(gw.boundary_receipt(0, 0))
    sw.clear_boundary(0, 0)
    gw.clear_boundary(0, 0)

Surface boundary edits rebuild the active boundary index. Direct values own
the channel and override authored time series/rating curves. Clearing restores
the original hydraulic and incoming-quality prescription, including its time
series. The legacy ``set_edge_bc_flow`` keeps its m³/s/m convention; the new
``set_flow_boundary`` uses total m³/s. ``get_edge_flux_bulk`` already returns
total outward flow and must not be multiplied by edge length again.

Groundwater head boundaries use a one-sided unconfined Darcy face, with the
cell-to-edge normal distance and arithmetic saturated transmissivity. A specified
flow shares the same donor-water limiter as wells. Outgoing quality uses the
cell's dissolved concentration; incoming water uses the supplied tuple. Clear
restores the native lateral no-flow condition. ``boundary_flow`` reports the
last applied outward flow on a live boundary, or zero after clear.
``boundary_receipt`` retains history and uses the source convention
(positive inward). Boundary IDs are ``boundary/<cell>/<edge>`` and are reserved
for that purpose. Both domains expose ``boundary_receipt`` returning ``BoundaryReceipt``; its heat fields
contain total advective J (unlike independent source heat). Surface water ledgers
record gross boundary inflow and outflow separately, even in the same window.
``sw.species_ledger("TSS")`` exposes storage and cumulative external, boundary,
rainfall, infiltration, native coupling and exfiltration mass terms.

This boundary contract supplies advective quality. It does not currently apply
solute diffusion from a prescribed concentration or soil heat conduction across
an edge.

Clock and lifecycle
===================

``advance_to(seconds)`` accepts a finite target between the current elapsed time
and the configured end time. It never overshoots the requested exchange endpoint.
Sources expire at their specified elapsed time even with ordinary ``step()``,
``stride()`` or callback-driven runs. Supported setters may run between steps
or in step callbacks. Recursive stepping/advancing from a callback is rejected.
``solver.elapsed`` reads the live clock inside callbacks. Step-begin callbacks
receive the tentative step size; a source installed there can shorten the step
at its expiry.

Python views retain their owner and become stale after lifecycle/structural
changes. Another thread cannot access an engine while it is executing a native
call. Raw C callers must serialize access to each engine and use the controlling
thread for callbacks.

The current interface promises forward explicit coupling. It does not promise
rollback or restart of runtime prescriptions/receipts. Native hotstart saving
rejects a run that has used the new runtime coupling service, so it cannot
silently save an incomplete restart. The standalone HydroCouple component and
Composer integration remain separate deliverables.

C API
=====

``openswmm/engine/openswmm_coupling.h`` publishes the copied source tuple,
atomic bulk and scalar setters, receipt queries, capabilities, and runtime
surface/GW boundary functions. ``openswmm_engine.h`` provides
``swmm_engine_advance_to`` and ``swmm_engine_get_elapsed_seconds`` independently
of the optional 2D build. ``swmm_coupling_advance_to`` is the 2D service alias.
