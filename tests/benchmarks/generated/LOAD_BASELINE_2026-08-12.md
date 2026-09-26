# Model load benchmark — BASELINE

**Date:** 2026-08-12  
**Host:** macOS-26.5.2-arm64-arm-64bit-Mach-O  
**CPU:** arm  
**Benchmark:** `/Users/calebbuahin/Documents/Projects/cbuahin_github/openswmm.engine/build/darwin-bench/tests/benchmarks/bench_model_load`  
**Corpus:** `/Users/calebbuahin/Documents/Projects/cbuahin_github/openswmm.engine/tests/benchmarks/generated`  
**Repetitions:** 5 (medians reported)

Times are milliseconds, medians of the reported repetitions.
`open` is open+close; `open_init` adds initialize();
`full` adds start()+end()+report(). No routing steps are run.
Peak RSS is the process high-water mark, one process per model.

| Model | open (ms) | open_init (ms) | full (ms) | peak RSS (MB) |
|---|---:|---:|---:|---:|
| `fv_variant` | 311.2 | 8047.4 | 8343.1 | 822 |
| `gage_heavy` | 335.3 | 339.8 | 354.3 | 92 |
| `grid_100k` | 306.7 | 336.5 | 660.9 | 396 |
| `grid_100k_geo` | 625.1 | 657.2 | 1028.7 | 489 |
| `grid_10k` | 28.5 | 32.5 | 65.6 | 59 |
| `grid_10k_geo` | 56.4 | 59.7 | 100.0 | 67 |
| `grid_250k` | 823.2 | 886.0 | 1691.6 | 906 |
| `grid_250k_geo` | 1626.9 | 1709.2 | 2596.1 | 1049 |
| `grid_500k` | 1762.1 | 1993.7 | 3656.7 | 1451 |
| `grid_500k_geo` | 3553.0 | 3707.2 | 5549.2 | 1749 |
| `grid_50k` | 145.7 | 159.5 | 319.9 | 221 |
| `grid_50k_geo` | 294.5 | 310.1 | 480.6 | 270 |
| `inflow_heavy` | 403.9 | 436.5 | 751.1 | 456 |
| `inflow_heavy_rpt` | 410.1 | 442.7 | 2550.1 | 469 |
| `street_heavy` | 448.3 | 465.9 | 641.5 | 376 |
| `transect_heavy` | 70.2 | 75.7 | 141.2 | 115 |
| `ts_heavy` | 7213.1 | 7037.1 | 7038.6 | 1183 |

## Phase-timer breakdown (`OPENSWMM_PERF=1`, full cut, seconds)

| Model | open.read | open.resolve | open.validate | open.prescan2d | res.extfiles | res.tables | res.transects | res.xsect | res.shrink | init.state | init.hydraulics | init.hydrology | init.quality | init.geometry | start.iface | start.plugins |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `fv_variant` | 0.257 | 0.011 | 0.000 | 0.036 | 0.000 | 0.000 | 0.000 | 0.005 | 0.003 | 0.004 | 7.663 | 0.000 | 0.000 | 0.001 | 0.000 | 0.044 |
| `gage_heavy` | 0.310 | 0.014 | 0.000 | 0.007 | 0.000 | 0.012 | 0.000 | 0.000 | 0.001 | 0.000 | 0.000 | 0.001 | 0.000 | 0.000 | 0.000 | 0.002 |
| `grid_100k` | 0.252 | 0.010 | 0.000 | 0.036 | 0.000 | 0.000 | 0.000 | 0.005 | 0.003 | 0.004 | 0.027 | 0.000 | 0.000 | 0.001 | 0.000 | 0.037 |
| `grid_100k_geo` | 0.500 | 0.018 | 0.000 | 0.101 | 0.000 | 0.000 | 0.000 | 0.005 | 0.010 | 0.004 | 0.026 | 0.001 | 0.000 | 0.001 | 0.000 | 0.037 |
| `grid_10k` | 0.023 | 0.001 | 0.000 | 0.004 | 0.000 | 0.000 | 0.000 | 0.001 | 0.001 | 0.000 | 0.003 | 0.000 | 0.000 | 0.000 | 0.000 | 0.004 |
| `grid_10k_geo` | 0.045 | 0.002 | 0.000 | 0.007 | 0.000 | 0.000 | 0.000 | 0.001 | 0.001 | 0.000 | 0.002 | 0.000 | 0.000 | 0.000 | 0.000 | 0.004 |
| `grid_250k` | 0.671 | 0.026 | 0.000 | 0.086 | 0.000 | 0.001 | 0.000 | 0.012 | 0.007 | 0.011 | 0.070 | 0.000 | 0.000 | 0.002 | 0.000 | 0.088 |
| `grid_250k_geo` | 1.314 | 0.046 | 0.000 | 0.252 | 0.000 | 0.001 | 0.000 | 0.012 | 0.028 | 0.012 | 0.074 | 0.007 | 0.000 | 0.002 | 0.000 | 0.094 |
| `grid_500k` | 1.580 | 0.065 | 0.000 | 0.178 | 0.000 | 0.001 | 0.000 | 0.025 | 0.023 | 0.024 | 0.160 | 0.000 | 0.000 | 0.004 | 0.000 | 0.193 |
| `grid_500k_geo` | 2.845 | 0.099 | 0.000 | 0.524 | 0.000 | 0.002 | 0.000 | 0.025 | 0.058 | 0.025 | 0.160 | 0.011 | 0.000 | 0.005 | 0.000 | 0.191 |
| `grid_50k` | 0.121 | 0.006 | 0.000 | 0.018 | 0.000 | 0.000 | 0.000 | 0.002 | 0.001 | 0.002 | 0.013 | 0.000 | 0.000 | 0.000 | 0.000 | 0.018 |
| `grid_50k_geo` | 0.238 | 0.009 | 0.000 | 0.048 | 0.000 | 0.000 | 0.000 | 0.002 | 0.005 | 0.002 | 0.014 | 0.002 | 0.000 | 0.000 | 0.000 | 0.019 |
| `inflow_heavy` | 0.335 | 0.010 | 0.000 | 0.057 | 0.000 | 0.000 | 0.000 | 0.005 | 0.003 | 0.004 | 0.031 | 0.000 | 0.000 | 0.001 | 0.000 | 0.035 |
| `inflow_heavy_rpt` | 0.335 | 0.011 | 0.000 | 0.058 | 0.000 | 0.000 | 0.000 | 0.005 | 0.003 | 0.004 | 0.028 | 0.000 | 0.000 | 0.001 | 0.000 | 1.825 |
| `street_heavy` | 0.136 | 0.298 | 0.000 | 0.020 | 0.000 | 0.000 | 0.000 | 0.294 | 0.003 | 0.004 | 0.016 | 0.000 | 0.000 | 0.001 | 0.000 | 0.018 |
| `transect_heavy` | 0.056 | 0.006 | 0.000 | 0.009 | 0.000 | 0.000 | 0.005 | 0.001 | 0.001 | 0.001 | 0.005 | 0.000 | 0.000 | 0.000 | 0.000 | 0.008 |
| `ts_heavy` | 6.407 | 0.002 | 0.000 | 0.554 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |

Phase timers come from a separate single-iteration run, so they will not sum exactly to the medians above.

