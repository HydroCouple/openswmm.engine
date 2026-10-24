@page lid_node_legacy_hydrology Storage-node LID Legacy Hydrology Adaptation

Implemented October 5, 2026. This note describes the runtime implementation;
older redesign proposals are not the authoritative formulation. Chapter 6
of the engine manual describes input syntax and practical limits. GUI T10
contains the control examples and resaturation animation.

## Purpose and scope

Reuse legacy SWMM's exponential soil conductivity and modified Green–Ampt
surface infiltration while retaining storage-node connectivity, signed ports,
Dynamic Wave backwater, arbitrary ordered layers and conservative quality
transport. Conventional subcatchment LIDs and the shared infiltration routine
are unchanged. Node MEDIA's existing sixth physical parameter was already
named conductivity slope in the schema/API/GUI; its former runtime use as a
power exponent was incorrect. No new input field or GUI toggle is required.

This is not a Richards solver. It omits intercell matric-pressure gradients,
upward capillary redistribution, retention hysteresis and independent
saturated layer reactors. Multiple MEDIA layers use donor gravity drainage;
one Green–Ampt history belongs to MEDIA directly beneath SURFACE.

## Fluxes and accepted transfers

For MEDIA above field capacity:

```text
q = Ks * exp(-conductivity_slope * (porosity - theta))
Q = donor_area * q
```

Otherwise q = 0. AGGREGATE uses q = Ks, field capacity zero. Slope >= 0
is valid; slope zero gives constant Ks. Every explicit substep is at most
one second. All cells compute candidate fluxes from the same pre-update
state. The accepted transfer is bounded by the potential volume, donor
water above field capacity and receiving pore capacity. A receiver whose
bottom is below the mobile table routes to mobile storage; that transfer
has no retained receiver-capacity bound. Donors below the mobile table skip
free drainage. Debits and credits use exactly the same accepted volume and
are replayed with pollutant mass by LidNodeTreatment.

Surface-to-media potential infiltration calls the existing
`infil::grnampt_getInfil` with MOD_GREEN_AMPT, zero additional rainfall,
actual ponded depth and the substep. Rain/runon was already captured into
surface storage; supplying it again would double-count it. Ponded depth is
`theta_surface / surface_porosity * surface_thickness`, accounting for
vegetation displacement. The call receives the climate infiltration and
recovery factors. Zero conductivity or a zero conductivity multiplier
produces no entry, avoiding the legacy routine's zero-Lu divisions.

The history is cloned for each potential calculation. Timer and saturation
transitions are retained, but F and Fu advance only by
`accepted_volume / surface_area`, after water/capacity bounds. Rejected
potential infiltration does not advance either inventory. F is an active
front-history depth, not a lifetime cumulative flow statistic. The native
routine solves integrated Green–Ampt and supplies the F = 0 transition.
An empty surface switches to the dry branch if backwater does not intersect
the upper zone. Recovery's Fu is floored by actual cell wetness; IMD cannot
exceed actual deficit and recovered F is nonnegative. Suppressing history
recovery does not remove water: physical evaporation/drainage are accounted
separately. Surface-to-aggregate uses the aggregate conductivity directly.

## Finite upper zone and hydraulic wetting

Let Lu be the shared Green–Ampt empirical upper-zone depth, limited to the
first authored MEDIA thickness. Its empirical base value in feet is
`4 * sqrt(Ks_in_inches_per_hour) / 12`. IMDmax = porosity - wilting_point;
Fumax = IMDmax * Lu. Initialization derives IMD and Fu from the current
profile rather than starting a nominally dry history in wet media.

The physical upper-zone deficit is an area-weighted volume average over
`[media_top - Lu, media_top]`. For each intersecting numerical cell:

```text
zone_geometry += area * intersection_thickness
empty_pores += area * thickness_above_mobile_table * (porosity - theta)
deficit = empty_pores / zone_geometry
wetness_depth = clamp((IMDmax - deficit) * Lu, 0, Fumax)
```

The submerged fraction has physical theta = porosity, even though its
retained/mobile split reports retained theta separately. Partial cells are
included geometrically; no additional water is invented by reconciliation.

`reconcileInfiltration` runs before physical preparation and after final
accepted port transfers:

1. Rising head or accepted positive receipt into the first authored MEDIA
   lowers IMD to at most the physical deficit and raises Fu to at least
   physical wetness. Reverse/port water never increments F.
