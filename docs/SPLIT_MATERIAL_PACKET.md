# Experimental material packet adapter

Schema `openswmm.node-surface-water/10` has a separate codec and C interface in
`openswmm_split_material.h`. Schemas 1–9 and their water-only API remain unchanged.
This adapter requires one explicitly bound, trusted **in-process** native pair.
JSON represents values; it does not transfer a spending capability. Independent
processes, authenticated remote channels, paired restart, retry, installed
components, Composer and GUI activation require further qualification.

The profile is the existing optional closed, one-passive-pollutant, isolated wet
storage profile: serial CPU, first-order native surface solver, changing-volume
receiving pools, exact represented slices and retained directional water sums.
The native SoA arrays, solver, quality writer, funding calculation and journal
remain the numerical owners. No HydroCoupleSDK is required.

## Authority and execution

`swmm_split_material_validate_input` checks the same authored eligibility on an
OPENED metadata or executable handle without configuring a role, starting a
clock or writing results. Runtime configuration, geometry and binding remain
separate checks. Builds without the required material features return the
feature refusal.

Configure the two executable OPENED handles with `swmm_split_material_configure`.
Export initialized surface geometry and import it into the initialized network
using the separate material codec. Set the ordinary native split callback before
start. Start both engines, set the network's restricted material transport, then
call `swmm_split_material_bind` exactly once with capacity 1..65536. Binding checks
native configuration, ordered nodes/points, pollutant/basis, conversion and initial
hydraulic/quality stocks. Neither endpoint may have another material owner.
Geometry requires all coupled-node flags and mapped footprint areas. Native
FOOTPRINT defaults omit both optional chamber-area/band columns; otherwise both
columns must have node-sized extents. Mixed shapes are refused before decoding
publishes a frame.

The ordinary callback round-trips outer geometry, CONDUCTANCE, ADVANCE, settled
REPORT (phase 8), and FINISH frames. CONDUCTANCE changes no physical clock. REPORT
and FINISH have zero outer duration but may close a positive-duration physical
mesh window. Both intervals remain explicit. A routing step without a physical
window issues no material offer. An actual empty window still consumes its native
ordinal and publishes its hydraulic observation.

The restricted callback runs at three native transaction boundaries:

| Stage | Retained owner and action after successful full-value comparison |
| --- | --- |
| OFFER (1) | The network retains the unique immutable native issuance. Only its complete matching returned value releases that capsule once to the registered producer, before any cell credit. |
| PROOF (2) | The channel retains the actual sealed producer journal/proof. The returned value includes its complete issued view, applied legs, directional rows, per-node spend and actual queue/direct/span. The network compares that value, prepares material and water, then publishes the native water booking once. |
| ACK (3) | After native receiving confirmation, the channel retains the unique acknowledgement. The returned value binds the issued window and committed queue/span. Only then does the producer retire its outstanding acknowledgement. |

Every inner packet binds issuer, recipient, nonzero session, outer sequence,
outer phase, native ordinal, receipt, generation, capture time, physical endpoints,
point map, concentrations and allowance. The native capsule and session state
remain outside the copyable packet and boundary frame. Decoding/copying the same
packet twice has no ownership effect. Matching identifiers alone never suffice;
the entire returned value must match the retained native value. Member order and
JSON whitespace are immaterial.

A failed offer stops before a new native leg. A missing or invalid proof leaves
the full unresolved network allowance and the surface's actual applied journal.
A lost ACK is **after commitment**: the network retains its committed receipt,
the surface retains its pending acknowledgement, and the session stops. Neither
case permits retry, rollback, new issuance or implicit discard. Accepted surface
step notification still follows network quality/output and its step-end callback.
Natural elapsed=0 completion is separate from successful finalization.

## Representation and limits

Every packet contains `schema`, integer `stage`, `node_count`, `basis` and `body`.
Inner packets additionally contain `issuer`, `recipient`, `session`,
`outer_sequence` and integer `outer_phase`. Unsigned 64-bit identities, ordinals,
receipt IDs, generations and point IDs are canonical decimal strings (no sign or
leading zero except `"0"`). Topology indices, enum values and counts are bounded
JSON integers. This avoids converting an identity through a JavaScript number.

