# Physical-time momentum smoothing study

The opt-in `THETA_REFERENCE_STEP` fixes the time interval over which THETA
retains momentum. A zero reference preserves the historical per-face-firing
calculation. Use a fixed positive interval across a refinement series;
changing it with the routing step changes the numerical model being tested.

The 22 October 2026 study uses THETA 0.8, a 1 s reference, 300 s dry-start
runs, a 1,600-triangle mesh and unchanged physical-shaft geometry, boundary
forcing, exchange coefficients, head regularisation and equalisation. It
tests all 36 Table 2 rows at routing steps 0.0125, 0.00625 and 0.003125 s.
The 1 s reference is a stated numerical relaxation interval, not a fit to
the observations. The reference report is
`docs/validation/rubinato2017/momentum_report.md`.

## Reproduce

1. Build the engine and the native test target containing
   `test_2d_exchange_rubinato_model.cpp`. Run
   `--gtest_filter=RubinatoModel.PhysicalShaftSensitivity` in a writable
   results folder to generate the 36 self-contained `*_SHAFT.inp` decks in
   `rubinato2017_model_out/`. That test also runs the original coarse cases.
   The focused `PhysicalTimeMomentumSmoothingConvergesAcrossAllFourRegimes`
   regression tests four regimes at the two finest intervals using the
   default 1D iteration controls; it is independent of the full study's
   tighter iteration settings.
2. Use Python with numpy. Run the collector with an explicit production
   shared library and those input decks. The collector defaults to all 36
   names in Table 2 order; `--cases` accepts a comma-separated subset.

```sh
python3 tests/benchmarks/rubinato2017/momentum_convergence.py \
  --library /absolute/path/to/engine/library \
  --input-directory /absolute/path/to/rubinato2017_model_out \
  --output /absolute/path/to/study --phase matrix \
  --steps 0.0125,0.00625,0.003125 --settings tight5 --reference-step 1
```

The collector sets fixed routing (`VARIABLE_STEP 0`, `LENGTHENING_STEP 0`),
80 maximum trials and a 1e-5 m head tolerance. Each simulation gets a separate
process, input, report, one-second series, JSON job/result and retained log.
The exact actual routing-step extrema, binary/input hashes, heads, exchange
integrals, 1D/2D continuity and separate volume ledgers are recorded. The
late rate is derived from cumulative transfer at exactly 240 and 300 s;
the 180–240 s window diagnoses remaining drift. Positive is sewer to surface.

3. Save the tested source-file hashes as `source_manifest.json` at the study
   root, including dirty-source provenance. Then summarize without rerunning
   the engine:

```sh
python3 tests/benchmarks/rubinato2017/summarize_momentum.py \
  --study /absolute/path/to/study --output /absolute/path/to/summary
```

Copy the resulting `momentum_data.json` to
`docs/figures/data/rubinato2017_momentum.json`. With numpy/matplotlib,
regenerate the manual figures using
`scripts/build_manual_figures.py build --only 'hydraulics_ch9_rubinato_momentum_*'`.
The snapshot includes binary/source identifiers and all numerical and
experimental diagnostics; raw simulation outputs remain in the results folder.

## Interpretation

The temporal criterion is a change below 1% between the two finest rates,
normalised by the finest magnitude. It is separate from a 2% successive-window
drift check, a 2% ordinary 1D continuity check, and experimental agreement
within `xi_f + 0.02 Q_e`. That illustrative experimental band is not a
confidence interval. Keep every failure visible. Do not combine the 1D and
2D ledgers without accounting for the final pending exchange inventory.

These are reconstructed laboratory cases with an artificial pressure
reservoir and assumed manhole location. They reuse measured values in the
boundary reconstruction, have no demonstrated mesh convergence and do not
replay measured transient histories. A converged numerical comparison does
not establish independent experimental validation.
