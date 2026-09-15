# Model load benchmark — RESULTS_phase1b

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
| `street_heavy` | 158.6 | 174.1 | 344.0 | 243 |
| `transect_heavy` | 68.2 | 74.0 | 142.4 | 104 |
| `grid_250k` | 837.9 | 894.2 | 1656.4 | 825 |
| `ts_heavy` | 2663.6 | 2514.3 | 2517.5 | 1179 |

## Phase-timer breakdown (`OPENSWMM_PERF=1`, full cut, seconds)

| Model | open.read | open.resolve | open.validate | open.prescan2d | res.extfiles | res.tables | res.transects | res.xsect | res.shrink | init.state | init.hydraulics | init.hydrology | init.quality | init.geometry | start.iface | start.plugins |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `street_heavy` | 0.138 | 0.006 | 0.000 | 0.020 | 0.000 | 0.000 | 0.000 | 0.002 | 0.003 | 0.003 | 0.013 | 0.000 | 0.000 | 0.001 | 0.000 | 0.018 |
| `transect_heavy` | 0.055 | 0.004 | 0.000 | 0.009 | 0.000 | 0.000 | 0.002 | 0.001 | 0.001 | 0.001 | 0.005 | 0.000 | 0.000 | 0.000 | 0.000 | 0.007 |
| `grid_250k` | 0.687 | 0.027 | 0.000 | 0.085 | 0.000 | 0.001 | 0.000 | 0.012 | 0.008 | 0.011 | 0.069 | 0.000 | 0.000 | 0.002 | 0.000 | 0.087 |
| `ts_heavy` | 1.874 | 0.001 | 0.000 | 0.548 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |

Phase timers come from a separate single-iteration run, so they will not sum exactly to the medians above.

