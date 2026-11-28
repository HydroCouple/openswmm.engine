# Mixed-flow closures against the laboratory record: TPA, the Preissmann slot and unsteady friction in both solvers

**Date:** 2026-11-25
**Issue:** openswmm.engine #156, Phase 6 (comparison matrix) and Phase 0 (the re-runnable study folder)
**Engine:** `swmm6_rel` at `26e23cc5`, Darwin preset (clang, arm64, Release), one engine run at a time
**Plan of record:** `plans/MIXED_FLOW_CLOSURES_TPA_UF_PLAN_2026-08-29.md` Parts IV-V
**Study folder:** `studies/mixed_flow_closures/` (untracked by design; rebuilt this round, see §2)

This memo is the Phase 6 deliverable the plan names ("results memo to
`docs/analysis/`"). A first scoring round ran on 2026-08-31 (recorded in
`plans/HANDOFF_P6_STUDY_SCORING_2026-08-31.md`); its untracked study folder
did not survive on the owner's machine, only its E2/E3 decks and five
digitized series did (`docs/figures/decks/rapid_fill/`,
`docs/figures/decks/negative_pressure/`), and the engine has since taken the
fixes that round motivated (R1 `1f9ad3af` VJ seeding, R3 `2cba7d06` loud
divergence, the 2026-09-12 slot wave-speed bound `2717af76`, the TPA
continuity fix in the dynamic wave solver). Every number below is measured on
`26e23cc5`; where the 2026-08-31 round is quoted it is labelled as such.

---

## 1. What was compared

Thirteen configurations ("columns") on five laboratory experiments, their
virtual-junction-spliced variants, one numerical study and six engine
fixtures. The plan's eleven columns are C1-C11; C3i and C5i (TPA with the
implicit pressurized head update, with and without unsteady friction) are the
out-of-plan configuration the 2026-08-31 round found to carry the
sub-atmospheric result and are kept as columns.

