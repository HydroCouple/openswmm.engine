# Network and surface water phase contract

The component type `org.hydrocouple.openswmm.engine` accepts the roles
`split_network` and `integrated2d`. Create one instance of each, with distinct
instance IDs, the same fixed INP and separate fresh output directories. Connect
the surface output `openswmm.node-surface-water.v1` to the network input with
that same ID. The surface writes `model.2d.h5` by default. Its optional Boolean
configuration argument `surface_results` disables that writer when false.
The network writes `model.out`. Both participants write their own report.

The time-stepped Composer workflow supports this direct connection. The
network owns routing time and synchronously calls the provider's
`updateValues(query)` during hydraulic iterations and at native routing
boundaries. The surface reads the request from that input and publishes its
receipt on the output. The network reads the receipt before resuming the
solver. Ordinary workflow updates of the surface only publish lifecycle
status. A component host can prepare the pair in either order. The network
takes geometry and starts on its first update after both participants prepare.

A runnable [split-water composition](../examples/hydrocouple/split-water/composition.json)
and its adjacent INP use relative paths. Open a fresh copy in Composer and
include the engine build's `components` directory in the library search path.
The example drains and spills during a 60-second simulation.

A composition containing multiple independent pairs can use Composer's shared
recorder when all pairs reach the same physical time at each snapshot. Pairs
with different calendars can run together with their own native result files.
Composer refuses a shared snapshot when those clocks diverge.

## Exchange representation

The exchange item has HydroCouple shape [1] and data kind String. Its UTF-8
JSON value is a compound transaction with the fields below. The quantities
carry different units and have different physical meanings, so the enclosing
item has unknown ValueKind. Composer can report this as a connection
diagnostic. Read individual quantities according to this schema. The contract
requires one direct consumer and preserves node order. Scalar quantity
adapters or interpolation cannot service this transaction.

Every packet carries these fields.

| Field | Type and meaning |
| --- | --- |
| `schema` | `openswmm.node-surface-water/1` for node-only coupling, `openswmm.node-surface-water/2` when outfalls are mapped, `/3` for ordinary watershed/LID outlets, `/4` for the qualified mesh groundwater owner, `/5` for reviewed source pools, `/6` for the reviewed joint Richards profile, `/7` for reviewed models with outfall return destinations, or `/8` when classical NODE LIDs are present |
| `kind` | `geometry`, `request` or `receipt` |
| `phase` | 0 geometry, 1 conductance, 2 advance, 3 finish, 4 outfall preparation, 5 source preparation, 6 LID trial, 7 LID acceptance. Only configured owners permit their preparation phases. |
| `sender`, `target` | Unsigned 64-bit integer tokens issued to native participants in this run. Sender is positive. Geometry targets 0. Requests target the surface. Receipts reverse the pair. |
| `sequence` | Unsigned 64-bit integer. Geometry uses 0. Each subsequent request increments the network sequence, including successive conductance queries at one clock time. A receipt echoes the request sequence. |
| `start_s`, `duration_s` | Seconds relative to the model start. The epoch is the configured `epoch_julian_day`. Conductance, advance and outfall preparation use the routing duration. Geometry and finish use duration 0. Receipts echo the original start and duration. |
| `node_count` | Positive integer. Nodal columns have exactly this many entries in negotiated order. |

Finite physical numbers use binary64 precision. Preserve integer tokens as
integers and round-trip physical numbers at binary64 precision. Hydraulic
columns use SWMM's internal feet so transfer introduces no additional SI
conversion. One foot is exactly 0.3048 metres. The water balance uses m³.
Additional descriptive fields can be ignored. A different schema version
requires a separate implementation.

**Geometry.** The prepared provider publishes the exact ordered array of
`node_ids`, a 0/1 `coupled` flag per node, `ponded_area_ft2`, `pond_area_ft2`
and `pond_band_ft`. The last two columns can be empty under native footprint
defaults. Areas and bands are nonnegative. The `configuration` string is the
canonical UTF-8 INP path, a newline and the original INP bytes as UTF-8 text.
Network import verifies the configuration and node IDs before starting. This
handshake therefore transfers the model text once to the connected participant.
Subsequent transactions use the negotiated participant tokens and row order.