Each physical binary64 number is an object with exactly one member:
`{"f64":"3ff0000000000000"}` represents 1.0. The string contains 16 lowercase
hexadecimal digits describing the IEEE754 bits, most significant first. It is
independent of host byte order and retains subnormals and signed zero. Nonfinite
values and ordinary JSON floating-point literals are refused.

Exact amounts use `{"parts":[...]}` with at most 64 finite nonzero binary64
components in their original native order. These growing expansions are not a
unique normal form: re-adding their parts may produce a different component list.
An empty array is zero. A separate exact sum supplies sign and funding checks;
the transmitted list is never normalized. Redemption bit-matches its original
order and components, so even an equal-value repartition is refused. Existing
canonical receipt-row and archive checks stay unchanged. No rounded scalar,
decimal reconstruction or wrapper unit conversion replaces the exact sum.
The declared bases are MG/L, UG/L or #/L; native amounts retain concentration*ft3,
surface amounts concentration*m3. Geometry declares class BULK, the exact
pollutant ID, volume conversion and supported numerical policies. Other species,
classes, remapping or ownership profiles are refused.

The codec refuses duplicate object keys, embedded NUL, unknown/missing record
fields, invalid integer/physical types, zero components or nonempty zero
expansions, mismatched node/point extents, basis changes and nonfinite/negative gross amounts. It bounds
input to 64 MiB, nesting to 32, parser work to 3 million events, arrays to 65536,
node/point extents to 4096 and strings to 8 MiB. Encoder work is bounded before
constructing native payloads; a proof is limited to 32768 encoded legs. A native
journal capacity is not a promise that every possible window fits the packet
budget. A packet-budget refusal retains the native obligations and stops.

The callback response is borrowed and must remain valid after callback return
until the library immediately parses it. It may alias the immutable request.
Caller-owned heap/user storage is also suitable; a callback-local stack buffer is
invalid. The library never frees this response. Null and oversized responses are
refused before reading. Reentrant evaluation, stepping, callback changes and pair
teardown are prohibited until consumption; observational codec operations are
allowed. Release codec-created text with `swmm_split_packet_free` and frames with
`swmm_split_frame_destroy`. Outputs remain untouched on refusal. The thread-local
material diagnostic does not overwrite an engine's original terminal diagnostic.

Qualification artifacts remain local under
`tests/verification/component_material_packet_2026-11-07`. This packet adapter
does not advertise general CF4 material transfer or remote authenticity.

## Component endpoint

The optional engine component module supplies distinct `split_material_network`
and `split_material_surface` roles on `openswmm.node-surface-material.v10`.
The surface output is a private same-plugin endpoint, not a generic string
transport. Wiring captures weak generation tokens before preparation; lazy
network start explicitly binds the two opaque native handles after both have
prepared. No global engine lookup or public context/book pointer is used.

Each component lifecycle operation leases both endpoints before status signals
can run, including failure reporting and cleanup. Native outer dispatch has a
single-use authorization; unsolicited output pulls are refused even while a
status callback is running. Nested OFFER/PROOF/ACK callbacks use a separate
owned response buffer and copy/validate the native packet without calling
`swmm_split_evaluate` again. This keeps the active outer request and reply
intact. Only the accepted network boundary publishes component time.

Both preparation and finish orders are supported. A close refusal retains the
native handle and cannot publish a successful Finished status. An expired peer
is refused before its raw port pointer is dereferenced. This is a synchronous
trusted in-process profile; it does not add remote packet authenticity or
coordinated component restart. See [component configuration](COMPONENT_ENGINE.md)
and `examples/hydrocouple/split-material`.

## Private recovery preparation

The internal `PeerPassiveStorageCheckpoint` stages only acknowledged native
funding and producer owners. It keeps the existing peer-book standalone
checkpoint refusal. A versioned, bounded same-platform envelope records the
closed topology, conversion, journal capacity and native sealed receipt history.
The native replay validator checks initial pool anchors, funds, current/old
concentration, queue history and remaining delivery obligations. Producer legs,
latest spend and publication ordinal are reconstructed from that history;
empty water publications advance the ordinal without inventing a receipt.

