<!-- PR body for: reform/pr10-mc-validation → feature/uncertainty-sidecar
     Open at: https://github.com/HydroCouple/openswmm.engine/compare/feature/uncertainty-sidecar...reform/pr10-mc-validation
     (This file is committed for provenance; paste its contents as the PR description.) -->

# Uncertainty sidecar reform — PRs 1–10 (integration)

Full reform of the ROM uncertainty sidecar, fixing all six findings (F1–F6)
of the 2026-07-01 code review. 20 commits across 12 stacked branches
(`reform/pr1…pr10`, each individually pushed for slice-by-slice review if
wanted); this PR integrates the tip. **72/72 ctest gate.**

## What changed, by finding

| Finding | Fix | PR |
|---|---|---|
| F1 — total-head ROM + spread-collapsing reseed (quantiles were "spread regrown since last reseed") | **Deviation formulation**: state is δa = Pᵀ(h − h_det); the nominal member provably never leaves the deterministic run; `checkAndReseed()` deleted (1D) / reduced to wet-domain basis rebuild (2D) | 6, 7 |
| F2 — outfall edges deleted → uniform inputs produced zero spread | **Grounded Laplacian** (Dirichlet at outfall-adjacent rows) | 4 |
| F3 — "decorrelated" LHS columns had rank correlation exactly −1 | Independent Fisher–Yates shuffled strata (measured max \|ρ\| = 0.13) | 5 |
| F4 — dimensional jump at first weighted basis rebuild | Weight normalization to mean 1.0 | 5 |
| F5 — hard-wired 4-parameter set | **Parameter registry**: any named parameter via `[UNCERTAINTY] LAYER NAME PERT [DIST] [ENTRY]` with RATE_MULT / FORCING_MULT / FORCING_VECTOR / COUPLING_MULT taxonomy + UNIFORM / truncated-NORMAL / LOGNORMAL sampling; legacy columns bit-exact | 9a–9c |
| F6 — orifice Q ÷ Manning, head clamp at datum, updateBasis retry storm | Coupling difference-form with Cd-driven spread; invert clamp; rebuild backoff | 2, 6, 7 |

## Validation (PR 10 — `docs/uncertainty/VALIDATION.md`)

ROM bands vs 21 brute-force Monte-Carlo engine runs (Manning ±20%,
free-surface 5-junction chain, 1 h):

- **Coverage 0.997** (floor 0.90) — ROM [q05, q95] brackets the MC median.
- **Width ratio min/med/max = 0.68 / 1.25 / 1.61**, 100% within [0.3×, 3×].

The validation also *caught and fixed* a real formulation error (1D Manning
sensitivity projected absolute head, over-widening bands ~15× on sloped
networks → depth is now the sensitivity reference), and documented the
regimes where 1D bands are qualitative (surcharge/backwater; front-arrival
timing).

## Key invariants now enforced by tests

- Zero perturbation ⇒ q05 = q50 = q95 = deterministic, to 1e-12 (1D + 2D).
- Frozen-forcing analytic steady state δa = (mm−1)·b_j, per member per mode.
- Spread never collapses under sustained forcing (regression on old reseed).
- Registry back-compat: default columns bit-exact vs the pre-registry design;
  no-extra-params ROM paths bit-identical.

## Docs

`DEVIATION_FORM.md` (normative), `PARAMETER_REGISTRY.md` (normative),
`VALIDATION.md` (measured), `HOW_IT_WORKS.md` + `USER_GUIDE.md` aligned.

🤖 Generated with [Claude Code](https://claude.com/claude-code)