**Requests.** Each query carries nodal `head_ft` (datum elevation),
`depth_ft`, `volume_ft3`, `rim_volume_ft3` (the native full volume used to
limit withdrawals), `queued_ft3` and `direct_ft3`. The scalar `remaining_s`
is the remaining uniform queue delivery span. Positive pending water credits
the network. Negative pending water debits it. The Boolean `flush_on_report`
records the network's binary output policy, which also determines when the
native surface must flush pending batches.

**Conductance receipts.** `conductance_ft2_per_s` is an ordered array of
[zero-based node index, nonnegative conductance] pairs. The units are ft³/s per
foot of head. Preserve repeated indices and row order when accumulating in the
hydraulic iteration. The surface clock stays fixed during this query.

**Advance and finish receipts.** `queued_ft3`, `direct_ft3` and `remaining_s`
replace the network's pending state. Adding these amounts a second time would
double count exchange. The surface owns its cumulative water ledger. The
receipt's `balance` object contains the following named fields.

| Fields | Meaning |
| --- | --- |
| `init_storage`, `final_storage` | Initial and current surface water storage, m³ |
| `external_in`, `external_out`, `rainfall_in`, `boundary_in`, `boundary_out`, `evap_out`, `infil_out` | Cumulative water, m³, positive in the named direction |
| `coupling_1d_to_2d_in`, `coupling_2d_to_1d_out` | Gross cumulative network spill and surface drainage, m³ |
| `outfall_in`, `outfall_out`, `aquifer_in`, `infil_to_aquifer`, `infil_aquifer_pending` | Native water ledger slots, m³. Outfall slots are active when outfalls are mapped. Aquifer slots are active under versions 4–8 when groundwater is configured; reviewed footprints and joint Richards follow the bounded profiles below. |
| `active` | Boolean indicating native surface accounting |
| `solver_nsteps`, `solver_nrhs`, `solver_n_tiers` | Integer solver counters. A step count of −1 means unavailable. Tier count ranges from 0 to 8. |
| `solver_avg_h`, `solver_last_h` | Internal surface step durations, s |
| `solver_active_min`, `solver_active_mean`, `solver_active_max` | Active-cell fractions, with −1 for unavailable statistics |

The sibling array `tier_cells` contains eight integer cumulative cell counts.
The network's copy of this ledger is diagnostic. Physical water belongs to
its domain owner and to the outstanding delivery queue until consumed.

**Finalization.** FINISH settles a partial surface batch and returns the last
queue and ledger at the existing clock boundary. If the host finishes the
surface first, its output asks its connected network input to settle via one
String buffer write carrying schema `openswmm.node-surface-finish/1`, the
`network` and `surface` instance IDs, and `time_s`. The network accepts this
notification from a finishing peer at the same boundary and issues the usual
FINISH query. Both teardown orders settle the batch once. On failure, close
both participants. An already performed physical advance has no rollback.

The codec in `openswmm_split_packet.h` copies between native frames and JSON.
It is enabled in the component build with 2D support. Free encoded strings
with `swmm_split_packet_free` and decoded frames with
`swmm_split_frame_destroy`. A decoded receipt or geometry frame contains only
the columns published for that variant. The combined node-state inspection
accessor requires the head/depth and pending-water columns of a request.

The tested execution scope is one Composer process and the serial CPU,
first-order, water-only DYNWAVE footprint profile. Metadata preflight and
resolved geometry validation enforce the native profile. Ordinary watershed and conventional LID outlets are supported through native network routing under version 3. Version 4 supports the bounded mesh groundwater profile below. Version 5 adds reviewed R4 sources, version 6 reviewed joint Richards nodes, and version 8 classical NODE LIDs. Snow, RDII, transport and graph restart require further phase contracts and qualification.

## Version 2 mapped outfalls

The geometry packet selects version 2 when the resolved native surface owner
has mapped outfalls. Every subsequent packet uses that version and includes
`mapped_outfalls`, a nonempty ordered list of unique node indices. Each index
identifies an outfall in the negotiated `node_ids` array. The preparation,
conductance, advance and finish packets retain that exact list. Version 1
continues to serve node-only compositions.

