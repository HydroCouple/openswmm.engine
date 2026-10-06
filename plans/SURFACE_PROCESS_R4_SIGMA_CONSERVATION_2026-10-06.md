# R4 SIGMA table/column conservation — 2026-10-06

Continue the [saved corrective phase](SURFACE_PROCESS_R4_AREA_ET_2026-10-06.md) under the user's “Proceed.” Parent `c5414e38` contains the reproduced 0.984802466 m³ closed-cell failure and the internal common-stage SIGMA restriction. Keep the updated node-storage LID retained/mobile partition and optional 1D Richards solver unchanged.

1. Preserve the failing moving-table cases as regression tests. Audit the combined saturated/unsaturated balance, including the lower-boundary availability share and collapsed-column cases. The existing post-sweep correction has no way to apply a negative correction when its saturated profile has zero storage derivative.
2. Settle the existing explicit SIGMA sweep and table against the same actual lower-boundary water transfer. Trial a bounded table height using the original column snapshot; accepted bottom flux plus compression surplus minus residual support must equal saturated storage change minus actual external saturated exchanges. Bracket the physical table interval, use a finite residual tolerance, and book saturation excess or physically unavailable withdrawal only at the settled boundary. Keep the existing soil laws, layer count, ET stress and interface kernels; add no numerical input or GUI control.
3. Verify closed and forced high-conductivity columns; full/near-zero column thickness; intake, soil ET, positive/negative node/lateral/link/source/deep terms; paired species and no node refunds; US/SI consistency; timestep refinement. Run the existing groundwater gates, ET/receiving/source-stage suites and updated node/Richards controls in an exact isolated source tree. Capture unchanged ordinary closures/no-aquifer controls and timing and the scalar iteration bound.
4. Remove the internal SIGMA refusal only after those checks pass. Keep production ownership activation gated by the remaining R4 inside/outside, generalized-store, transport, return/refund, restart/results and performance requirements.

This numerical correction uses the previously approved groundwater/LID parameters. It introduces no new GUI input or visualization decision. Any later UI additions still require concrete review before implementation. Test inputs, outputs, baseline and source hashes remain visible under `tests/verification/surface_r4_sigma_2026-10-06/`.

## Implemented correction and discrete balance

The original table predictor could reach the surface, leaving a fully saturated column with zero storage derivative. Its subsequent correction applied positive surplus as Dunne water but silently ignored negative surplus. That created water even in a closed cell with no ET. The correction keeps the existing explicit SIGMA/ALE column kernel, soil laws, capillary option and fixed layer count. It changes how the column and its moving table settle together; it does not replace SIGMA with Richards or modify node-storage LID physics.

For one trial height `h` let `L = zs - h`. Replay `advanceColumn` from the original layer snapshot at that `L`, with identical completed intake, ET, physical Darcy flux and step duration. Let `external` be actual lateral minus node plus link plus source-in minus source-out minus deep water, divided by cell area. Solve the water-depth residual

```
R(h) = theta_s * (h - hg0) - external
       - dt * (f_bot + overflow_to_sat - deficit_from_sat).
```

The column's own identity is `delta_hu = dt*(f_top - et_taken - f_bot - overflow + deficit)`. Adding both balances cancels every internal bottom transfer. Accepted top water and actual external exchanges alone change total storage. Every trial resets layer water; no forcing, receipt or ET history is booked twice. Only one settled profile is committed.

The specific-yield predictor is tried first. If it fails the depth residual tolerance `1e-15*max(1,zs,abs(external))`, the scalar solver brackets `[0,zs]` and uses up to 100 bisections, with explicit non-finite/non-convergence diagnostics. An upper-bound excess is routed through existing Dunne accounting. A lower-bound shortage follows the existing deep-first/node refund convention, correcting already booked actual rates/ledgers. Already accepted node receipts in all feasible signed tests require zero refunds. Unavailable withdrawals outside those refundable channels fail explicitly. The existing mass pass reads the final column swap; physical Darcy recharge remains an internal diagnostic.

## Qualified result

**298 cases pass in 15 isolated suites**, including 11 SIGMA cases, all 25 common-source-stage cases, 30 groundwater transport cases and all 53 updated node-storage/Richards controls. The 11 cases span all four soil laws, closed high-conductivity tables at zero/full/near-full thickness, ET, intake/rejection, both signs of node/link receipts, deep loss, capped pumping, signed source knots and paired inert species. Capillary-on tests cover 2/4/8/32 layers. Two cells exercise actual lateral transfer, mass and distinct timesteps, including water/mass still in face accumulators. A forced 16 s trajectory converges toward a 0.00625 s reference as steps shrink from 0.2 to 0.025 s. These are fixture-specific convergence checks, not a general convergence theorem.

The standalone 24-case probe reduces the maximum absolute water residual from **0.984802466 to 8.04e-15 m³**. All existing groundwater numerical gates pass. Six new regression cases failed against unmodified production parent `c5414e38` before the correction. The deep-drainage regression was subsequently improved to test actual limits and conservation rather than assuming a zero table remains empty after later internal recharge.

Sixteen CLOSED_FORM/ENSLAVED trajectories are byte-identical to an independently rebuilt exact-parent library. Node, hydraulic storage, semi-discrete Richards and `SigmaColumn.cpp` production files retain their parent hashes. No-aquifer behavior is unchanged by dispatch; existing inertial/no-aquifer controls pass. The qualified common stage can now attach SIGMA receivers, including moving-table completed owner/mesh ET. The production router activation gate remains present.

Eight-layer, single-cell timing medians range from about **0.99 to 4.17 times** the failing parent's time (about 0.56–5.70 microseconds per corrected cell firing). Five runs of 5000 forced 0.1 s firings cover two conductivities and four laws. The highest ratio occurs for Russo at `Ks=0.01 m/s`. These small timings compare different physical trajectories after a bug fix; they are neither full-model performance qualification nor a node Richards cost estimate. A consistency solve can replay the O(layers) explicit sweep multiple times; difficult cases therefore cost more than one sweep.

## Preserved review boundary and next phase

No new GUI input, numerical knob, output selector or executable deployment is introduced. Existing approved R4 area/contact ownership, component ET and potential/actual/unused/pending visualization designs apply. The updated partly submerged node-storage LID retained/mobile drainage and connected-cell routing are preserved.

R4 remains incomplete: partial inside/outside native area, generalized stores, constituent provenance/quality integration, durable returns/refunds, restart, saved results and broader timing/performance qualification precede runtime activation. The next tranche is partial inside/outside area and generalized native/sealed/covered LID footprints; any additional input or visualization decision must be prepared for review before implementation. R5 node Richards–aquifer bottom coupling and UEB remain later phases. [Visible evidence](../tests/verification/surface_r4_sigma_2026-10-06/README.md) records exact source hashes, failed-parent proof, final counts, convergence and costs.
