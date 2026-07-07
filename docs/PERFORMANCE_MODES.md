# 1D Dynamic-Wave Performance Modes

The refactored engine offers two operating modes with a deliberate speed/accuracy
trade-off. Both are threaded (persistent-team OpenMP with an Apple-Silicon P-core
clamp); threading scales with model size (see the measured table below).

## Mode A — bit-exact (default)

Byte-identical `.out` to the legacy EPA-SWMM engine at any thread count. Build with the
fast kernels OFF (this is the parity guarantee):

```
cmake --preset=Darwin -DOPENSWMM_FAST_XSECT_LOOKUP=OFF -DOPENSWMM_FAST_MANNING_POW=OFF
```

Use for validation, regression, and regulatory work. Set `[OPTIONS] THREADS` to the
number of performance cores (the engine clamps to P-cores and pins QoS automatically;
`SWMM_DW_THREADS` overrides).

## Mode B — fast production (opt-in)

Not bit-identical (legitimate SWMM numerics; validated by mass balance + peak-hydraulic
accuracy, not point-wise trajectory — the DW solution is chaotic under variable step).
Build with the fast kernels ON, and in the `.inp` raise the timestep:

```
cmake --preset=Darwin -DOPENSWMM_FAST_XSECT_LOOKUP=ON -DOPENSWMM_FAST_MANNING_POW=ON
```

```ini
[OPTIONS]
THREADS              4          ; performance cores
LENGTHENING_STEP     6          ; Courant-lengthen short conduits
ROUTING_STEP         0:00:06    ; match the lengthening (seconds)
```

`LENGTHENING_STEP` = `ROUTING_STEP` (in seconds) is the tuning knob:

| Setting | Speedup vs legacy | Peak flow err (90th pct) | Fitness |
|---|---|---|---|
| 4  | ~3× | 3% | conservative |
| **6** | **~3.8×** | **5.5%** | **recommended production default** |
| 8  | ~4.3× | 6.6% | aggressive |
| 20 | ~7.5× | 24% | screening only — peaks unreliable |

Beyond ~dt 6 s the coarse step damps local surcharge peaks (mass balance stays <0.3% but
per-element peaks drift). Pick the smallest setting that meets your speed need.

### Levers that do NOT help (measured)
- **Anderson acceleration** (`ANDERSON_ACCEL YES`): 3× *slower* on stiff surcharging
  networks — it destabilizes convergence (iterations rise). Leave OFF.
- **Fast math kernels alone**: ~0% when threaded (the loop is barrier-bound; speeding the
  math just makes threads wait longer). They only help paired with the timestep lever.

## Threading scales with model size

| Model | Conduits | T=4 speedup |
|---|---|---|
| Bellinge | 1,015 | 1.74× (bit-exact) |
| GSO 10yr | 7,781 | 2.36× (bit-exact) |

Small models are barrier-bound (per-phase work is smaller than barrier sync cost); larger
models thread better. For maximum throughput on large models, combine Mode B with THREADS
at the P-core count.