The network issues phase 4 once per routing interval before the hydraulic
solve. The surface calls its existing native tailwater and reservation
operations, then returns `outfall_boundaries`. This array contains one
[head_ft, ramp, limit_cfs] tuple per entry of `mapped_outfalls`. Head is the
cached tailwater elevation in native feet. Ramp is the native wet/dry blend in
[0,1]. Limit is the finite nonnegative withdrawal rate in ft³/s allocated to
each incident link, including its aggregate barrel flow. Unmapped nodes retain
their native unlimited rate. These fields are installed before the network
rolls its hydraulic state. The native solver applies the tailwater and rate
limits in its ordinary iterations.

Version 2 requests also carry `node_inflow_cfs` and `node_outflow_cfs`,
each with `node_count` entries. The advance request supplies the accepted
post-routing totals before the network books its system boundary balance.
The surface's native outfall owner uses their difference to distribute
discharge and consume reserved donor water. Its existing point/cell partitions,
spread receivers and unused-reservation expiry perform the transfer. The
receipt includes the gross `outfall_in` and `outfall_out` water ledgers.

Preparation and conductance keep the clock fixed. Each advance must consume
its matching preparation at the same clock and routing duration. Finish
requires the previous reservation to have been consumed. Duplicate, stale or
mismatched phase requests stop the run. Tailwater relaxation, flap gates,
overlapping donors, multiple barrels and mixed node/outfall exchange retain
their native calculations.

The [outfall example](../examples/hydrocouple/split-outfall/composition.json)
uses this protocol through the same component connection as the node-only
example. Its pan receives discharge and subsequently returns water to the
network.


## Version 3 ordinary watershed and conventional LID outlets

The [split-watershed composition](../examples/hydrocouple/split-watershed/composition.json)
and its adjacent INP provide a runnable relative-path example.

Version 3 retains the same phase port and native node/outfall transactions. It
selects one source owner: the network runs the existing runoff substeps,
conventional LID kernels, drain/runon routing and routing-time interpolation.
Their water reaches the mesh through native node spill and mapped outfalls.
The surface skips runoff and conventional LID advancement. No new watershed
solver, source clock or water delivery algorithm is introduced.

Every version 3 packet carries positive `source_count`, equal to the configured
subcatchment count, and `mapped_outfalls`, which may be empty. Geometry also
carries the exact ordered `gage_ids`. A source-bearing model refuses versions
1 and 2. Model identity and source count must match both independent engines.

Only ADVANCE requests carry `weather`:

| Field | Meaning |
| --- | --- |
| `gage_rates` | Current network gage intensities in project rainfall units (mm/h or in/h), in negotiated gage order. Nonnegative finite values. |
| `gage_current` | Native current record index per gage, or -2 for no state and -1 for the pre-record interval. Nonnegative indices must exist in the matching source series. This is a boundary view for the native read-only rainfall report, not a second gage driver. |
| `evap_ft_per_s` | Native climate evaporation rate in ft/s. |
| `samples` | Ordered objects with `start_s` and `rates`, one per newly evaluated native runoff weather boundary. The first accepted journal starts at 0. Starts strictly increase across transactions and cannot exceed the requested interval end. |

The source owner's existing independent runoff clock can evaluate ahead of the
routing clock. These samples retain its exact native boundary times and rates;
they are not resampled at coupling times. Before advancing, the surface replays
them through the existing `bookGageRates` history and installs the native
reporting cursor and evaporation. It consumes no source water independently.
Repeated journal entries, malformed rows, missing initial history, source/gage
order mismatches and schema downgrades are refused before the surface advances.

This increment qualifies ordinary watershed outlets, conventional rain-barrel
LID drains to a node and to another subcatchment, LID overflow, changing two-gage rainfall,
climate evaporation, node/outfall delivery, binary-only output and early finish.
It does not open reviewed surface footprint ownership or any aquifer path.
Those must retain R4 source pools and R5 joint trial/acceptance semantics in a
future phase contract. Detailed per-unit LID report destinations are refused
until their output ownership is isolated.


## Version 4 mesh groundwater node and conduit water exchange

The [split-groundwater composition](../examples/hydrocouple/split-groundwater/composition.json)
and adjacent INP provide a runnable relative-path example.

The `integrated2d` participant owns the existing mesh groundwater solver,
shared/independent groundwater clocks, tiers, pending cohorts and surface
infiltration/return exchanges. The network has no active groundwater solver.
Both open the same full configuration. The bounded profile requires active
MESH groundwater, Gardner soil, explicit CLOSED_FORM, ENSLAVED or SIGMA closure,
GW_ET NONE and water-only transport settings. Reviewed source footprints,
legacy subcatchment aquifers, node LIDs and joint Richards bottoms remain
refused before advancement.