2. Full submergence of the media top sets IMD = 0, Fu = Fumax, F = 0 and
   saturated = true. Interface checks allow 32 machine epsilons times a
   local elevation scale, including a depth one ULP below the exact top.
3. Recession from full submergence sets IMD = physical deficit,
   Fu = physical wetness, F = 0 and saturated = false. The next event starts
   an approximate downward front in remaining wet media. Continue
   reconstructing while head falls and F remains zero, until accepted surface
   infiltration starts the new front; the first exposed slice alone would
   leave an artificially small deficit.
4. An empty surface with the upper zone intersected by backwater does not
   invoke dry history recovery. Partial backwater wetting retains an
   approximate front; interacting upward/downward fronts are not resolved.

Network water enters at the actual hydraulic port. Media port receipts fill
local retained capacity before excess joins mobile water. Fully submerged
cells are returned to field-capacity retained moisture, using explicit
retained/mobile transfers. Their combined volume remains unchanged.
`resetPorts` and provisional hydraulic/quality iterations do not advance
infiltration state. Final accepted transfers are applied exactly once.

## Native hotstart V10

Per-node LID restart records add a count followed by either zero doubles
(no active surface-media history) or eleven doubles:

```text
S, Ks, IMDmax, IMD, F, Fu, Fumax, Lu, T, last_reconciled_head, saturated(0|1)
```

The existing CRC covers these fields. Reading validates finite values,
nonnegative inventory bounds, IMD <= IMDmax and Fu <= Fumax. T may be
negative. Applying checks authored/profile geometry and S, Ks, IMDmax,
Fumax and Lu against the initialized model before applying node state.
V10 saves retained masses and moisture as before; ordinary non-LID file
versions are unchanged. Compatible V8/V9 LID files lack the new history,
so it is reconstructed from restored moisture/depth, with an explicit
warning that exact continuation is unavailable. These older files are not
silently described as exact restarts. Configuration INP/GeoPackage stores
parameters, not evolving runtime history.

## Verification and reproduction

Build the nine engine test targets and run:

```text
ctest --test-dir build/darwin -R '^test_engine_(lid_nodes|quality_routing|treatment|hotstart|outfall_backflow|water_age_lid|heat_lid|infiltration|lid)$' --output-on-failure
```

241 tests pass. Twelve added LID tests cover analytical conductivity,
Green–Ampt equivalence/ponding response, rejected-volume history, Ks/multiplier
zero, dry recovery, retained-port wetting, partial/full resaturation,
recession and second storms, provisional trials, exact V10 continuation and
V9 reconstruction. Conventional LID/infiltration suites guard shared paths.
The routed resaturation fixture checks water closure within 0.005 ft³ and
per-step conservative-tracer closure within 0.1% of incoming mass, including
zero-quality reverse water. The full-chain quality fixtures test ZERO/LAST
boundary quality. No numerical-warning suppression or continuity balancing
correction was introduced.

The test `LidNodes.RoutedReversalResaturationRecessionAndSecondEventConserve`
reads `tests/unit/engine/data/lid_node_resaturation.inp` and writes a one-second
state CSV in `tests/output/lid_nodes_2026_10_04/`. GUI article scripts render
that data and rerun all six 24-hour examples at 0.1, 0.05 and 0.025 seconds.
Their validation document records binary hashes and actual performance
sensitivity. Good continuity alone does not establish converged timing,
field validity or improved treatment: the synthetic constituent's 2/day
first-order decay necessarily gives greater reaction with longer exposure
and has an 8.3-hour half-life.

## Primary formulation references

- EPA SWMM `lidproc.c`: https://github.com/USEPA/Stormwater-Management-Model/blob/develop/src/solver/lidproc.c
- EPA SWMM `infil.c`: https://github.com/USEPA/Stormwater-Management-Model/blob/develop/src/solver/infil.c
- Tu, Wadzuk and Traver (2020), connected blocks with gravity and matric
  diffusivity, van Genuchten functions and texture emulators:
  https://doi.org/10.1371/journal.pone.0235528

The legacy relationships reduce calibration demands but do not establish
superiority over a validated matric-gradient model. Additional retention
curves, interface-head treatment and validation are needed if capillary
barriers, root-zone dynamics or upward supply determine the study outcome.