Restore requires fresh enrolled target owners and separately decoded incoming
network/surface observations with matching node/species/basis and point map.
Preparation leaves both targets unchanged. Its one-shot candidate pins the exact
owning slots, requires target validation, and swaps both owners without throwing.
The caller must exclusively retain the slots and address-stable contexts/state,
validate every numerical domain, then install those domains before committing
this owner candidate. This private seam does not enable engine, component or GUI
resume. Hydraulic/CPU/mesh inventories, split scheduling and output continuation
still require the coordinated pair transaction.

Open grants, frozen deliveries, prepared quality, unacknowledged replies and
terminal owners cannot be saved. Restored sessions receive fresh host-entropy
capability epochs, including when the host forks after prior attachment. Archived
identities are provenance, never authority. Retired native capsules and the last
`returnedReply()` diagnostic are not resurrected. Schema 10 remains a value
representation redeemed against current native issuance; these session labels do
not provide remote authentication.

Partial receiving-owner continuation is qualified for the authored exact-slice
profile: save after the first or second nonzero slice, restore the private owner
against independently captured native numerical observations, and continue the
remaining no-publication intervals through native step seven. All three bases
and both directions match the uninterrupted split pair and integrated reference.
Fresh executable hosts reproduce the same receipt remainders, concentration,
queue/span and retained roundoff. Exact-fraction checks cover represented slice
partitions, signed delivery, funded pending spill and inventory plus roundoff.

This qualification supplies observed native hydraulics to the restored receiving
owner; it does not restart a hydraulic or surface solver. The actual ADVANCE must
first prove no new publication or refill. The next refill requires its genuine
funding/proof/ACK handoff and a separately restored surface marcher. Full paired
numerical state, split scheduling and coordinated output continuation remain the
next recovery gates; public engine/component/GUI resume stays disabled.

## Private outputs-off paired numerical restart

The internal C++ `SWMMEngine` pair coordinator now supports
`enableMaterialPairCheckpoint()`, `materialPairCheckpointBytes()` and
`restoreMaterialPairCheckpoint()` for the closed material profile. Explicitly
attach at the fresh, running boundary of an already bound pair with outputs off.
Save only at acknowledged, accepted running boundaries; restore only into a
fresh, identically configured and bound pair. Standalone split checkpoints and
public C, component, Composer and GUI resume remain disabled.

One bounded, versioned same-platform envelope captures both actual engine
contexts, the network routing state, distinct surface node columns, surface CPU,
mesh and report interpolation state, exact published node concentration, native
water/material ledgers, peer owners, pending delivery spans and split scheduling.
Preparation validates every domain and its joins before either target changes.
Installation is nonthrowing and preserves target-local callbacks, channels and
context addresses. Both numerical roles and the funding session receive fresh
identities; saved identities remain provenance. No solver prefix is rerun to
reconstruct the saved state, and no HydroCoupleSDK dependency is added.

Qualification covers all three concentration bases, both flow directions,
partially delivered receipts, pending tails, empty publications and the next
actual funding/proof/ACK refill. Fresh executable hosts restore at time zero and
advance only the remaining steps. Complete numerical snapshots match the
uninterrupted pair after excluding fresh authority identities. Checksum-valid
second-role damage refuses without changing either target; valid retry succeeds.

Saving remains optional and subject to the unchanged native replay-work and
archive limits. A later save can refuse while the live simulation continues;
refusal leaves the caller's result buffer untouched. In the authored long-tail
fixture, snapshots through step 17 are admissible, later saves exceed the work
limit, and both uninterrupted and earlier-restored pairs still reach native
completion at step 80. This is not a promise of late-run checkpoint availability.
Coordinated output-file publication and public/component/GUI recovery are the
remaining gates. Evidence is local under
`tests/verification/component_pair_checkpoint_2026-11-08`.

## Private paired output continuation

The explicit C++ call `enableMaterialPairCheckpoint(true)` selects the separate
paired output profile. Before start, prepare the network with
`prepareOutputCheckpointing()` and the surface with
`prepareSurfaceOutputCheckpointing()`, then start them with `start(1)` and
`start(0)` respectively. Both destinations must be fresh. Text reports, averaging,
extra output plugins, buildup and external checkpoint companion files are outside
this profile. Public C, component, Composer and GUI resume remain disabled.