Version 4 uses the same phases and port. It retains the native generic node
exchange law and pending node queues. Its request `node_inflow_cfs` and
`node_outflow_cfs` columns are required even without mapped outfalls; the native
node donor limiter needs the accepted inflow as well as held volume. This
version permits groundwater-only mesh coupling with no surface node/outfall
mapping. It requires `source_count` even when zero and `mapped_outfalls` even
when empty. Ordinary source-bearing models also retain the version 3 weather
journal unchanged.

Geometry additionally carries:

| Field | Meaning |
| --- | --- |
| `link_ids` | Exact ordered model link IDs. |
| `conduit_links` | Ordered base-link indices for native conduit rows. |
| `conduit_count` | Count of these rows, repeated in every version 4 packet. |
| `aquifer_bed` | One 0/1 ownership flag per node. The network installs the native resolved flags so a mapped storage bed suppresses its separate storage exfiltration owner. |

Geometry and ADVANCE/FINISH receipts carry `groundwater_conduits`, with one
entry per conduit in each column:

| Column | Native quantity |
| --- | --- |
| `coupled` | 0/1 flag selecting the signed native conduit/aquifer law. |
| `head_relative_ft` | Native water table elevation relative to the conduit mid-invert, ft. |
| `conductivity_ft_per_s` | Native bed conductivity, ft/s. Nonnegative. |
| `path_ft` | Native characteristic path length, ft. Nonnegative; positive for a coupled row. |
| `gain_limit_cfs` | Published finite nonnegative groundwater gain allowance per barrel, ft3/s. |

The network installs these exact native values before its first routing step
and after each accepted advance. An ADVANCE request sends `conduit_seepage_cfs`,
the signed accepted native loss rate per barrel: positive leaks from the pipe
and negative gains into it. The surface passes this rate to the existing native
post-routing owner, which applies barrel counts, length shares, pending budgets
and groundwater firing. It does not recompute or independently debit the
hydraulic exchange. ONE_WAY and TWO_WAY retain their existing native laws.

Geometry and ADVANCE/FINISH receipts also include diagnostic
`groundwater_ledger_m3`, exactly 16 finite values in public `SWMM_GW2D_LED_*`
selector order: recharge, lateral net, deep loss, node net, Dunne return,
capillary rise, ET, infiltration input, initial storage, current storage including
pending, link net, source input, source output, rejected infiltration, boundary
input and boundary output. The diagnostic does not create a network aquifer or
book another transfer. Read groundwater results from the surface HDF5 owner.

The codec validates schema/configuration compatibility, link/conduit order,
flags, dimensions, finiteness and budget signs. An aquifer-bearing model refuses
schema downgrade. A groundwater-only participant does not require an outfall
preparation phase; mapped outfalls retain their existing preparation contract.
This increment introduces no joint Richards trial or admission operation.


## Version 5 reviewed footprint source ownership

Reviewed SUBCATCH UNIFORM footprints retain the existing native R4 owner:
`SourceWaterDriver`, its physical pools and clock groups, `CommonSourceStage`,
`SurfaceExchange` and `SourceWaterDeliveryBridge` live together in the surface
participant. The same native stage builder initializes managed complete-model
runs and split runs. Ordinary unowned sources stay in the network. Owned weak
catchment graphs are suppressed in the network's ordinary runoff/LID kernels.
No source driver or delivery queue is recreated in the receiving network.

Version 5 requires active mesh groundwater, Gardner soil and an explicit
qualified closure. It retains the native reviewed ET handling, all-cell common
source schedule, pending cohorts and mesh weather remainder. The current
profile requires mesh results, wall boundaries, NODE_ENROLMENT ROWS,
LINK_SEEPAGE NONE, no enabled groundwater node beds and no node LIDs.
Qualified mapped outfalls use
the existing preparation phase; authored return destinations select version 7. These restrictions retain
the qualified native common-source scope; joint Richards admission and broader
return paths need additional phases. A source-bearing geometry refuses versions
1–4. Validation uses the existing ownership preview and exact configured model.

