# Native HydroCouple component hosting

Build with `OPENSWMM_WITH_HYDROCOUPLE=ON` to host a foreign component. SWMM links
the HydroCouple interfaces and JSON headers; HydroCoupleSDK is not linked.
Builtin `[PROCESS_COMPONENTS]` rows remain native configuration appliers.

## Registration and persistence

A foreign row uses a stable **instance** ID as its first token and an explicit
library and component **type**:

```text
[PROCESS_COMPONENTS]
ReceivingReach library="components/receiver.so" component=org.hydrocouple.openswmm.test.sink config="receiving-reach.json"
```

Library and configuration paths resolve against the parent model. Within the
hosting document, `output_directory` and file-argument references resolve against
that document. Library discovery uses the shared stamped loader; an incompatible
library is refused before its C++ factory is called. Instances remain owned by
their native engine; library code outlives all instances. Duplicate instance IDs,
recursive native-engine components and multiple receiving owners of one native
outfall are refused. Receiving instances also require separate output directories.
Missing libraries include the platform loader diagnostic.

Metadata-only native INP opening parses the hosting document and exposes the row's extra
arguments without loading or creating the foreign model. Authoring through
`swmm_process_component_argument_get/set` preserves extra arguments and ordinary
INP/GeoPackage serialization. Editing a loaded binding requires save/reopen before
execution. A nested hosted graph inside the complete-engine HydroCouple wrapper
remains refused until that graph's ownership and recovery are qualified.
The existing GeoPackage plugin open creates configured bindings; it is not the
INP-only metadata entry point. Opening and saving never prepare a receiver or
advance its clock. Initialized instances receive cleanup on failed setup and
on engine close, including when no run was started.

## Qualified export profile

The first native-host profile is the existing `hydrocouple.interval-transfer/1`.
It is deliberately identical to the complete-engine component's direct receiver
contract: LEGACY mass pollutants in MG/L or UG/L, native heat enabled, no MSX or
spatial exports, and external outfalls without subcatchment rerouting. Amounts
are water m³, pollutant kg and sensible enthalpy J relative to 0 °C. Species IDs
are stable and can be declared in a different receiving order. Undefined mass
bases, count pollutants, renamed fields and unit conversions are refused.

```json
{
  "schema": "openswmm.hosted-component/1",
  "profile": "hydrocouple.interval-transfer/1",
  "output_directory": "out/receiving-reach",
  "arguments": {
    "config": {
      "schema": "hydrocouple.interval-transfer/1",
      "epoch_julian_day": 2461041.5,
      "expected_transfers": 3,
      "receipt_file": "receipts.csv",
      "species": [{"id": "Dye", "basis": "kg"}]
    }
  },
  "exports": {"outfalls": ["O"], "exchange_times_s": [60, 120, 600]},
  "connections": [
    {"output": "transfer_id", "input": "transfer_id"},
    {"output": "epoch_julian_day", "input": "epoch_julian_day"},
    {"output": "interval_start_s", "input": "interval_start_s"},
    {"output": "interval_end_s", "input": "interval_end_s"},
    {"output": "water_m3", "input": "water_m3"},
    {"output": "enthalpy_j", "input": "enthalpy_j"},
    {"output": "species.Dye", "input": "species.Dye"}
  ]
}
```

Argument values are JSON. A single-key `{"@uri":"configuration.json"}` value
requests an explicitly supported File argument. Unknown arguments/types and
unknown hosting keys fail before physical advancement. The instance ID is set by
the process row, not a second argument in the document. The foreign reference
directory is its fresh output directory; relative runtime output arguments use
that private directory. Arbitrary foreign component outputs retain their own
documented behavior; the host cannot isolate paths hidden inside foreign code.

Every interval field must connect exactly once to the same receiving owner.
Acknowledgments are scalar `accepted_transfer_id` and `accepted.<amount-field>`
outputs with matching SI units, dimensions and extensive quantity semantics.
There are no adapters, fan-out or implicit concentration-to-mass conversions.

## Native schedule and physical ownership

The native solver chooses its ordinary steps, clipped at the configured exchange
endpoints on its millisecond clock. At each endpoint the host differences the
authoritative cumulative outfall receipts, publishes an already-debited tuple,
then updates the receiver once and validates the token and accepted amounts.
The final publication marks the donor Done so the receiver can validate its own
completion. Reading an item never advances native physics.

The native outfall owner already removed exported water, species and heat.
Neither the host nor a receiver may debit that native inventory again. Partial
acceptance must preserve native tuple proportions; its residual is explicit
pending inventory. Missing, changed or partial acceptance stops further native
advancement. It does not roll back, refund or retry a completed hydraulic step.
Reverse flow requires a separate bidirectional contract and stops this profile.

`transfer_N.json` records the native cumulative receipts, offered/accepted/pending
amounts, exact endpoint, owner and token. The offer is saved before invoking the
foreign receiver so a receiver failure retains its outstanding inventory.
All component errors are drained after lifecycle, argument and connection calls;
error/fatal entries stop the native run and warnings are retained with receipts.
Cancellation between endpoints records otherwise unsent exports in
`cancelled.json` before finishing the receiving instance. Host/graph restart is
not implied by this export profile and remains a separate qualification.
