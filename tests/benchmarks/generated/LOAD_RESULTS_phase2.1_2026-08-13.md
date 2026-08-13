# Model load benchmark — RESULTS_phase2.1

**Date:** 2026-08-13  
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
| `grid_500k_geo` | 3454.3 | 3654.3 | 5453.6 | 1732 |
| `grid_250k_geo` | 1694.4 | 1730.6 | 2584.1 | 1096 |
| `grid_500k` | 1766.3 | 1940.5 | 3529.6 | 1447 |
| `ts_heavy` | 2548.2 | 2480.8 | 2521.2 | 1170 |

## Phase-timer breakdown (`OPENSWMM_PERF=1`, full cut, seconds)

| Model | open.read | open.resolve | open.validate | open.prescan2d | res.extfiles | res.tables | res.transects | res.xsect | res.shrink | init.state | init.hydraulics | init.hydrology | init.quality | init.geometry | start.iface | start.plugins |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `grid_500k_geo` | 2.835 | 0.106 | 0.000 | 0.514 | 0.000 | 0.002 | 0.000 | 0.026 | 0.063 | 0.025 | 0.159 | 0.010 | 0.000 | 0.004 | 0.000 | 0.188 |
| `grid_250k_geo` | 1.338 | 0.048 | 0.000 | 0.252 | 0.000 | 0.001 | 0.000 | 0.012 | 0.029 | 0.011 | 0.069 | 0.005 | 0.000 | 0.002 | 0.000 | 0.090 |
| `grid_500k` | 1.480 | 0.059 | 0.000 | 0.168 | 0.000 | 0.001 | 0.000 | 0.024 | 0.019 | 0.024 | 0.159 | 0.000 | 0.000 | 0.004 | 0.000 | 0.188 |
| `ts_heavy` | 1.884 | 0.001 | 0.000 | 0.544 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |

Phase timers come from a separate single-iteration run, so they will not sum exactly to the medians above.