| Column | Solver | Closure | Pressurized update | Unsteady friction |
|---|---|---|---|---|
| C1 | FV | slot | explicit | off |
| C2 | FV | slot | implicit | off |
| C3 | FV | TPA | explicit | off |
| C3i | FV | TPA | implicit | off |
| C4 | FV | slot | implicit | on |
| C5 | FV | TPA | explicit | on (Pinto et al.'s SVUF) |
| C5i | FV | TPA | implicit | on |
| C6 | DW | EXTRAN | | off |
| C7 | DW | slot | | off |
| C8 | DW | dynamic slot | | off |
| C9 | DW | TPA | | off |
| C10 | DW | TPA | | on |
| C11 | DW | slot | | on |

| Experiment | Source | Decisive measurement |
|---|---|---|
| E1 | Soares et al. (2015) reservoir-pipe-valve water hammer, copper pipe D = 20 mm, L = 15.22 m, a = 1255 m/s, Q0 = 0.133 L/s (via Pinto et al. 2025) | valve pressure, 0-4 s: first peak 99.3 m (= 45.15 + aV/g), decay of the 4L/a oscillation |
| E2 | Vasconcelos, Wright and Roe (2006) rapid filling, acrylic pipe D = 94 mm, L = 14.33 m, 0.073 m at rest, 3.1 L/s admitted; run at the 2006 numerics (a = 25 m/s, n = 0.012) and at Pinto et al.'s (a = 150 m/s, n = 0.009) | pressure and velocity at 9.9 m, surge-tank level; bore arrival at 9.9 m (0.15 m crossing, measured 6.69 s) |
| E3 | Vasconcelos et al. (2006) siphon: the E2 pipeline raised 0.15 m at its center, primed to 0.30 m, 0.45 L/s withdrawn; a = 300 m/s | pressure at 14.1 m and velocity at 9.9 m over 0-40 s; the crest goes sub-atmospheric with no transducer on it |
| E4 | Aureli et al. (2015) V-shaped pipe D = 192 mm: 7.00 m at 8.4 % down, 5.12 m at 27.7 % adverse, gate at 5.00 m, dam-break from a 0.561 m head (via Pinto et al. 2025; a = 50 m/s) | pressure at 3.00 m and 8.52 m, 0-30 s |
| E5 | Vasconcelos et al. (2006) Fig. 7 numerical study | head profile along 8-13 m at t = 7.0 s versus grid and celerity |
| E6 | in-tree slot-program fixtures and `slot_surcharge.inp` | completion, continuity, wall time |

Scores are Pinto et al. (2025) Eqs. 14-15 (NSE and RMSE) on the digitized
series resampled at the observation times; bore arrival is an absolute
threshold crossing shared by observation and every column; E1 is scored on
the 2.0-2.4 s point-wise inset and on the per-cycle peak/trough envelope with
a fitted decay time constant.

## 2. The study folder, rebuilt

`studies/mixed_flow_closures/` runs from any checkout given `OPENSWMM_BIN`:
`config/matrix.yaml` (columns, experiments, sweeps), `decks/` (E1-E5 and the
`_vj` variants hand-written, E6 byte copies), `data/observed/` (nine series
with `provenance.md`), `scripts/run_matrix.py` (patches `[OPTIONS]` and
conduit `N`, runs serially, scans the report, scores), `scripts/metrics.py`,
`scripts/outfile.py` (a self-contained `.out` reader, checked frame-for-frame
against the Python package's reader), `scripts/plots.py`,
`scripts/digitize/` (the figure digitizer). The README documents the quick
start.

Decks and series inherited from the 2026-08-31 round: E2 (2006 numerics) and
E3 are the committed application-manual decks (`REPORT_STEP` respelled as a
fractional clock, see §6), with their five digitized series. Rebuilt this
round: E1, E4, E2 at Pinto's numerics, E5 and every `_vj` variant; the E1
and E4 series were re-digitized from the native-resolution raster images
embedded in the publisher PDFs (gray-level separation for Pinto's figures;
calibration from the tick marks; provenance and cross-checks in
`data/observed/provenance.md`: the E1 first peak reads 99.3 m against the
Joukowsky prediction 99.3 m, the oscillation period gives a = 1255 m/s).

Two boundary choices were measured before the matrix ran and are recorded in
the deck headers:

- **E1's valve.** An orifice valve closed by a control rule produces no water
  hammer in the finite-volume solver: a node with a structure attached is a
  storage node of area `MIN_SURFAREA`, and the head rose 0.6 m instead of
  54 m. The deck of record closes the valve as a sealed one-conduit dead end
  with the steady flow already seeded, so closure is t = 0; such a dead end is
  transparent and its `MIN_SURFAREA` is irrelevant (identical traces at
  1e-8, 1e-6 and 3e-4 m²).
- **E4's gate.** A single node cannot hold a pressurized upstream state and a
  dry downstream state, so the gate is a full-section orifice (Cd 1.0) between
  a sealed upstream-side node and a dry node; the dam-break release through
  an orifice law is a boundary-local approximation and is scored as such.

Deck facts found on the way: `[VIRTUAL_JUNCTIONS]` rows need an elevation
token (a bare name is "undefined object" at the conduit, where the input
reference says one token per line); a bare decimal `REPORT_STEP` token is
read as hours, so the two committed decks had a 3-minute report step.

## 3. Results

Status of the 156 base cells on `26e23cc5`: every cell completes
(0 FAIL, 0 SKIP); 14 cells complete with a trace that leaves the physical
range for a step or two (the `spike` note: dynamic-wave TPA and dynamic-slot
pressurization spikes at a virtual junction, and dynamic-wave columns on the
E4 dam-break); E3's explicit slot C1, an expected failure in the 2026-08-31
round, completes. Full record: `studies/mixed_flow_closures/results/summary.csv`;
overlays in `results/figures/`.

### 3.1 E3 — the siphon: the closure's payoff

Only TPA tracks the measured decline at 14.1 m, and only TPA carries the crest
below atmospheric pressure. The TPA columns' 4 mm RMSE is below the ±8 mm
resolution of the source figure. The dynamic-wave TPA port (C9/C10) is the
best dynamic-wave column (NSE 0.57, crest 0.031 m above the invert, where the
other dynamic-wave columns sit 0.09 m above it); in the 2026-08-31 round every
dynamic-wave column scored −0.5 to −0.6 — the TPA continuity fix in the dynamic
wave solver since then is what moved it. The explicit slot C1 completes but
oscillates spuriously about the record (NSE 0.69 after the F1 seeding fix,
0.32 before it; it failed loud at t = 0 on 2026-08-31, before the §8.5.3 slot
wave-speed bound) and never goes below atmospheric. Unsteady friction is
inert here (velocities of a few cm/s). Table re-run after the F1 fix; the
TPA columns moved by under 0.001.

| column | NSE p141 | NSE v99 | crest min gauge head (m) | note |
|---|---|---|---|---|
| C1 | 0.691 | -3.463 | 0.045 |  |
| C2 | 0.548 | 0.278 | 0.044 |  |
| C3 | 0.996 | 0.815 | -0.040 |  |
| C3i | 0.996 | 0.827 | -0.039 |  |
| C4 | 0.545 | 0.269 | 0.044 |  |
| C5 | 0.996 | 0.770 | -0.040 |  |
| C5i | 0.996 | 0.798 | -0.040 |  |
| C6 | -0.516 | -0.787 | 0.090 |  |
| C7 | -0.628 | -3.775 | 0.089 |  |
| C8 | -0.614 | -4.044 | 0.088 |  |
| C9 | 0.569 | -2.061 | 0.031 |  |
| C10 | 0.563 | -2.342 | 0.030 |  |
| C11 | -0.631 | -3.438 | 0.089 |  |

Crest gauge head at 10 / 20 / 30 / 40 s: C3 0.078 / 0.054 / 0.011 / −0.027 m,
C3i 0.105 / 0.057 / 0.010 / −0.038 m, C2 0.105 / 0.072 / 0.057 / 0.044 m,
C9 0.116 / 0.083 / 0.052 / 0.031 m. The vacuum is still deepening at 40 s.

### 3.2 E2 — the filling bore

Every finite-volume column puts the bore (0.15 m crossing at 9.9 m) within
0.06 s of the measured 6.69 s at the 2006 numerics (crown crossing 5.82 s
against 6.26 s measured) and scores the pressure trace at NSE 0.92–0.93, the
surge-tank level at 0.85–0.86 and the velocity at 0.54–0.62; the surge-tank
first peak is over-predicted by 5–11 % (0.42–0.45 m against 0.40 m), as the
2006 paper's own model was (7 %), and the post-shock oscillations behind the
bore over-predict the 10–15 s pressure rise by 0.03–0.05 m (the 2026-08-31
round's F5). At Pinto et al.'s numerics (a = 150 m/s, dx = 0.094 m) the
explicit columns arrive 0.9 s early and the implicit ones 0.3 s early at the
same pressure score, and the velocity score drops. Slot and TPA are
indistinguishable above the crown, as designed. Unsteady friction moves the
scores by hundredths (C5 is the best velocity and surge-tank score, by 0.01–0.02).
The dynamic-wave columns arrive 1.0–7.7 s late; TPA and the dynamic slot pass
a one- or two-step spike of 12–19 m of head through the virtual junction at
the bore, which destroys their pressure score and leaves the surge-tank score
(0.73–0.79) intact. The 2026-08-31 numbers for the finite-volume columns
(bore 24–25 s, pressure unscoreable) were the R1 seeding defect, since fixed.

| column | NSE p99 | NSE st | NSE v99 | bore (s) | surge-tank damping ratio | note |
|---|---|---|---|---|---|---|
| C1 | 0.934 | 0.846 | 0.599 | 6.753 | 0.81 |  |
| C2 | 0.916 | 0.850 | 0.542 | 6.041 | 0.78 |  |
| C3 | 0.933 | 0.846 | 0.600 | 6.701 | 0.81 |  |
| C3i | 0.932 | 0.853 | 0.575 | 6.018 | 0.79 |  |
| C4 | 0.922 | 0.849 | 0.545 | 6.098 | 0.77 |  |
| C5 | 0.927 | 0.860 | 0.624 | 6.018 | 0.78 |  |
| C5i | 0.931 | 0.853 | 0.578 | 6.075 | 0.78 |  |
| C6 | -0.664 | -0.102 | -0.870 | 14.372 | 0.77 |  |
| C7 | 0.424 | 0.345 | -1.289 | 9.983 | 0.72 |  |
| C8 | -27.823 | 0.746 | -0.520 | 7.701 | 0.73 | spike |
| C9 | -64.465 | 0.734 | -0.575 | 8.001 | 0.74 | spike |
| C10 | -0.387 | 0.637 | -1.014 | 8.302 | 0.73 |  |
| C11 | 0.404 | 0.318 | -1.310 | 10.048 | 0.63 |  |

| column | NSE p99 | NSE st | NSE v99 | bore (s) | surge-tank damping ratio | note |
|---|---|---|---|---|---|---|
| C1 | 0.935 | 0.702 | 0.293 | 5.791 | 0.87 |  |
| C2 | 0.837 | 0.669 | 0.040 | 6.388 | 0.83 |  |
| C3 | 0.935 | 0.702 | 0.293 | 5.792 | 0.87 |  |
| C3i | 0.912 | 0.674 | 0.067 | 6.347 | 0.84 |  |
| C4 | 0.860 | 0.593 | -0.135 | 6.689 | 0.82 |  |
| C5 | 0.922 | 0.730 | 0.333 | 5.956 | 0.86 |  |
| C5i | 0.904 | 0.660 | 0.049 | 6.440 | 0.84 |  |
| C6 | -24.631 | 0.013 | -0.775 | 10.901 | 0.85 | spike |
| C7 | 0.430 | 0.424 | -1.465 | 9.841 | 1.23 |  |
| C8 | -6.255 | 0.786 | -0.619 | 7.652 | 0.81 | spike |
| C9 | 0.749 | 0.787 | -0.659 | 8.853 | 0.82 |  |
| C10 | -23.853 | 0.492 | -2.006 | 7.951 | 0.84 | spike |
| C11 | 0.409 | 0.390 | -1.435 | 9.907 | 0.48 |  |

Virtual-junction composition (§2.2(6) of the plan): splicing the 9.9 m reach
at 3.0 and 6.0 m changes no finite-volume score by more than 0.004 and no
finite-volume bore arrival by more than 0.04 s (`e2_rapid_fill_2006_vj`);
the dynamic-wave columns move by 0.2–2.8 s and their spikes change station.

### 3.3 E1 — water hammer

The tables below are the matrix as re-run after the seeding fix of finding
F1 (the first run's single-conduit cells overshot the Joukowsky peak by 43 %;
see F1). On both decks the explicit columns land on the peak (99.4–99.5 m
against 99.3 m measured and predicted) and the implicit ones overshoot it by a
third (133 m); every finite-volume column then damps the 4L/a oscillation
about twice as fast as the apparatus at the paper's 1 m cells and the
matrix's Courant number of 0.66 (decay time constant 0.67–0.73 s explicit,
1.2 s implicit, against 1.43 s measured), so by 2 s the simulated peaks are at
46–48 m against 58 m measured and the envelope scores 0.14–0.57. The probes
of §4.4 show the case is reproduced on a converged mesh: at 0.25 m cells and
Courant 0.9, TPA without unsteady friction is under-damped (τ 3.2 s) and TPA
with k3 = 0.015 matches the record (τ 1.18–1.25 s, envelope NSE 0.983–0.994,
inset 0.56–0.64) — the paper's own finding. Below the crown the slot floors
the troughs at the pipe crown (0.02 m) while TPA carries them to −8 to −11 m
against −7 m measured. The dynamic wave solver does not represent water
hammer: the slot columns give a 0.4 m rise and EXTRAN/TPA pin the sealed dead
end at its 200 m surcharge cap.

| column | NSE env_peak | NSE env_trough | NSE p_valve | decay τ (s) | note |
|---|---|---|---|---|---|
| C1 | 0.126 | 0.381 | 0.056 | 0.65 |  |
| C2 | -0.751 | -0.450 | -0.006 | 1.12 |  |
| C3 | 0.137 | 0.401 | 0.055 | 0.65 |  |
| C3i | -0.758 | -0.441 | -0.006 | 1.12 |  |
| C4 | -0.811 | -0.498 | -0.006 | 1.19 |  |
| C5 | -0.101 | 0.172 | 0.015 | 0.70 |  |
| C5i | -0.833 | -0.512 | -0.006 | 1.20 |  |
| C6 | -97.812 | -136.519 | -392.427 | — |  |
| C7 | -1.681 | -1.346 | -0.008 | — |  |
| C8 | -1.636 | -1.383 | -0.001 | — |  |
| C9 | -97.812 | -136.519 | -392.427 | — |  |
| C10 | -97.812 | -136.519 | -392.427 | — |  |
| C11 | -1.681 | -1.346 | -0.008 | — |  |

| column | NSE env_peak | NSE env_trough | NSE p_valve | decay τ (s) | note |
|---|---|---|---|---|---|
| C1 | 0.478 | 0.578 | -0.267 | 0.72 |  |
| C2 | -0.684 | -0.572 | -0.006 | 1.10 |  |
| C3 | 0.569 | 0.656 | -0.251 | 0.72 |  |
| C3i | -0.674 | -0.560 | -0.006 | 1.10 |  |
| C4 | -0.738 | -0.622 | -0.006 | 1.16 |  |
| C5 | 0.244 | 0.365 | -0.059 | 0.70 |  |
| C5i | -0.730 | -0.612 | -0.006 | 1.16 |  |
| C6 | -32.252 | -58.950 | -149.563 | — |  |
| C7 | -1.665 | -1.360 | -0.006 | — |  |
| C8 | -1.665 | -1.361 | -0.008 | 6.97 |  |
| C9 | -32.252 | -58.950 | -149.563 | — |  |
| C10 | -33.425 | -60.461 | -154.122 | — |  |
| C11 | -1.665 | -1.359 | -0.006 | — |  |

**Where the single-conduit overshoot comes from (probes in
`results/_smoke/e1_P*.inp`).** It is not the initial head gradient (a
uniform 45.11 m start still peaks at 142 m), not the cell size (64 cells of
0.25 m: 144 m), not the seeded flow (with Q0 = 0 and the 0.26 m head
difference alone the first peak is still 91 m). The decisive probe is the
*static* pipe: reservoir and valve node both at 45.11 m, Q0 = 0, nothing to
move — and the sealed dead-end junction still launches a ±45 m transient
(45.11 → 90.0 m at 27 ms → the crown → back, while the link reports zero
flow). The same static pipe with one virtual junction at mid-length, or with
the dead end as a storage node of 3e-4 m², stays at 45.11 m for the whole
run. A sealed one-conduit dead-end junction attached to a conduit whose other
end is a real node is therefore mis-initialized by the finite-volume solver;
with the splice in place the first peak is the Joukowsky peak. This is an
engine defect, recorded for follow-up (finding F1), not fixed here; the
spliced deck's numbers are the ones that describe the physics.

### 3.4 E4 — the V-shaped pipe

The explicit slot C1 is the only column that scores both stations positively:
first slosh peak and timing at 8.52 m (NSE 0.34; bore 2.20 s against 2.00 s
measured) and the upstream drain-down at 3.00 m (0.35). TPA scores the
upstream station negative (−1.6 explicit, −1.8 implicit) and the adverse leg
at −0.03 / −0.9: once the upstream pool has drained through the gate, the
return surges on the pressurized reach ring at the acoustic celerity where
the record shows a smooth slosh, and the implicit columns oscillate about the
slosh peaks. Venting the pipe ends (plain junctions at both ends, as the
apparatus is open to the atmosphere) moved the implicit TPA upstream score
from −35.8 (sealed ends: a vacuum at the capped end the apparatus cannot have)
to −1.8 and is the deck of record. Unsteady friction at Pinto et al.'s
calibration (n = 0.007, k3 = 0.005) degrades every finite-volume column
(C5 p852 −4.9: after 15 s the SVUF column settles at 0.5 m of head where the
record is dry). The dynamic wave solver cannot run the dam-break: EXTRAN, the
dynamic slot and both unsteady-friction columns spike (continuity 20–150 %),
the static slot delivers the first slosh 2.2 s late at a third of its height,
and DW TPA never brings water past the 0.02 m threshold at 8.52 m. Splicing
two more virtual junctions into the legs changes the finite-volume bore
arrivals by ≤ 0.2 s and the scores by up to 0.3 — the dam-break's chaotic
post-peak oscillations are sensitive to the mesh in a way the smooth E2 and
E3 solutions are not.

| column | NSE p300 | NSE p852 | bore (s) | note |
|---|---|---|---|---|
| C1 | 0.353 | 0.335 | 2.197 |  |
| C2 | 0.308 | -1.380 | 2.403 |  |
| C3 | -1.593 | -0.029 | 2.262 |  |
| C3i | -1.815 | -0.915 | 2.483 |  |
| C4 | 0.374 | -1.222 | 2.284 |  |
| C5 | -1.736 | -4.945 | 2.168 |  |
| C5i | -1.751 | -0.311 | 2.408 |  |
| C6 | -0.025 | -271.846 | 4.470 | spike |
| C7 | -0.251 | -0.602 | 4.244 |  |
| C8 | -0.040 | -5.39e+04 | 3.811 | spike |
| C9 | -0.031 | -1.059 | — |  |
| C10 | -0.041 | -7.83e+04 | 3.502 | spike |
| C11 | -0.267 | -56.588 | 3.032 | spike |

| column | NSE p300 | NSE p852 | bore (s) | note |
|---|---|---|---|---|
| C1 | 0.349 | 0.341 | 2.194 |  |
| C2 | -0.035 | -8.842 | 2.205 |  |
| C3 | -0.583 | 0.085 | 2.239 |  |
| C3i | -0.735 | -0.674 | 2.323 |  |
| C4 | 0.270 | -1.362 | 2.068 |  |
| C5 | -0.706 | -4.772 | 2.148 |  |
| C5i | -0.705 | -0.741 | 2.217 |  |
| C6 | -0.025 | -407.559 | 4.477 | spike |
| C7 | -0.127 | -3.900 | 4.208 |  |
| C8 | -0.068 | -2.88e+05 | 3.877 | spike |
| C9 | -0.169 | -1.042 | — |  |
| C10 | -0.179 | -1.070 | — |  |
| C11 | -0.141 | -149.108 | 3.013 | spike |

### 3.5 E5 — the Vasconcelos et al. (2006) numerical study

At t = 7.0 s on the 2006 numerics (a = 25 m/s, 400 cells) the pressurization
front stands at x = 12.27 m for the explicit columns and 11.87 m for the
implicit slot (threshold 0.11 m between the undisturbed 0.073 m and the
filled ~0.15 m), where the paper's own model (Fig. 7b, a = 25 m/s) shows it
near 11.5 m and near 11.7 m at a = 50–100 m/s. The paper's model reaches the
9.9 m station ~0.4 s earlier than the experiment; ours reaches it on the
measured time (§3.2), so the comparison is between two models, not against
the record. Across the celerity sweep (25–300 m/s) the explicit columns' front stays
at 12.27–12.33 m; the implicit slot's moves back from 11.87 m (25 m/s) to
10.92 m (150 m/s) and at 300 m/s its 7 s profile has no front above the
threshold.

| column | front x (m) | status |
|---|---|---|
| C1 | 12.27 | OK |
| C2 | 11.87 | OK |
| C3 | 12.27 | OK |
| C3i | 12.28 | OK |

### 3.6 E6 — engine fixtures

All 48 fixture cells complete; the finite-volume columns close continuity at
0.000 % on every fixture, the dynamic-wave columns at −0.8 to +2.3 %.


## 4. Sensitivity sweeps

All 400 sweep runs complete (k3: 200, n: 125, celerity: 75); the `spike`
notes are the dynamic-wave pressurization spikes of §3.2 and the E4
dam-break. Tables are NSE of the experiment's decisive series; the best
value per column is bold.

### 4.1 Unsteady friction coefficient k3 (0–0.045; columns C4, C5, C5i, C10, C11)

E1: every k3 > 0 lowers the envelope score of the SVUF column (0.137 at 0,
−0.10 at the calibrated 0.015, −0.41 at 0.045) — the numerical damping of
§3.3 already exceeds the measured one, so the term can only make it worse.
E2: the finite-volume scores move by hundredths across the whole grid; the
surge-tank score of the SVUF column rises monotonically (0.846 → 0.869 at
the 2006 numerics, 0.702 → 0.734 at Pinto's), the pressure score falls
slightly (0.935 → 0.898 at Pinto's numerics), so the calibrated 0.020
transfers as a mild, mixed effect. E4: non-monotone (C5 at 8.52 m: −5.0 at
0, +0.24 at 0.025–0.030, −0.86 at 0.045; C4 swings between −1.2 and −10),
the "knife edge, not a threshold" the 2026-08-31 round recorded. The
dynamic-wave TPA + UF column spikes at most k3 values; the slot + UF column
falls monotonically with k3.

| k3 | C4 | C5 | C5i | C10 | C11 |
|---|---|---|---|---|---|
| 0 | **-0.751** | **0.137** | **-0.758** | -97.812 | -1.681 |
| 0.005 | -0.773 | 0.042 | -0.778 | -97.812 | -1.681 |
| 0.01 | -0.792 | -0.020 | -0.814 | -97.812 | -1.681 |
| 0.015 | -0.811 | -0.101 | -0.833 | -97.812 | -1.681 |
| 0.02 | -0.830 | -0.170 | -0.840 | -97.812 | -1.681 |
| 0.025 | -0.847 | -0.218 | -0.866 | -97.812 | -1.681 |
| 0.03 | -0.863 | -0.274 | -0.891 | -97.812 | -1.681 |
| 0.035 | -0.878 | -0.326 | -0.902 | -97.812 | -1.681 |
| 0.04 | -0.892 | -0.372 | -0.919 | -97.812 | -1.681 |
| 0.045 | -0.905 | -0.413 | -0.921 | **-97.812** | **-1.681** |

| k3 | C4 | C5 | C5i | C10 | C11 |
|---|---|---|---|---|---|
| 0 | 0.916 | **0.933** | 0.932 | -64.465 (spike) | **0.424** |
| 0.005 | 0.921 | 0.930 | **0.933** | -2.555 (spike) | 0.419 |
| 0.01 | 0.916 | 0.929 | 0.931 | -7.752 (spike) | 0.414 |
| 0.015 | 0.916 | 0.929 | 0.930 | **0.774** | 0.409 |
| 0.02 | **0.922** | 0.927 | 0.931 | -0.387 | 0.404 |
| 0.025 | 0.920 | 0.927 | 0.929 | 0.704 | 0.399 |
| 0.03 | 0.915 | 0.926 | 0.929 | 0.655 | 0.393 |
| 0.035 | 0.915 | 0.926 | 0.926 | -47.775 (spike) | 0.388 |
| 0.04 | 0.918 | 0.924 | 0.929 | 0.716 | 0.383 |
| 0.045 | 0.920 | 0.925 | 0.928 | -13.085 (spike) | 0.378 |

| k3 | C4 | C5 | C5i | C10 | C11 |
|---|---|---|---|---|---|
| 0 | **0.850** | 0.846 | 0.853 | 0.734 (spike) | **0.345** |
| 0.005 | 0.848 | 0.850 | 0.853 | 0.705 (spike) | 0.337 |
| 0.01 | 0.849 | 0.853 | **0.855** | **0.742 (spike)** | 0.331 |
| 0.015 | 0.847 | 0.857 | 0.855 | 0.645 | 0.324 |
| 0.02 | 0.849 | 0.860 | 0.853 | 0.637 | 0.318 |
| 0.025 | 0.846 | 0.862 | 0.854 | 0.586 | 0.311 |
| 0.03 | 0.840 | 0.864 | 0.853 | 0.542 | 0.304 |
| 0.035 | 0.842 | 0.866 | 0.854 | 0.567 (spike) | 0.297 |
| 0.04 | 0.839 | 0.868 | 0.852 | 0.479 | 0.289 |
| 0.045 | 0.836 | **0.869** | 0.850 | 0.471 (spike) | 0.282 |

| k3 | C4 | C5 | C5i | C10 | C11 |
|---|---|---|---|---|---|
| 0 | 0.837 | **0.935** | **0.912** | 0.749 | **0.430** |
| 0.005 | 0.807 | 0.932 | 0.907 | -24.287 (spike) | 0.425 |
| 0.01 | 0.784 | 0.932 | 0.909 | 0.656 | 0.420 |
| 0.015 | 0.815 | 0.932 | 0.904 | -48.804 (spike) | 0.415 |
| 0.02 | 0.860 | 0.922 | 0.904 | -23.853 (spike) | 0.409 |
| 0.025 | **0.879** | 0.920 | 0.906 | -20.211 (spike) | 0.404 |
| 0.03 | 0.801 | 0.918 | 0.909 | **0.823** | 0.399 |
| 0.035 | 0.828 | 0.910 | 0.907 | 0.794 | 0.394 |
| 0.04 | 0.871 | 0.909 | 0.900 | -81.397 (spike) | 0.388 |
| 0.045 | 0.823 | 0.898 | 0.899 | -2.805 (spike) | 0.383 |

| k3 | C4 | C5 | C5i | C10 | C11 |
|---|---|---|---|---|---|
| 0 | -1.417 | -5.049 | -0.518 | **-1.046** | **-0.520** |
| 0.005 | -1.222 | -4.945 | **-0.311** | -7.83e+04 (spike) | -56.588 (spike) |
| 0.01 | -9.996 | -4.688 | -1.280 | -1.072 | -1.053 |
| 0.015 | -9.857 | -0.006 | -0.579 | -1.073 | -1.068 |
| 0.02 | -9.842 | 0.082 | -5.613 | -1.073 | -1.070 |
| 0.025 | **-1.181** | 0.242 | -0.530 | -1.072 | -1.072 |
| 0.03 | -1.733 | **0.242** | -0.405 | -1.073 | -1.073 |
| 0.035 | -9.790 | -0.812 | -3.607 | -1.073 | -1.074 |
| 0.04 | -9.875 | -0.824 | -0.634 | -1.071 | -1.075 |
| 0.045 | -1.807 | -0.857 | -3.292 | -1.070 | -1.075 |

### 4.2 Manning n (0.007–0.013; columns C2, C3, C5, C7, C9)

E2 improves monotonically with n up to the grid's top value 0.013 — past the
2006 paper's 0.012 and Pinto's 0.009 — with or without unsteady friction
(C3 pressure 0.896 → 0.938, surge tank 0.641 → 0.875 at the 2006 numerics;
C5 0.934 / 0.885 at 0.013): the simulated oscillation is under-damped and
asks for more friction than the acrylic pipe has, which is the dilemma
unsteady friction is meant to resolve and here does not. E4's adverse-leg
score rises the same way (C3 −0.03 at 0.009 → 0.57 at 0.013; C5 0.12 →
0.53). E3 is insensitive (the siphon's velocities are centimeters per
second) and E1 prefers the lowest n by hundredths.

| n | C2 | C3 | C5 | C7 | C9 |
|---|---|---|---|---|---|
| 0.007 | 0.889 | 0.896 | 0.893 | 0.424 | **0.822** |
| 0.009 | 0.897 | 0.915 | 0.908 | **0.430** | 0.276 |
| 0.011 | 0.912 | 0.929 | 0.921 | 0.428 | -0.483 |
| 0.012 | 0.916 | 0.933 | 0.927 | 0.424 | -64.465 (spike) |
| 0.013 | **0.925** | **0.938** | **0.934** | 0.417 | -47.358 (spike) |

| n | C2 | C3 | C5 | C7 | C9 |
|---|---|---|---|---|---|
| 0.007 | 0.688 | 0.641 | 0.683 | **0.466** | 0.799 |
| 0.009 | 0.761 | 0.732 | 0.762 | 0.424 | **0.843** |
| 0.011 | 0.822 | 0.812 | 0.830 | 0.372 | 0.758 |
| 0.012 | 0.850 | 0.846 | 0.860 | 0.345 | 0.734 (spike) |
| 0.013 | **0.873** | **0.875** | **0.885** | 0.318 | 0.758 (spike) |

| n | C2 | C3 | C5 | C7 | C9 |
|---|---|---|---|---|---|
| 0.007 | -1.417 | -5.049 | -4.945 | **-0.520** | -1.046 |
| 0.009 | -1.380 | -0.029 | 0.118 | -0.602 | -1.059 |
| 0.011 | -0.828 | 0.346 | 0.382 | -0.714 | **-1.016** |
| 0.012 | **-0.487** | 0.397 | 0.496 | -0.761 | -1.034 |
| 0.013 | -0.796 | **0.566** | **0.534** | -0.808 | -1.023 |

### 4.3 Celerity (25–300 m/s on E2 and E5; columns C1, C2, C3, C5, C8, C9)

The explicit finite-volume columns complete at every celerity and the
pressure score rises slightly with it (C3: 0.933 at 25 m/s → 0.946 at 150,
0.930 at 300); the surge-tank score prefers the 2006 paper's 25 m/s. The
high-celerity filling divergence that pinned the 2026-08-31 round is gone
(the §8.5.3 wave-speed bound): C1/C3/C5 at 300 m/s score 0.86–0.94. The
implicit slot degrades above 50 m/s (0.835 at 100) and spikes at 300 m/s;
the dynamic-wave TPA is clean only at 50–150 m/s (0.79–0.80) and spikes at
25 and 300; the dynamic slot spikes at most values. Across the celerity sweep (25–300 m/s) the explicit columns' front stays
at 12.27–12.33 m; the implicit slot's moves back from 11.87 m (25 m/s) to
10.92 m (150 m/s) and at 300 m/s its 7 s profile has no front above the
threshold.

| celerity | C1 | C2 | C3 | C5 | C8 | C9 |
|---|---|---|---|---|---|---|
| 25 | 0.934 | 0.916 | 0.933 | **0.927** | -27.823 (spike) | -64.465 (spike) |
| 50 | 0.939 | **0.926** | 0.939 | 0.914 | -0.368 | 0.785 |
| 100 | 0.939 | 0.835 | 0.942 | 0.920 | **0.814** | 0.798 |
| 150 | **0.945** | 0.864 | **0.946** | 0.913 | -2.805 (spike) | **0.802** |
| 300 | 0.941 | -95.956 (spike) | 0.930 | 0.858 | -8.757 (spike) | -13.254 (spike) |

| celerity | C1 | C2 | C3 | C5 | C8 | C9 |
|---|---|---|---|---|---|---|
| 25 | 0.922 | **0.914** | 0.921 | 0.915 | 0.388 | 0.276 |
| 50 | 0.922 | 0.896 | 0.922 | 0.914 | -10.649 (spike) | -41.552 (spike) |
| 100 | 0.930 | 0.458 | 0.929 | **0.930** | -37.331 (spike) | **0.815** |
| 150 | 0.935 | 0.837 | **0.935** | 0.922 | -6.255 (spike) | 0.749 |
| 300 | **0.939** | -9.863 (spike) | 0.934 | 0.912 | **0.742** | -84.325 (spike) |

| celerity | C1 | C2 | C3 |
|---|---|---|---|
| 25 | 12.27 | 11.87 | 12.27 |
| 50 | 12.27 | 11.88 | 12.27 |
| 100 | 12.32 | 11.43 | 12.32 |
| 150 | 12.33 | 10.92 | 12.32 |
| 300 | 12.31 | — | 12.30 |

### 4.4 Numerics probes after the matrix (same day; records in `results/summary_probe_*.csv`)

Run on the engine with the F1 seeding fix, through the runner's `--set` overrides.

**Soares (E1) is reproduced on a converged mesh.** On either deck, with 0.25 m
cells, `FV_CFL 0.9` and the routing step equal to the Courant substep
(0.00017 s): TPA explicit without unsteady friction gives the Joukowsky peak and
an under-damped decay (τ 3.2 s against 1.43 s measured, envelope NSE −0.4 /
−0.3); TPA explicit with Vítkovský friction at the paper's k3 = 0.015 gives the
peak (99.5 m), τ 1.18–1.25 s and envelope NSE 0.983–0.994 on peaks and
troughs (0.56–0.64 point-wise on the 2.0–2.4 s inset). At the paper's 1 m
cells the scheme's own damping stands in for the term (Courant 0.9, no UF:
τ 1.01 s, envelope 0.95 / 0.97; with UF 0.60 / 0.68), and at the matrix's
Courant number of 0.66 it over-damps everything (§3.3). The issue's Soares
criterion (NSE ≥ 0.90) is therefore met where the mesh is converged, and the
unsteady-friction term does the job the paper assigns it.

| Numerics (e1_soares, TPA explicit) | k3 | first peak (m) | τ (s) | envelope NSE peaks / troughs |
|---|---|---|---|---|
| dx 1 m, Cr 0.66 (matrix) | 0 | 99.5 | 0.66 | 0.19 / 0.31 |
| dx 1 m, Cr 0.66 (matrix) | 0.015 | 99.5 | 0.71 | −0.05 / 0.10 |
| dx 1 m, Cr 0.9 | 0 | 99.5 | 1.01 | 0.95 / 0.97 |
| dx 1 m, Cr 0.9 | 0.015 | 99.5 | 0.77 | 0.60 / 0.68 |
| dx 0.25 m, Cr 0.9 | 0 | 99.5 | 3.18 | −0.38 / −0.32 |
| dx 0.25 m, Cr 0.9 | 0.015 | 99.5 | 1.18 | **0.983 / 0.992** |
| dx 0.25 m, Cr 0.9, spliced deck | 0.015 | 99.5 | 1.25 | **0.994 / 0.996** |

**A report step shorter than the routing step corrupts the output, not the
solution (F8, fixed).** The probes that first looked like a "several substeps
per routing step" blow-up (first peaks of 669–1495 m) all had `REPORT_STEP`
below `ROUTING_STEP`; with the two equal, or the routing step cut to the
Courant substep, every cadence gives 99.5 m, and the report file's own
routing-step maximum reads 99.48 m in every case. The engine emitted one
report per routing step, so a finer report grid fell behind the routing
clock and the output interpolation weight reached −20. The weight was first
bounded to [0, 1] (25b4cfa1); on the owner's decision the deck is now refused
at initialization with ERROR 195, as legacy refuses it (gate
`SentinelParityOptions.ReportStepShorterThanRoutingStepIs195`). The bound
stays for a report step lowered through the options API after initialization
(gate `FvEngine.LaggingReportsStayWithinTheRoutingStep`). A water-hammer deck
that needs fine reports sets `ROUTING_STEP` at or below `REPORT_STEP`.

**Aureli (E4) does not yield to numerics.** Local time stepping off, Courant
0.9 and 0.024 m cells give the best finite-volume result, the explicit slot at
NSE 0.37 / 0.38 with the slosh timing fixed (bore 2.08 s against 2.005 s);
local time stepping ON at Courant 0.9 degrades it (0.03 / −0.58, a spurious
rise at 0.6 s), second order hurts (0.06 / −3.0), and TPA stays negative on the
upstream station. The gap is the gate (an orifice law between a sealed and a
dry node) and amplitude damping, not the closure.

**E2 at the paper's Courant number** (0.95, one substep per routing step): the
bore arrives at 5.92 s against 6.69 s measured — the matrix's 6.70 s at
Courant 0.5 was partly dissipation slowing the bore; the pressure score (0.93)
does not move.

## 5. Findings

Against the issue's acceptance criteria:

| Criterion (issue #156) | Measured on 26e23cc5 |
|---|---|
| TPA-FV reproduces the 2006 rapid-filling experiment (bore arrival, head traces) | **Met.** Bore at 9.9 m 6.70 s against 6.69 s measured; pressure NSE 0.93, surge tank 0.85, velocity 0.60 (C3). |
| TPA sustains sealed sub-atmospheric reaches the slot closure cannot | **Met.** Crest 0.040 m below the invert at NSE 0.996 (RMSE 4 mm); every slot column bottoms 0.044–0.090 m above it. |
| UF reproduces Soares NSE ≥ 0.90 at the paper's (n, k3) | **Not met.** 0.015 point-wise, 0.14 / 0.40 on the envelope (single conduit); 0.24 / 0.37 (spliced). The solver damps the oscillation 2–3× faster than measured before any friction term acts (F2), and the single-conduit deck carries the dead-end defect (F1). |
| UF reproduces Vasconcelos Neto NSE ≥ 0.9 | **Met for pressure and surge tank** (0.93, 0.86), **not for velocity** (0.62 best). |
| UF reproduces Aureli NSE ≥ 0.8 | **Not met** by any column: best 0.35 / 0.34 (explicit slot without UF); the SVUF column scores −1.7 / −4.9. |
| Virtual-junction composition proven | **Met on E2** (splice changes scores ≤ 0.004, bores ≤ 0.04 s); on E4 the dam-break's post-peak oscillations move by up to 0.3 NSE; on E1 the splice *removes* the F1 artifact. |
| Study re-runs end-to-end with only `OPENSWMM_BIN` set | **Met** (this round ran it from scratch, 556 engine runs). |

Findings for the engine (none fixed here):

- **F1 — sealed dead-end initialization transient (finite volume).** A
  static, flow-free 45 m column with a sealed one-conduit dead-end junction
  launches a ±45 m transient (`results/_smoke/e1_P5_static.inp`); a
  virtual-junction splice or a storage-node dead end removes it. It doubles
  E1's first water-hammer peak on the single-conduit deck. **Cause and fix
  (same day):** `Router::initFv` seeded every conduit without a virtual
  junction from the link's initial depth, clamped to the full depth by
  legacy `initLinkDepths`, so the cells of a pressurized conduit started at
  the crown while its nodes stood on the slot line; the first node solve
  then walked the dead end down to its cells. Cells are now seeded from the
  uncapped end-node average whenever it lies above the crown; free-surface
  seeds are bit-identical (seven decks compared), the static column stays
  at rest, and the single-conduit deck gives 99.4 m. Gate
  `FvEngine.PressurizedDeadEndSeedsAtRest`.
- **F2 — water-hammer decay is over-damped.** Both closures, at the paper's
  dx = 1 m and Cr 0.9, damp the 4L/a oscillation with τ = 0.65–0.72 s
  (explicit) / 1.1–1.2 s (implicit) against 1.43 s measured, so the
  unsteady-friction term's effect on E1 is unobservable, and the k3 sweep is
  monotonically harmful there. The implicit update keeps more of the
  oscillation but overshoots the first peak by a third even on the spliced
  deck's clean start.
- **F3 — the E3 result stands on the current engine**, and the dynamic-wave
  TPA port is now the best dynamic-wave column there (0.57, crest 0.031 m
  above the invert), where the 2026-08-31 round had it at −0.5; the explicit
  slot completes instead of failing loud.
- **F4 — dynamic-wave pressurization spikes.** TPA and the dynamic slot pass
  12–19 m of head through a virtual junction for one or two report steps as
  a bore arrives (E2), and all dynamic-wave columns but the static slot and
  TPA spike on E4's dam-break with 20–150 % continuity error.
- **F5 — the finite-volume solutions want more friction than the
  apparatus.** E2's and E4's scores rise monotonically to the n grid's top
  value (0.013) with or without unsteady friction; E2's late pressure rise is
  over-predicted by 0.03–0.05 m and the surge-tank peak by 5–11 % (the 2006
  paper's own model: 7 %).
- **F6 — E4's dam-break is the hard case**: only the explicit slot is
  positive at both stations; TPA rings at the acoustic celerity after the
  upstream pool drains; unsteady friction at the calibrated values makes
  every column worse and its k3 response is non-monotone.
- **F7 — input reference mismatch**: `[VIRTUAL_JUNCTIONS]` rows need an
  elevation token (§6).
- **F8 — a report step shorter than the routing step extrapolates the
  output** (§4.4): the report grid fell behind the routing clock and the
  interpolation weight reached −20 (770 m in the `.out` for a 99.5 m
  solution). The weight is bounded, and such a deck is now refused at
  initialization with ERROR 195, as legacy refuses it. **Fixed (same day).**

## 6. Side findings (not fixed here)

- `docs/figures/decks/rapid_fill/rapid_fill.inp` and
  `docs/figures/decks/negative_pressure/negative_pressure.inp` carried
  `REPORT_STEP 0.05` / `0.02`, read as decimal hours since engine `63b59b41`:
  the application-manual figures that run them had one output frame.
  Respelled as `0:00:00.05` / `0:00:00.02` in this commit.
- The input reference's `[VIRTUAL_JUNCTIONS]` format ("Name", one token per
  line) does not match the parser, which rejects a row without an elevation
  token by reporting the conduit's node as undefined.

## 7. References

- Vasconcelos, J.G., Wright, S.J., Roe, P.L. (2006). Improved simulation of flow regime transition in sewers: two-component pressure approach. *J. Hydraul. Eng.* 132(6), 553-562.
- Pinto, S.I.G., Vasconcelos, J.G., Soares, A.K. (2025). Unsteady friction in mixed-flow models based on the Saint-Venant equations. *J. Hydraul. Eng.* 152(1), 04025046.
- Aureli, F., Dazzi, S., Maranzoni, A., Mignosa, P. (2015). Validation of single- and two-equation models for transient mixed flows: a laboratory test case. *J. Hydraul. Res.* 53(4), 440-451.
- Vítkovský, J.P., Lambert, M.F., Simpson, A.R., Bergant, A. (2000). Advances in unsteady friction modelling in transient pipe flow. *Proc. 8th Int. Conf. on Pressure Surges*, 471-482.
