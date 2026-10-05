# Surface–subsurface R1 — shared process contracts (2026-10-05)

The user authorized the original program, Richards first, and then local commits and continued implementation. Richards commits: engine `85ebe251`, GUI `557187b3`. R1 follows the approved `hydrology/surface/` location and the original P0a/P0b split. No push was requested.

## P0a: preserve the existing paths

`surface::InfilBank` owns the per-element method and Horton, Green–Ampt, curve-number and constant states. `RunoffSolver` and `Infil2D` use its single dispatch. Runoff retains its native feet boundary, subcatchment patterns, groundwater cap and LID wrappers. The mesh retains row/tag resolution, provenance, destinations and its held-rate cadence. SI conversion occurs once at the mesh boundary. The infiltration kernels and their arithmetic remain unchanged.

The bank's `BANK | EXTERNAL` flag gives the later aquifer capacity provider an ownership seam. External elements advance neither the infiltration state nor the bank's cumulative diagnostic, and `Infil2D` leaves their published rate untouched. R1 production drivers select no external owners. A dedicated constant-rate slot replaces the old use of Horton `fmin`, with the same unit-conversion operation.

The cumulative bank depth is the integral of published capacity. It is not actual applied recharge: surface supply limiting and the aquifer transfer ledger remain the drivers' responsibility.

The degree-day solver moves into `surface/DegreeDaySnow.*`. `Snow.hpp` retains the existing include/type aliases. `SnowModel` provides a per-element SI step and transient-state pack/unpack, plus the native-unit batch boundary needed to preserve subcatchment arithmetic and plowing order. The engine calls that batch contract; snow-modified precipitation assembly and the existing mass/age ledgers remain unchanged. The SI point step includes snowfall once and does not perform subcatchment plowing. UEB, mesh snow and restart serialization of snow remain later rounds; defining a state packing contract does not itself add a hotstart record.

`SurfaceElement` is the planned non-owning geometry/site view. The new snow-pack assignment API validates before mutation, rejects changes after initialization, supports clearing, and keeps the deferred reference name synchronized. The GUI property and attribute-table pickers share this engine assignment and the existing snow-pack registry/editor. Planned mesh snow parameters appear disabled with an explanation until the mesh snow model exists.

## P0b: intentional corrections

The existing mesh path omits monthly conductivity and soil-recovery factors. The zero Horton drying time also differs: runoff normalizes it to the legacy tiny value while the mesh deliberately uses zero for no recovery (its existing tests rely on this convention). Preserve and document that difference; changing it is outside the planned factor correction. Apply conductivity/recovery factors separately after the P0a gate. CONSTANT's dedicated slot is behavior-preserving in P0a; its conductivity adjustment is an intentional correction in P0b.

## Verification discipline

The accepted baseline is an isolated archive of engine commit `85ebe251`, built in Release with the same numerical flags as the candidate. Only the frozen R1 patch is applied to the candidate source. The staged baseline CLI loads its own staged engine; hashes differ from the candidate, and the corpus runner confirms matching configurations. Source patch, hashes and logs are in `tests/verification/surface_r1_2026-10-05/isolated/`. Earlier shared-working-tree build logs are superseded: another active task changed outfall code during that rebuild, so those logs are not acceptance evidence. GUI verification uses the shared working tree with its unrelated changes; its R1 source hashes and surgical patch are recorded separately.

The new driver parity test compares actual subcatchment infiltration volume with a mesh cell under matched wet–dry–wet forcing, ponding and both project unit systems. The five existing subcatchment methods use the runoff driver; CONSTANT, which has no legacy subcatchment input token, uses the shared single-element boundary and a hand-computed physical rate. Each comparison has a 1e-12 m depth tolerance. Additional gates cover external ownership, continuation from packed state, point snow balance, assignment clearing/save/reopen, rename and lifecycle rejection.

Results and limitations are updated after the final runs; do not treat intermediate failed fixture/build logs as acceptance evidence.


## P0a accepted results

- Five unchanged engine suites plus the new seven-case surface contract suite pass (135 test cases passed, one existing integration case skipped).
- Registered corpus: 25/25 `.out` files byte-identical, including degree-day snow, water age and heat. Separate finite mesh census: 7 identical; 3 fixtures produce no output on either side and are explicitly skipped.
- Twelve additional running API fixtures (six infiltration methods, US/SI) produce byte-identical full wet–dry–wet trajectories: elapsed time, per-cell surface depth, held capacity, actual cumulative infiltration and total infiltration volume. Inputs, library hashes, harness and results are retained under `api_census/`.
- GUI property assignment, pending mesh fields and attribute-table suites pass. Assignment/clear/rename use the same engine record; the modal picker is exercised through its Use Snow Pack button. A rendered table is saved under `tests/gui/data/surface_r1_out/`.

P0b and the aquifer/UEB rounds remain subsequent work. These finite checks establish the stated refactor gates; they do not validate new aquifer physics or UEB.