Geometry adds `subcatchment_ids` in exact model order and `source_groups`, one
native source-clock group index per subcatchment (-1 means ordinary/unowned).
The network checks this map against its own native graph resolver before start.
Ownership polygons, component areas and cell shares remain available in the
configured model metadata. Areas are not independently recomputed by the wrapper.

Phase **5 SOURCE_PREPARE** runs before the network runoff/routing pass. It calls
the existing bridge's `stage` for the next native receiving interval and returns
its held outlet rates and preceding accepted rates. It advances no pool, mesh,
groundwater or hydraulic clock. The network uses these native values in the
same inflow assembly and held-rate/trapezoidal accounting as an embedded engine.
The bridge remains staged until the network issues the matching ADVANCE after
its accepted hydraulic solve. The surface then advances the native common
source/aquifer schedule and commits/captures its bridge once. Newly captured
outlets become available to the following receiving interval, as in native
execution. No interpolation or second debit is performed by the component.

SOURCE_PREPARE, conductance and ADVANCE share the exact routing duration. An
advance without preparation, a replayed offer or a changed duration is refused.
The public packet clock retains its routing-millisecond representation; native
bridge staging uses the surface engine's corresponding unrounded native clock.
This preserves the native source receiver arithmetic without resampling clocks.
A failed accepted step stops the pair; it does not provide rollback or retry.

Geometry and SOURCE_PREPARE/ADVANCE/FINISH receipts carry `reviewed_sources`:

| Field | Meaning |
| --- | --- |
| `held_rate_cfs`, `previous_rate_cfs` | One nonnegative finite native held rate and preceding accepted rate per hydraulic node, ft3/s. Geometry initializes them to zero. |
| `pending_runoff_ft3`, `pending_drain_ft3` | Separate nonnegative finite per-node water inventories still held by the native bridge. These are diagnostics, not another network queue or another transfer. |
| `totals_ft3` | Six native cumulative source totals: rainfall, evaporation, infiltration, aquifer/outfall returns, published runoff and published LID drainage. |
| `storage_ft3` | Native accepted inventory in reviewed pools and unsettled internal catchment groups, ft3. |

The native held-rate correction uses both rate columns for node volume,
statistics and routing balances. ADVANCE also updates the network's diagnostic
source totals and accepted inventory. Native source/groundwater objects retain
all physical state. FINISH follows native teardown and retains the last accepted
source history and offer; it is not a graph checkpoint. Early-finish files may
contain zero-row datasets, whose full structure and attributes are checked in
addition to ordinary full HDF5 equality.

The profile is qualified against native managed runs with source groups,
conventional LIDs, shared/independent groundwater clocks, all three closures,
ET and early finish. The earlier complete-model source and joint Richards
results remain unchanged. Version 5 does not carry mapped node-LID bottom
trial/admission/acceptance. Version 6 adds the bounded Richards profile below.

## Version 6 joint Richards node admission

Version 6 extends the reviewed-source profile with water-only NODE LIDs whose
bottom is MESH_AQUIFER and whose authored flow model is Richards. The surface
participant retains the sole active aquifer, finite directional reservations,
gross receipts, pending cohorts and owner clocks. The network retains hydraulic
routing and accepted layer-port transfers. Each participant has its own model
and owned boundary snapshots. No context, router or solver is shared.

The protocol adds two operations at the native pre-routing LID preparation point,
after runoff and lateral inflow assembly:

1. **6 LID_TRIAL** sends the finished preceding node-column state, current native
   hydraulic boundary, assembled lateral rates, evaporation and infiltration
   factor. The surface calls the existing native node-admission implementation
   to build private column/publication drafts using its actual aquifer budgets.
   The native pressure/storage envelopes, joint directional awards and bounded
   Richards solves are reused. The trial publishes no column water or aquifer
   receipt and advances no owner clock. Its receipt returns the proposed water
   state and a `lid_trial_sequence` bound to that trial's sequence.
2. **7 LID_ACCEPT** confirms that exact pending trial and duration. The surface
   retains the native gross groundwater receipts once and publishes the native
   drafts. The network installs the accepted column/hydraulic state, then runs
   the ordinary native hydraulic solver and `lidnode::finishStep` once. The
   acceptance does not advance the aquifer to the hydraulic clock.
3. **2 ADVANCE** carries the finished hydraulic column state and port traces to
   the surface. The surface verifies the exact completed owner endpoint, adopts
   the accepted boundary snapshot and runs the existing post-routing schedule.
   Pending receipts settle at the native aquifer firings, including independent
   clocks and all three qualified closures. No second bottom exchange is booked.

