# street_inlets — one street reach, two ways of attaching an inlet

| | |
|---|---|
| Deck | `street_inlet_junction.inp` |
| Copied from | `examples/inlets/street_inlet_junction.inp` in this repository, 2026-09-20 |
| Used by | the Application Manual's street-inlets chapter; figure `workflow_ch5_inlet_capture_vs_bypass` |
| Changed on copy | nothing: byte-identical to the example |

The example is maintained in `examples/inlets/` with its own README and is
exercised by the engine's tests; this copy exists so the manual's figure has
an input under `docs/figures/decks/` like every other simulated-tier figure.
Keep the two in step: if the example changes, re-copy it and rebuild the
figure.

## What the deck contains

One street reach draining to a parallel sewer through two inlets attached
differently — a conduit-attribute inlet on the first reach and an inlet
junction between the second and third — and all four `[INLETS]` design
grammars, including the two-line combination encoding and a custom design
pointing at a diversion curve.