The version 2 pair envelope binds both actual output prefixes to the numerical
pair. The network binary and surface HDF5 writers retain independent validated
report clocks, mesh cadence and REPORT_START selection. The material HDF5
checkpoint profile retains the single declared pollutant, its selected row,
native concentration unit, complete report history and append handle. It
validates that selection at enrollment even before the lazy concentration
dataset exists. Restoring a zero-record checkpoint creates that dataset on its
first report; restoring a populated prefix reopens it for append.

Both prefixes and all numerical domains prepare before publication. One existing
`CheckpointOutputPublication` replaces both fresh destinations, then the writers
and numerical owners adopt their prepared state without throwing. Failed
preparation preserves both fresh files and solvers, allowing valid retry. The
publication primitive rolls back an earlier rename if the second rename fails;
this does not provide power-loss recovery or a single visibility event to
external readers. The forced second-rename qualification exercises that primitive
against the actual fresh writer destinations; invalid-prefix tests exercise the
complete pair restore path.

Native post-quality REPORT ordering remains authoritative. A separately recorded
publisher sample time joins the latest accepted REPORT publication and delivery
generation; it distinguishes settled current concentration from the old sample
used by ADVANCE. The outputs-off version 1 pair and existing water-only output
formats retain their bytes and guards.

Qualification includes 80 restores over all concentration bases and both signs,
different mesh cadence and REPORT_START gating, plus 12 fresh executable restores
without prefix replay. Final binary bytes and 102 complete HDF5 comparisons match
uninterrupted native execution. The authored report cadence uses supported
half-window receipt deliveries. Other cadences can reach the existing exact
partition refusal, and frequent publications can reach the unchanged optional
checkpoint-work limit sooner. Refused later saves preserve the caller's buffer;
live and restored runs can continue through accepted native steps and normal end.
Evidence is local under
`tests/verification/component_pair_output_restart_2026-11-08`.

## Paired C recovery

`openswmm_split_material.h` exposes paired recovery through an immutable
`SWMM_MaterialCheckpoint` value. It coordinates both actual bound roles, including
both writers in the output profile. Ordinary single-engine checkpoints still
refuse split participants. Component/Composer/GUI resume remains gated: a compiled
C backend feature does not establish the host's collective checkpoint ownership.

Query `swmm_split_material_checkpoint_capabilities()` for compiled numerical and
binary/HDF5 support. After configuring the material roles and initializing and
exchanging geometry, outputs-enabled callers prepare each role with
`swmm_split_material_checkpoint_prepare_outputs()`, then start the network with
`start(1)` and surface with `start(0)`. Outputs-off callers skip preparation and
start both with `start(0)`. Set the native callbacks and transport, bind the pair,
then call `swmm_split_material_checkpoint_enable(network, outputs)` once.

Capture and restore take the network handle only. Capture requires an accepted,
acknowledged running boundary. Restore requires a separately initialized,
identically configured, bound and enrolled fresh running pair at time zero,
with separate fresh output destinations when enabled. Both participants validate
before output publication and state installation. Callbacks and channels remain
those of the target pair; saved identities are provenance and receive new role
and funding epochs on each restore. A snapshot may seed multiple fresh pairs.

Capture returns an owned snapshot. `swmm_split_material_checkpoint_data()` borrows its immutable
binary bytes and explicit length until `swmm_split_material_checkpoint_destroy()`, independently of
engine lifetime. `swmm_split_material_checkpoint_from_bytes()` copies bounded caller storage and
checks outer framing, native format/platform/version and checksum. Model,
numerical and embedded output compatibility remain restore checks. Snapshot
success replaces the output variable without freeing an earlier value; callers
must release each returned snapshot. Refusal preserves all output variables.
Destroying a null snapshot is harmless. Persistence is caller-owned; the facade
does not open checkpoint paths or translate binary state through schema10 JSON.

Engine recovery is refused during callbacks, stepping, reporting and material
transport, before changing native attachments or output bindings. Immutable
snapshot observation/import is independent of engine execution. Each operation
requires exclusive owner-thread access; simultaneous destruction is invalid.
The separate fixed thread-local `swmm_split_material_checkpoint_last_error()` diagnostic preserves
both engines' original errors. Unavailable builds retain argument validation,
report zero compiled capabilities and refuse recovery operations. Native exact
partition, save-work and 64 MiB archive bounds remain unchanged, as do the
same-platform and trusted in-process restrictions.