All packets carry the ordered `lid_nodes` cohort and trial identity. A new trial
requires a staged source interval and no pending trial or unconsumed acceptance.
Only the matching acceptance may follow a trial. Conductance/ADVANCE require
acceptance for their exact duration. FINISH requires no unconsumed trial or
acceptance. Replayed, missing, stale or differently timed phases stop the pair;
there is no rollback/retry. An accepted hydraulic interval must finish its
native ports before its column state can cross ADVANCE.

Trial/acceptance packets and ADVANCE/FINISH boundaries carry `richards_nodes`:
ordered node rows with native column water/pressure, cell moisture, solver
report/transfers, hydraulic port traces, captured/held/treated water, evaporation
and exact owner endpoints/flags. `hydraulics` has eight fields in order: head,
depth, mobile volume, full volume, report full volume, lateral flow, storage
evaporation loss and storage exfiltration loss. Length is ft, volume is ft3 and
lateral flow is ft3/s. Column solver reports retain their native SI quantities:
water transfers/balances are m3 and time is seconds. Cell moisture is a fraction.
Authored geometry, materials and controls are read from the matching configured
model; they are not restored from packets. Chemistry and thermal fields are
excluded. Decoding validates node order/cohort, column extents, finite values,
nonnegative inventories and transfer/port indices. Version 6 models refuse
schemas 1–5.

The network can initialize without a local aquifer, but must import resolved
paired aquifer bed flags from matching surface geometry before start. The surface
still performs native bed resolution and common-source eligibility checks. Other
node-LID formulations, non-reviewed profiles, ordinary groundwater node beds,
conduit seepage and non-reviewed node-LID formulations remain outside this
bounded version. Qualified mapped outfalls use the existing preparation phase;
return destinations select version 7. Surface mesh results remain required.

Nine positive profiles cover shared/independent clocks for CLOSED_FORM, ENSLAVED
and SIGMA, competing columns on one aquifer cell, upward flow with evaporation,
downward flow and early finish. All column water/pressure/head values, adaptive
solver statistics and public bottom receipt diagnostics match native execution
at accepted boundaries. Two native replay checks and malformed packet tests
exercise failure paths. Binary and full HDF5 equality also hold through Composer.
The relative-path example is
[`split-richards/composition.json`](../examples/hydrocouple/split-richards/composition.json).

## Version 7 reviewed outfall return receipts

Reviewed source models may use the existing mapped-outfall preparation and
accepted exchange, including native tailwater, withdrawal limits and flap gates.
The source bridge is staged first. OUTFALL_PREPARE (phase 4) then reserves the native interval;
Richards trial/admission follows when configured. Conductance and ADVANCE must
consume that exact prepared interval. No shared hydraulic solver or checkpoint
flags are introduced for this external source attachment.

An authored outfall return destination selects schema
`openswmm.node-surface-water/7`. This retains the reviewed source and optional
Richards fields of versions 5/6 and adds:

| Field | Scope and meaning |
| --- | --- |
| `outfall_return_targets` | Geometry: native subcatchment destination indices (-1 for none), in native outfall-row order. Must match the configured model exactly. |
| `outfall_returns_ft3` | ADVANCE request and receipt: one finite nonnegative volume per original subcatchment. Contains native-booked water for reviewed groups only; unowned rows are zero. The receipt echoes exactly the accepted inventory. |

The network performs native routing and retains the accepted outfall inflow and
outflow view before the native balance pass can rewrite inflow on backflow.
The existing `updateRoutingMassBalance` books the authoritative return volumes.
Only return-bearing reviewed profiles defer their ADVANCE request until this
booking completes. The surface uses the original accepted flow view for mapped
exchange and installs the booked return inventory. Its normal native step
advances the source/aquifer schedule, flushes report boundaries, advances the
clock and calls the existing `creditOutfallReturns` before bridge commit/capture.
No hydraulic law, return volume or receiving rate is recomputed by the wrapper.

After successful acceptance the network clears the transferred reviewed return
inventory once. Its ordinary unowned catchments retain their original network
runon volumes and runoff clocks. The private source driver retains native return
availability, pending water, source group clocks and later source firing. Invalid
inventory, unowned positive credit, schema downgrade or changed return mapping
is refused. A failure stops the pair without retry or rollback.

