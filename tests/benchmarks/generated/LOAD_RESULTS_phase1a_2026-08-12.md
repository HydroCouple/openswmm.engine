# Model load benchmark — RESULTS_phase1a

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
| `fv_variant` | 301.6 | 7942.5 | 8255.7 | 832 |
| `gage_heavy` | 48.6 | 49.5 | 70.1 | 100 |
| `grid_100k` | 301.9 | 328.4 | 669.3 | 408 |
| `grid_100k_geo` | 619.2 | 684.0 | 1025.5 | 477 |
| `grid_10k` | 28.5 | 31.4 | 67.6 | 57 |
| `grid_10k_geo` | 54.7 | 57.7 | 95.7 | 66 |
| `grid_250k` | 834.7 | 905.0 | 1720.7 | 951 |
| `grid_250k_geo` | 1645.0 | 1772.8 | 2799.7 | 1154 |
| `grid_500k` | 1757.0 | 1930.4 | 3506.1 | 1438 |
| `grid_500k_geo` | 3462.3 | 3696.1 | 5468.1 | 1719 |
| `grid_50k` | 145.5 | 158.8 | 315.6 | 215 |
| `grid_50k_geo` | 292.0 | 309.4 | 476.7 | 256 |
| `inflow_heavy` | 401.0 | 440.3 | 759.6 | 468 |
| `inflow_heavy_rpt` | 410.4 | 440.7 | 897.4 | 448 |
| `street_heavy` | 451.9 | 465.8 | 622.4 | 364 |
| `transect_heavy` | 70.1 | 75.3 | 142.2 | 105 |
| `ts_heavy` | 2571.0 | 2513.5 | 2516.2 | 1177 |

## Phase-timer breakdown (`OPENSWMM_PERF=1`, full cut, seconds)

| Model | open.read | open.resolve | open.validate | open.prescan2d | res.extfiles | res.tables | res.transects | res.xsect | res.shrink | init.state | init.hydraulics | init.hydrology | init.quality | init.geometry | start.iface | start.plugins |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `fv_variant` | 0.258 | 0.012 | 0.000 | 0.035 | 0.000 | 0.000 | 0.000 | 0.005 | 0.004 | 0.004 | 7.612 | 0.000 | 0.000 | 0.001 | 0.000 | 0.036 |
| `gage_heavy` | 0.039 | 0.002 | 0.000 | 0.006 | 0.000 | 0.000 | 0.000 | 0.000 | 0.001 | 0.000 | 0.000 | 0.001 | 0.000 | 0.000 | 0.000 | 0.002 |
| `grid_100k` | 0.303 | 0.012 | 0.000 | 0.035 | 0.000 | 0.000 | 0.000 | 0.005 | 0.004 | 0.004 | 0.027 | 0.000 | 0.000 | 0.001 | 0.000 | 0.037 |
| `grid_100k_geo` | 0.507 | 0.021 | 0.000 | 0.100 | 0.000 | 0.000 | 0.000 | 0.005 | 0.014 | 0.004 | 0.029 | 0.001 | 0.000 | 0.001 | 0.000 | 0.039 |
| `grid_10k` | 0.023 | 0.001 | 0.000 | 0.004 | 0.000 | 0.000 | 0.000 | 0.001 | 0.001 | 0.000 | 0.002 | 0.000 | 0.000 | 0.000 | 0.000 | 0.004 |
| `grid_10k_geo` | 0.045 | 0.003 | 0.000 | 0.007 | 0.000 | 0.000 | 0.000 | 0.001 | 0.002 | 0.001 | 0.003 | 0.000 | 0.000 | 0.000 | 0.000 | 0.004 |
| `grid_250k` | 0.739 | 0.027 | 0.000 | 0.086 | 0.000 | 0.001 | 0.000 | 0.012 | 0.007 | 0.011 | 0.069 | 0.000 | 0.000 | 0.002 | 0.000 | 0.099 |
| `grid_250k_geo` | 1.355 | 0.045 | 0.000 | 0.258 | 0.000 | 0.001 | 0.000 | 0.012 | 0.026 | 0.011 | 0.075 | 0.007 | 0.000 | 0.002 | 0.000 | 0.090 |
| `grid_500k` | 1.452 | 0.062 | 0.000 | 0.171 | 0.000 | 0.001 | 0.000 | 0.025 | 0.021 | 0.024 | 0.162 | 0.000 | 0.000 | 0.004 | 0.000 | 0.172 |
| `grid_500k_geo` | 2.856 | 0.104 | 0.000 | 0.524 | 0.000 | 0.002 | 0.000 | 0.026 | 0.061 | 0.025 | 0.173 | 0.015 | 0.000 | 0.005 | 0.000 | 0.225 |
| `grid_50k` | 0.117 | 0.006 | 0.000 | 0.019 | 0.000 | 0.000 | 0.000 | 0.002 | 0.003 | 0.002 | 0.014 | 0.000 | 0.000 | 0.000 | 0.000 | 0.017 |
| `grid_50k_geo` | 0.235 | 0.009 | 0.000 | 0.048 | 0.000 | 0.000 | 0.000 | 0.002 | 0.006 | 0.002 | 0.014 | 0.001 | 0.000 | 0.000 | 0.000 | 0.019 |
| `inflow_heavy` | 0.334 | 0.010 | 0.000 | 0.057 | 0.000 | 0.000 | 0.000 | 0.005 | 0.003 | 0.005 | 0.031 | 0.000 | 0.000 | 0.001 | 0.000 | 0.033 |
| `inflow_heavy_rpt` | 0.335 | 0.011 | 0.000 | 0.059 | 0.000 | 0.000 | 0.000 | 0.005 | 0.004 | 0.004 | 0.033 | 0.000 | 0.000 | 0.001 | 0.000 | 0.186 |
| `street_heavy` | 0.131 | 0.299 | 0.000 | 0.020 | 0.000 | 0.000 | 0.000 | 0.296 | 0.002 | 0.005 | 0.015 | 0.000 | 0.000 | 0.001 | 0.000 | 0.017 |
| `transect_heavy` | 0.053 | 0.007 | 0.000 | 0.009 | 0.000 | 0.000 | 0.005 | 0.001 | 0.001 | 0.001 | 0.004 | 0.000 | 0.000 | 0.000 | 0.000 | 0.007 |
| `ts_heavy` | 1.848 | 0.002 | 0.000 | 0.548 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |

Phase timers come from a separate single-iteration run, so they will not sum exactly to the medians above.