A single outfall cannot simultaneously map the same discharge to the mesh and
return it into a reviewed source. A graph may use separate mapped and return
outfalls. The prior CPU/water-only/Gardner/common-source restrictions remain;
ordinary groundwater node/link beds and other node-LID formulations need separate
qualification. The native INP reader now resolves outfall destination names after
all catchments have loaded and rejects unknown names. Files whose return was
previously silently discarded now execute the authored return; other models
retain their native result compatibility gates.

The combined Richards/outfall witness retains a regular native node-coupling
point. Outfall-only Richards layouts are outside this qualification; native
checkpoint/owner guards remain in force.

## Version 8 classical NODE LIDs and mixed columns

Models containing classical NODE LIDs select
`openswmm.node-surface-water/8`. Existing models retain their earlier schema.
The [split-lids example](../examples/hydrocouple/split-lids/composition.json)
combines a classical column with a Mesh Richards column on the reviewed native
source/aquifer schedule. Both participants read the same authored geometry and
materials; packets transfer accepted water state only.

Every packet carries `lid_nodes`, the complete ordered NODE LID cohort, and
`mesh_richards_nodes`, the exact ordered subset requiring native mesh admission.
`source_count` and `mapped_outfalls` remain present even when zero/empty. Source,
groundwater and outfall-return fields retain their earlier contracts when those
owners are configured. A formulation, subset or schema mismatch is refused.

Version 8 uses `lid_states` instead of `richards_nodes` on LID_TRIAL/LID_ACCEPT
and ADVANCE/FINISH packets. Each row retains the version 6 fields and adds:

| Field | Meaning |
| --- | --- |
| `richards` | Boolean, equal to the native authored column formulation. |
| `state.green_ampt` | Complete native surface infiltration history: `S`, `Ks`, `IMDmax`, `IMD`, `F`, `Fu`, `Fumax`, `Lu`, `T`, `saturated`. Native length is ft, conductivity ft/s, time s, moisture deficit a fraction. |
| `state.infiltration_cell` | Native cell index, required to match the configured column. |
| `theta` | Moisture fraction for every cell of either formulation. Classical rows have empty Richards water/pressure arrays. |

Requests also carry `lid_recovery_factor`, the native climate recovery factor,
alongside evaporation and infiltration factors. Closed boundaries require zero
unfinished lateral capture. Classical hydraulic port/mobile deltas remain valid
accepted traces after `finishStep`; the next native reset clears them. They are
not interpreted as a second pending transfer. Mesh Richards rows retain their
stricter completed-clock and zero unfinished port requirements.

When there are no Mesh Richards columns, the network runs native
`lidnode::prepareStep` and hydraulic `finishStep` once. It sends the finished
classical state on ADVANCE; the surface adopts it for native bottom/clogging
exchange and reporting without repeating preparation or finish. Classical
MESH_AQUIFER bottoms use the existing native node/aquifer exchange and queues;
CURRENT and SEALED bottoms retain their native laws.

When Mesh Richards columns are present, the surface performs phases 6/7 using
the existing native trial/admission code. Acceptance publishes Richards drafts
and calls native `prepareStepAfterMesh` once for the other classical rows. The
network installs the accepted cohort and routes/finishes it once. The trial
retains the original routing duration for ordinary preparation, rather than
reconstructing it by subtracting clock endpoints. No aquifer clock, receipt,
water transfer or column solve is duplicated.

Qualified cases include reviewed classical mesh beds with shared/independent
clocks and all three closures, upward exchange, evaporation/clogging, sealed and
current bottoms, mixed classical/Richards columns, early finish, and ordinary
unreviewed watershed sources with mesh beds and conduit seepage. All cell
moisture and public water ledgers match at accepted boundaries; native binary
and full HDF5 output equality are required through direct adapters and Composer.
Malformed history, extents and formulation/subset changes are refused.

Native managed-source eligibility guards still require explicit LID ownership
for active node beds and refuse conduit seepage in reviewed source models.
Broader reviewed source/generic groundwater bed/conduit combinations remain
outside this qualification. Non-Mesh Richards bottoms and outfall-only Richards
layouts also remain outside the bounded profile. This is water-only execution;
species, heat and graph recovery need their own contracts and qualification.
