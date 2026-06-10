# RDII Initial Abstraction — Physics-Based Seasonal Soil Moisture Model
## Problem Statement and Implementation Design

**Status:** Plan revised 2026-05-13 — design decisions locked, ready for implementation.
**Branch:** `dev`
**Companion plan:** [RDII_IMPLEMENTATION_PLAN.md](RDII_IMPLEMENTATION_PLAN.md) (linear pipeline, complete).

---

## 1. The Foundational Problem — Seasonal RTK as a Surrogate for Soil Moisture

### 1.1 What Seasonal RTK Calibration Is Really Doing

SWMM's RDII module allows RTK unit hydrograph parameters to be specified by month, enabling different R, T, and K values for each calendar month or season. In practice, modellers use this capability almost exclusively to adjust the **R parameter** — the fraction of rainfall that becomes RDII — to reproduce observed seasonal variation in RDII response.

The physical drivers of this seasonal variation are well understood:

- In winter and early spring, soil surrounding sewer infrastructure is at or near saturation. Antecedent moisture is high, initial abstraction capacity is depleted, and a large fraction of rainfall infiltrates through cracks, joints, and defects into the pipe.
- In summer and autumn, the same soil has dried through evapotranspiration and drainage. Antecedent moisture is low, initial abstraction capacity is largely restored, and a much smaller fraction of rainfall generates RDII.

Monthly R values are therefore not describing a changing physical property of the sewer infrastructure — **they are describing the seasonal state of soil moisture** using a static parameter as a surrogate. A high winter R absorbs the effect of depleted IA into the unit hydrograph fraction. A low summer R absorbs the effect of restored IA into the same parameter.

```
Current approach — soil moisture state absorbed into R:

         Winter R = 0.08  ]
         Spring R = 0.07  ]   Different R per month to mimic seasonal
         Summer R = 0.02  ]   soil moisture variation. No IA physics.
         Autumn R = 0.04  ]   R is doing two jobs simultaneously.
```

### 1.2 Why This Is Physically Wrong

The R parameter represents the long-term steady-state fraction of rainfall entering the sewer through infrastructure defects — a property of pipe condition, soil type, and drainage area geometry. It should not vary seasonally. When R is calibrated monthly, the calibrated values conflate two distinct physical quantities:

1. The true infrastructure leakage fraction — a physical constant for a given sewershed
2. The antecedent moisture state — a dynamic variable that evolves continuously with weather

This conflation has consequences. A model calibrated with monthly R values cannot correctly simulate an anomalously wet summer or a dry winter because the soil moisture state is locked into calendar-month lookup tables rather than being tracked dynamically. The model cannot be transferred to a changed climate scenario or long-term projection without re-calibrating monthly R values that are implicitly tied to historical average seasonal conditions.

### 1.3 The Correct Architecture

Seasonal variation in RDII response should emerge from **physically tracked soil moisture state** acting on a single, seasonally invariant set of RTK values:

```
Proposed approach — soil moisture tracked explicitly:

         Single R = 0.06  ]   One R representing true infrastructure
         Single T = 2.0   ]   leakage fraction. Stable across seasons.
         Single K = 2.0   ]

                               Seasonal variation in RDII emerges from:
                               - Temperature-dependent IA recovery (ET)
                               - Baseline time-decay recovery (drainage)
                               - Frozen ground suppression in winter
                               - Exponential rather than linear IA dynamics
```

When IA is tracked with temperature-modulated exponential dynamics, soil moisture evolves realistically through the year without calendar-month prescription. A cold winter suppresses IA recovery, antecedent moisture accumulates, effective IA entering spring is low, and RDII response is high. A warm dry summer drives rapid IA recovery, effective IA is high, and RDII response is low. The seasonal pattern emerges from physics, not from a lookup table.

This also makes RTK parameters physically interpretable and transferable. R becomes a property of sewer infrastructure that can be estimated from condition surveys or regional regression. T and K describe hydrograph shape governed by drainage area geometry and network travel time — both stable physical properties.

### 1.4 What the Linear IA Model Cannot Do

The current linear IA model fails to produce realistic seasonal behaviour even when temperature effects are added, because:

**Linear recovery is too fast immediately after a storm.** A linear ramp from depleted to full IA predicts that substantial IA is restored within a fixed time regardless of antecedent conditions. Freshly wetted soil recovers slowly at first, then faster as evapotranspiration progressively draws down the moisture profile — an asymptotic process that a linear ramp cannot reproduce.

**Linear depletion does not reflect wetting-front physics.** Linear depletion assumes the same IA consumption rate regardless of how much IA remains. In a wetting soil, infiltration capacity declines as moisture content rises — exponential depletion captures this correctly.

**Without a temperature coupling there is no seasonal signal.** With constant `IArecov`, identical recovery occurs in January and July. All seasonal variation must be forced through monthly R values. This is the root failure the proposed model is designed to correct.

---

## 2. The Exponential Decay Formulation

### 2.1 Governing Equations

**Depletion during storms** — available IA decays exponentially with accumulated rainfall depth:

```
IA_avail(t+dt) = IA_avail(t) * exp(-k_dep * delta_rain)
```

Where `delta_rain` is rainfall depth in the current time step and `k_dep` is a temperature-independent depletion rate coefficient. Both are in **project rain-depth units** — `delta_rain`, `Dmax`/`IAmax`, and `1/k_dep` all share the project's rain unit (inches for US-unit projects, mm for SI), so unit-consistent parameter pairs scale by 25.4 (e.g. `k_dep = 0.3 1/mm ≡ 7.62 1/in`). Depletion is treated as temperature-independent — during a storm, IA consumption is dominated by rainfall intensity and antecedent moisture state rather than temperature. This reduces the parameter count and avoids over-parameterisation.

Rainfall excess available for RDII convolution:

```
IA_consumed = IA_avail(t) - IA_avail(t) * exp(-k_dep * delta_rain)
excess = max(0, rainfall - IA_consumed)
```

### 2.1.1 Mass-Consistent Bookkeeping and Degenerate Limits

The storage is drained by exactly the depth it abstracts from the rainfall: the **same** `IA_consumed` both reduces `IA_avail` and is subtracted from the rainfall to form the excess (take ≡ lose). This identity is load-bearing. An earlier calibration prototype paired a threshold excess law, `excess = max(0, P - IA_avail)`, with an *independent* exponential drain of the bucket — under that decoupling, the optimizer drives `k_dep → 0`, the bucket never empties, and IA degenerates into a fixed daily threshold: antecedent wetness has no effect, the four recovery parameters become unidentifiable (zero fit gradient), and `IAmax` is forced below typical storm depths for the model to produce any flow at all. Any reimplementation (calibration harnesses, ports) must preserve the take ≡ lose identity. The reference implementation is the `sparsehydro` `IAModel` (`compute_excess_series`); the engine is pinned to it by golden-value tests in `tests/unit/engine/test_rdii.cpp` (`RdiiDecayReference*`).

Known limits of the consistent law, for calibration guidance:

- **`k_dep → 0` disables abstraction entirely** (`IA_consumed → 0`, `excess → P`, state frozen). The engine emits a warning for active rows with `k_dep = 0`; calibration should keep a strictly positive lower bound on `k_dep`.
- **`k_dep > 1/IA_avail` permits `IA_consumed > delta_rain`** for small pulses: the excess clamps at 0 and the storage intentionally drains faster than the supplied rain. This is the documented "aggressive application" regime — `k_dep` controls how aggressively the remaining capacity is applied to a given storm (`k_dep → ∞` consumes the full remaining capacity on any rainfall). Rule of thumb remains `k_dep ≈ 1/IAmax` (§9.2).

**Recovery during dry periods** — available IA recovers exponentially toward `IAmax`, with a rate that is the sum of independent time-based and temperature-driven contributions:

```
IA_avail(t+dt) = IAmax - (IAmax - IA_avail(t)) * exp(-k_rec(T) * dt)
```

This is the exact integral of the first-order recovery ODE:

```
dIA_avail/dt = k_rec(T) * (IAmax - IA_avail)
```

The asymptotic approach to `IAmax` is physically correct — fast initial recovery when the deficit is large, slowing as capacity is restored. The linear model cannot reproduce this shape.

### 2.2 Additive Recovery Rate — Separating Time and Temperature Effects

The recovery rate coefficient is the **sum of two independent contributions**:

```
k_rec(T) = k_0 + k_T * exp(theta_rec * (T - T_ref))
```

**Why additive decomposition is physically correct.**

IA recovery is driven by two distinct, parallel pathways:

1. **k_0 (base rate)** — gravity drainage, capillary redistribution, and baseline evaporation that occur regardless of temperature. These processes have weak or negligible temperature dependence over the range of interest. Even in cool conditions above freezing, soil water redistributes under gravity and capillary gradients.

2. **k_T * exp(theta_rec * (T - T_ref)) (thermal rate)** — evapotranspiration, microbial drying, and temperature-dependent unsaturated hydraulic conductivity. These processes have well-established Arrhenius-type temperature sensitivity.

Summing independent rate contributions follows from parallel first-order kinetics — each pathway drives IA recovery independently, and the effective rate is the sum. This is standard in reaction kinetics, soil science, and ecological process modelling.

**Advantages of additive over purely multiplicative form.**

The previous purely multiplicative form `k_ref * exp(theta * (T - T_ref))` implicitly couples time and temperature into a single coefficient. This has two shortcomings:

- At low temperatures, the exponential drives the entire recovery rate toward zero, which is physically incorrect — gravity drainage still operates above freezing.
- `k_ref` absorbs both time-dependent and temperature-dependent effects, making it harder to calibrate from physical observations.

The additive form guarantees a minimum recovery rate `k_0 > 0` even at low temperatures (above freezing), isolates the temperature sensitivity into `k_T` and `theta_rec`, and makes both parameters independently interpretable:

- `k_0` can be estimated from dry-period IA recovery observed in cool-season events where ET is negligible.
- `k_T` and `theta_rec` can be calibrated from the additional recovery seen in warm-season events.

**Gradient-from-reference formulation for the thermal term.**

The thermal term uses `exp(theta_rec * (T - T_ref))` rather than `exp(theta_rec * T)` to centre the parameterisation at a user-chosen reference temperature where the thermal contribution equals `k_T` exactly. When `T = T_ref`, the exponential equals 1 and `k_rec = k_0 + k_T`. The sensitivity parameter `theta_rec` (1/deg C) is a dimensionally consistent local derivative that does not change meaning if `T_ref` is adjusted.

**Recommended T_ref.** Set to the mean annual air temperature of the study catchment, or 10 deg C as a temperate climate default.

**Why temperature dependence belongs on recovery, not depletion.** Recovery is driven by evapotranspiration and unsaturated drainage — processes with well-established Arrhenius-type temperature dependence. Depletion during a storm is governed by rainfall intensity and antecedent soil moisture state on time scales of hours, where temperature plays a secondary role. Applying temperature dependence to recovery only focuses the physics where it matters and keeps the depletion parameter scalar (`k_dep`).

### 2.3 Frozen Ground Suppression

Recovery is suppressed below a threshold temperature `T_freeze`:

```
k_rec(T) = 0                                                      when T <  T_freeze
k_rec(T) = k_0 + k_T * exp(theta_rec * (T - T_ref))              when T >= T_freeze
```

This represents frozen or near-frozen ground where ET is negligible and soil drainage is impeded. Antecedent IA deficit accumulated during autumn storms accumulates through winter without recovery, producing elevated RDII in early spring — the physical mechanism behind the spring peak observed in cold-climate systems that monthly R values can only approximate coarsely.

### 2.4 Emergent Seasonal Behaviour

The following illustrates how the proposed model reproduces seasonal RDII variation from physics alone, without monthly R values:

| Season | T (deg C) | k_0 | k_T * exp(...) | k_rec(T) | IA Recovery | RDII Response |
|---|---|---|---|---|---|---|
| Winter | 2 | — | — | 0 (frozen) | Suppressed | High |
| Early spring | 6 | 0.01 | 0.004 | 0.014 | Slow | High |
| Late spring | 13 | 0.01 | 0.010 | 0.020 | Moderate | Declining |
| Summer | 22 | 0.01 | 0.032 | 0.042 | Fast | Low |
| Autumn | 10 | 0.01 | k_T | 0.01 + k_T | Moderate | Increasing |

This seasonal pattern emerges from `k_0`, `k_T`, `theta_rec`, and `T_freeze` applied to a single RTK set — no monthly lookup tables required. Note that even in early spring with low temperatures, the base rate `k_0` provides non-zero recovery — a physical improvement over the purely multiplicative form.

### 2.5 Reduction to the Linear Model

First-order Taylor expansion of the exponential recovery near full capacity gives:

```
IA_avail(t+dt) ≈ IA_avail(t) + k_rec(T) * (IAmax - IA_avail) * dt
```

When `IA_avail` is close to `IAmax`, this approximates the linear model with an effective `IArecov = k_rec(T) * (IAmax - IA_avail)`. The exponential model is therefore a physically consistent generalisation that converges to the linear model near full capacity and diverges — correctly — under large deficits and long dry periods.

---

## 3. Input Specification

### 3.1 Existing `[HYDROGRAPHS]` Section — Untouched, One ALL Row Per Response Is Enough

The `[HYDROGRAPHS]` parser, writer, and GeoPackage round-trip are **unchanged**. With physically tracked soil moisture, monthly RTK specification is no longer needed in practice, but the linear path remains fully supported for backward compatibility. A single `ALL` entry per response is sufficient when exponential decay is active:

```
[HYDROGRAPHS]
;UHGroup    Month  Response  R       T    K    IAmax  IArecov  IAinit
SanSewer    ALL    SHORT     0.055   1.0  2.0  8.0    0.10     2.0
SanSewer    ALL    MEDIUM    0.032   3.5  2.0  8.0    0.10     2.0
SanSewer    ALL    LONG      0.018  14.0  2.0  8.0    0.10     2.0
```

When `[RDII_DECAY]` provides a row for `(UHGroup, Response)`, `IArecov` from `[HYDROGRAPHS]` is ignored for that response; `IAmax` and `IAinit` continue to be used (they describe the abstraction reservoir, not its dynamics).

### 3.2 New `[RDII_DECAY]` Section

```
[RDII_DECAY]
;UHGroup    Response  k_dep   k_0    k_T      T_ref  theta_rec  T_freeze
SanSewer    SHORT     0.15    0.010  0.070    10.0   0.055      0.0
SanSewer    MEDIUM    0.10    0.008  0.037    10.0   0.055      0.0
SanSewer    LONG      0.05    0.005  0.013    10.0   0.040      0.0
```

Column definitions:
- `UHGroup` — unit hydrograph group name matching `[HYDROGRAPHS]`
- `Response` — SHORT, MEDIUM, or LONG
- `k_dep` — temperature-independent depletion rate (1/project rain-depth unit: 1/in or 1/mm)
- `k_0` — base recovery rate independent of temperature (1/hr)
- `k_T` — thermal recovery rate at T_ref (1/hr)
- `T_ref` — reference temperature for the thermal term (deg C)
- `theta_rec` — temperature sensitivity of thermal recovery (1/deg C)
- `T_freeze` — temperature below which all recovery is suppressed (deg C)

Granularity is per `(UHGroup, Response)` — three rows per UH group at most. A UH group with **no** `[RDII_DECAY]` rows continues to use the legacy linear IA logic; a group with one row falls back to linear for the two unspecified responses. This makes adoption incremental.

---

## 4. Architectural Decisions

Two cross-cutting decisions drive the implementation surface; both were locked before this revision.

### 4.1 ClimateState Moves into `SimulationContext`

Today `ClimateState` is owned by `SWMMEngine` ([src/engine/core/SWMMEngine.hpp:240](../src/engine/core/SWMMEngine.hpp#L240)), and the ~40 `climate_.X` references all live inside [SWMMEngine.cpp](../src/engine/core/SWMMEngine.cpp). The exp-decay helper needs read access to the current air temperature, and the project pattern is that all simulation state flows through `SimulationContext&`. So `ClimateState` moves into `ctx.climate_state` as part of this work.

**Scope of the migration:**
- New member `climate::ClimateState climate_state;` in `SimulationContext` near the other top-level state (after `options`).
- Delete `SWMMEngine::climate_` and rename every reference in `SWMMEngine.cpp` (`climate_.X` → `ctx_.climate_state.X`). Mechanical, single file.
- `ClimateFileReader` stays on `SWMMEngine` — it owns file handles and isn't simulation state.
- Plugins that currently know about the climate state (none directly; all access is through `engine.climate_` in SWMMEngine.cpp) are unaffected.

**Why this is the right time:** any other future helper (runoff variants, snowmelt sensitivity studies, GW recovery rate models) will want the same access. Doing it now amortises the cost.

### 4.2 Exponential Decay Lives in a Parallel `RDIIDecayData` SoA

Rather than adding `ExpDecayParams expDecay[3]` to `UnitHydParams` and pushing 6 nullable columns into the existing `unit_hydrographs` schema, the decay parameters live in a parallel struct:

```cpp
// data/InflowData.hpp — new section, sibling to UnitHydData

struct RDIIDecayEntry {
    std::string uh_name;   ///< Matches UnitHydEntry::name
    int         response;  ///< 0=SHORT, 1=MEDIUM, 2=LONG
    double      k_dep;     ///< 1/(project rain-depth unit)
    double      k_0;       ///< 1/hr
    double      k_T;       ///< 1/hr (at T_ref)
    double      T_ref;     ///< deg C
    double      theta_rec; ///< 1/deg C
    double      T_freeze;  ///< deg C
};

struct RDIIDecayData {
    std::vector<RDIIDecayEntry> entries;
    int count() const { return static_cast<int>(entries.size()); }
    void add(const RDIIDecayEntry& e) { entries.push_back(e); }
};
```

And a sibling field on `SimulationContext` next to `unit_hyds`:

```cpp
RDIIDecayData    rdii_decay;     ///< Parsed [RDII_DECAY] data
```

**Why parallel storage:**
- The linear `[HYDROGRAPHS]` write/read path is byte-stable. No risk of regressions on existing models.
- The GeoPackage `unit_hydrographs` schema is unchanged. A new `rdii_decay` table is additive — old GeoPackages still load.
- `RDIISolver::init()` resolves entries by name into per-group runtime params (see §5.4) — the SoA is purely for IO and lookup, runtime layout is the solver's concern.

**Runtime state:** the per-group, per-response IA state already exists implicitly via the `past_rain` buffer and convolution; no new state arrays are needed in `RDIIGroupSoA`. Hot-start interchangeability is preserved.

---

## 5. Implementation Plan

### 5.1 Data Structures

| Where | Change |
|---|---|
| [src/engine/data/InflowData.hpp](../src/engine/data/InflowData.hpp) | Add `RDIIDecayEntry` and `RDIIDecayData` after `UnitHydData` (after line ~123). |
| [src/engine/core/SimulationContext.hpp:445](../src/engine/core/SimulationContext.hpp#L445) | Add `RDIIDecayData rdii_decay;` immediately after `UnitHydData unit_hyds;`. |
| [src/engine/core/SimulationContext.hpp](../src/engine/core/SimulationContext.hpp) | Add `climate::ClimateState climate_state;` near top-level state (just after `options`). Add `#include "../hydrology/Climate.hpp"` (forward declaration won't work — ClimateState has inline initialisers). Reset in `reset()` (line ~1022). |
| [src/engine/hydrology/RDII.hpp](../src/engine/hydrology/RDII.hpp) | Add per-group, per-response decay parameters to `RDIIGroupSoA` (or a parallel `RDIIDecaySoA`). One entry per (group, response), populated in `init()` by resolving `ctx.rdii_decay` against `unit_hyds`. |

### 5.2 ClimateState Migration

| Where | Change |
|---|---|
| [src/engine/core/SWMMEngine.hpp:240](../src/engine/core/SWMMEngine.hpp#L240) | Remove `climate::ClimateState climate_;`. |
| [src/engine/core/SWMMEngine.cpp](../src/engine/core/SWMMEngine.cpp) | Replace `climate_.` → `ctx_.climate_state.` (~40 occurrences, all in this file). |
| [src/engine/hydrology/Climate.hpp](../src/engine/hydrology/Climate.hpp) | No structural change — `ClimateState` already public. |

Verify after migration: existing tests that reference climate behaviour still pass (snowmelt, evaporation, infiltration recovery factor).

### 5.3 Input Parsing — `[RDII_DECAY]` Handler

**New file:** [src/engine/input/handlers/InflowsHandler.cpp](../src/engine/input/handlers/InflowsHandler.cpp) gets a new `handle_rdii_decay` function alongside `handle_hydrographs`. Header in [InflowsHandler.hpp](../src/engine/input/handlers/InflowsHandler.hpp) gets the declaration.

```cpp
void handle_rdii_decay(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 8) continue;  // silent skip — matches other handlers

        RDIIDecayEntry e;
        e.uh_name = tok[0];

        std::string resp = Tokenizer::to_upper(tok[1]);
        if      (resp == "SHORT")  e.response = 0;
        else if (resp == "MEDIUM") e.response = 1;
        else if (resp == "LONG")   e.response = 2;
        else continue;

        e.k_dep     = to_double(tok[2]);
        e.k_0       = to_double(tok[3]);
        e.k_T       = to_double(tok[4]);
        e.T_ref     = to_double(tok[5]);
        e.theta_rec = to_double(tok[6]);
        e.T_freeze  = to_double(tok[7]);

        if (e.k_dep < 0.0 || e.k_0 < 0.0 || e.k_T < 0.0) continue;

        ctx.rdii_decay.add(e);
    }
}
```

**Registration:** [src/engine/plugins/DefaultInputPlugin.cpp:96](../src/engine/plugins/DefaultInputPlugin.cpp#L96) — add immediately after the `HYDROGRAPHS` registration:

```cpp
registry_.register_builtin("RDII_DECAY",   input::handle_rdii_decay);
```

### 5.4 Solver Resolution

In `RDIISolver::init(ctx)` ([src/engine/hydrology/RDII.cpp](../src/engine/hydrology/RDII.cpp), after the existing `[HYDROGRAPHS]` parameter population, before `validateExpDecay`):

```cpp
// Resolve [RDII_DECAY] entries onto per-group, per-response params.
for (const auto& e : ctx.rdii_decay.entries) {
    int uh_idx = findUnitHyd(e.uh_name);
    if (uh_idx < 0) continue;  // unknown group — could warn
    auto& dp = decay_params_[uh_idx][e.response];
    dp.active    = true;
    dp.k_dep     = e.k_dep;
    dp.k_0       = e.k_0;
    dp.k_T       = e.k_T;
    dp.T_ref     = e.T_ref;
    dp.theta_rec = e.theta_rec;
    dp.T_freeze  = e.T_freeze;
}
validateExpDecay(ctx);
```

### 5.5 IA Update — Per-Response Dispatch

In `RDIISolver::computeAll()`, the existing call to `applyIA()` becomes a dispatch:

```cpp
double excess = decay_params_[g][k].active
    ? updateIA_exp(uh, decay_params_[g][k], ia_avail_[g][k], rainfall, dt, ctx)
    : updateIA_linear(uh, month, k, ia_avail_[g][k], rainfall, dt);
```

The two helpers as drafted in the original plan, with `getTemperature` reading from `ctx.climate_state` (Fahrenheit→Celsius conversion at the call site) and falling back to `T_ref` when `ctx.options.temp_source == 0`.

### 5.6 Validation

`RDIISolver::validateExpDecay(ctx)` pushes a single warning into `ctx.warnings` (already exists at [SimulationContext.hpp:624](../src/engine/core/SimulationContext.hpp#L624)) when any decay row is active but `ctx.options.temp_source == 0`. The warning surfaces automatically through the report plugin header — no extra wiring needed.

### 5.7 INP Writer

In [src/engine/core/InpWriter.cpp](../src/engine/core/InpWriter.cpp), immediately after the `[HYDROGRAPHS]` block (after line 954), add:

```cpp
// [RDII_DECAY]
if (ctx.rdii_decay.count() > 0) { sec(f, "RDII_DECAY");
std::fprintf(f, ";;%-16s %-8s %-10s %-10s %-10s %-8s %-10s %-10s\n",
    "UHGroup","Response","k_dep","k_0","k_T","T_ref","theta_rec","T_freeze");
std::fprintf(f, ";;%-16s %-8s %-10s %-10s %-10s %-8s %-10s %-10s\n",
    "----------------","--------","----------","----------","----------",
    "--------","----------","----------");
static const char* uhResp[] = {"SHORT","MEDIUM","LONG"};
for (const auto& e : ctx.rdii_decay.entries) {
    std::fprintf(f, "%-16s %-8s %10.5f %10.5f %10.5f %8.2f %10.5f %10.5f\n",
        e.uh_name.c_str(), uhResp[e.response],
        e.k_dep, e.k_0, e.k_T, e.T_ref, e.theta_rec, e.T_freeze);
}}
```

### 5.8 GeoPackage Schema, Writer, Reader

**Schema** ([src/engine/input/geopackage/GeoPackageSchema.cpp](../src/engine/input/geopackage/GeoPackageSchema.cpp)), after the `unit_hydrographs` table (after line 380):

```sql
-- RDII exponential decay parameters (one row per UH group × response)
CREATE TABLE IF NOT EXISTS rdii_decay (
    fid             INTEGER PRIMARY KEY AUTOINCREMENT,
    simulation_id   TEXT NOT NULL,
    uh_name         TEXT NOT NULL,
    response        TEXT NOT NULL,   -- 'SHORT' | 'MEDIUM' | 'LONG'
    k_dep           REAL NOT NULL,
    k_0             REAL NOT NULL,
    k_T             REAL NOT NULL,
    T_ref           REAL NOT NULL,
    theta_rec       REAL NOT NULL,
    T_freeze        REAL NOT NULL,
    UNIQUE(simulation_id, uh_name, response)
);
```

**Writer** ([src/engine/input/geopackage/GeoPackageWriter.cpp](../src/engine/input/geopackage/GeoPackageWriter.cpp)): extend `write_rdii()` (around line 986) to also write the `rdii_decay` table from `ctx.rdii_decay.entries`. Call site at line 1118 needs no change.

**Reader** ([src/engine/input/geopackage/GeoPackageReader.cpp](../src/engine/input/geopackage/GeoPackageReader.cpp)): extend `read_rdii()` (around line 930) to read the `rdii_decay` table — guarded by `table_exists(db, "rdii_decay")` so older GeoPackages still load. Populates `ctx.rdii_decay`.

### 5.9 Output Plugin

No change. Binary output already exposes RDII as `SYS_IIFLOW` ([src/engine/plugins/DefaultOutputPlugin.cpp:233](../src/engine/plugins/DefaultOutputPlugin.cpp#L233)) — same channel, same units.

### 5.10 Report Plugin

[src/engine/plugins/DefaultReportPlugin.cpp:490](../src/engine/plugins/DefaultReportPlugin.cpp#L490) — distinguish the IA model in the simulation-options summary:

```cpp
bool has_exp = ctx.rdii_decay.count() > 0;
std::fprintf(f, "\n    RDII ................... %s",
    has_rdii ? (has_exp ? "YES (Exponential IA)" : "YES (Linear IA)") : "NO");
```

Continuity rows ([DefaultReportPlugin.cpp:602](../src/engine/plugins/DefaultReportPlugin.cpp#L602)) need no change — same `mb.routing_rdii` accumulator, populated by the unchanged `applyRdiiInflows()`.

---

## 6. Call Sequence Summary

```
InputReader::parse()
    ├── SectionRegistry::dispatch("HYDROGRAPHS")  → handle_hydrographs   [unchanged]
    └── SectionRegistry::dispatch("RDII_DECAY")   → handle_rdii_decay    [NEW]

RDIISolver::init(ctx)
    ├── populate uh_params from ctx.unit_hyds       [unchanged]
    ├── resolve ctx.rdii_decay onto decay_params_   [NEW]
    └── validateExpDecay(ctx)                       [NEW — pushes ctx.warnings]

RDIISolver::computeAll(ctx, rainfall, month, dt)   [each routing step]
    └── for each group g, response k:
          ├── if decay_params_[g][k].active:
          │     └── updateIA_exp(uh, dp, ia_avail, rain, dt, ctx)  [NEW]
          │           ├── reads ctx.climate_state.temperature
          │           └── getRecoveryRate(dp, T)
          └── else: updateIA_linear(uh, m, k, ia_avail, rain, dt)  [refactored — existing logic]

InpWriter::write(ctx, path)
    ├── write [HYDROGRAPHS]   [unchanged]
    └── write [RDII_DECAY]    [NEW]

GeoPackageWriter::write(ctx, db)
    ├── write rdii_assignments    [unchanged]
    ├── write unit_hydrographs    [unchanged]
    └── write rdii_decay          [NEW]

GeoPackageReader::read(db, ctx)
    ├── read rdii_assignments         [unchanged]
    ├── read unit_hydrographs         [unchanged]
    └── if table_exists("rdii_decay"): read rdii_decay   [NEW]
```

---

## 7. Files Touched

| File | Change | Phase |
|---|---|---|
| [src/engine/data/InflowData.hpp](../src/engine/data/InflowData.hpp) | Add `RDIIDecayEntry`, `RDIIDecayData` | 1 |
| [src/engine/core/SimulationContext.hpp](../src/engine/core/SimulationContext.hpp) | Add `rdii_decay`; move `climate_state` here | 1 |
| [src/engine/core/SWMMEngine.hpp](../src/engine/core/SWMMEngine.hpp) | Remove `climate_` member | 1 |
| [src/engine/core/SWMMEngine.cpp](../src/engine/core/SWMMEngine.cpp) | Rename `climate_.` → `ctx_.climate_state.` (~40 sites) | 1 |
| [src/engine/input/handlers/InflowsHandler.hpp](../src/engine/input/handlers/InflowsHandler.hpp) | Declare `handle_rdii_decay` | 2 |
| [src/engine/input/handlers/InflowsHandler.cpp](../src/engine/input/handlers/InflowsHandler.cpp) | Define `handle_rdii_decay` | 2 |
| [src/engine/plugins/DefaultInputPlugin.cpp](../src/engine/plugins/DefaultInputPlugin.cpp) | Register `RDII_DECAY` | 2 |
| [src/engine/core/InpWriter.cpp](../src/engine/core/InpWriter.cpp) | Write `[RDII_DECAY]` block | 2 |
| [src/engine/input/geopackage/GeoPackageSchema.cpp](../src/engine/input/geopackage/GeoPackageSchema.cpp) | Add `rdii_decay` table | 2 |
| [src/engine/input/geopackage/GeoPackageWriter.cpp](../src/engine/input/geopackage/GeoPackageWriter.cpp) | Write `rdii_decay` | 2 |
| [src/engine/input/geopackage/GeoPackageReader.cpp](../src/engine/input/geopackage/GeoPackageReader.cpp) | Read `rdii_decay` (guarded) | 2 |
| [src/engine/hydrology/RDII.hpp](../src/engine/hydrology/RDII.hpp) | Decay-param storage + helper decls | 3 |
| [src/engine/hydrology/RDII.cpp](../src/engine/hydrology/RDII.cpp) | `updateIA_exp`, `updateIA_linear`, `validateExpDecay`, dispatch | 3 |
| [src/engine/plugins/DefaultReportPlugin.cpp](../src/engine/plugins/DefaultReportPlugin.cpp) | Distinguish Linear vs Exponential in summary | 4 |
| `tests/unit/engine/test_rdii_decay.cpp` (new) | Unit tests | 4 |

---

## 8. Implementation Phases

```
Phase 1 — Plumbing
  ├── Move ClimateState to SimulationContext (mechanical rename in SWMMEngine.cpp)
  ├── Add RDIIDecayData SoA in InflowData.hpp + ctx field
  └── Verify existing tests pass (no behavioural change yet)

Phase 2 — IO Round-Trip
  ├── handle_rdii_decay + registry entry
  ├── InpWriter [RDII_DECAY] block
  ├── GeoPackage schema + writer + reader
  └── Test: INP → ctx → INP and INP → ctx → GeoPackage → ctx round-trip equality

Phase 3 — Solver Behaviour
  ├── Decay-param storage in RDIISolver
  ├── updateIA_exp / updateIA_linear split + dispatch
  ├── validateExpDecay warning
  └── Test: known rainfall + known params → expected excess; linear path unchanged

Phase 4 — Reporting & Tests
  ├── Report plugin summary line
  ├── Unit tests (frozen ground, recovery asymptote, dispatch toggle)
  └── Smoke test: an RTK model with monthly R replaced by single R + decay produces
      qualitatively correct seasonal RDII when run on a multi-year temperature record
```

Phases 1 and 2 are pure plumbing — completing them gives you the round-trip without changing any simulation results. Phase 3 is where behaviour changes, gated entirely by whether `[RDII_DECAY]` rows exist for a group.

---

## 9. Calibration Strategy

### 9.1 Migration from Monthly RTK Calibration

**Step 1 — Identify the true infrastructure R.** Examine calibrated monthly R values. The minimum R across months represents the driest antecedent condition where IA is largely restored and the monthly R value is closest to the true leakage fraction. Adopt this minimum R as the single invariant R value.

**Step 2 — Estimate IAmax from the R range.** The difference between maximum and minimum monthly R represents the IA effect that has been absorbed into R. Work backward from the mean storm depth of the calibration period to estimate the IA depth this R difference corresponds to:

```
IAmax ~ (R_max - R_min) / R_min * mean_storm_depth
```

**Step 3 — Initialise k_dep.** Rule-of-thumb starting value:

```
k_dep ~ 1.0 / IAmax    [complete depletion at one IAmax depth of rainfall]
```

**Step 4 — Initialise k_0 from cool-season events.** Identify pairs of storms in cool-season months (low ET) separated by 5–15 days. The ratio of RDII response between successive events at similar rainfall depths gives an estimate of the IA recovery fraction over the intervening dry period attributable to gravity drainage alone — this calibrates k_0.

**Step 5 — Initialise k_T from warm-season recovery.** Repeat Step 4 for warm-season events. The additional recovery beyond what k_0 predicts is attributable to the thermal term. With T and T_ref known, k_T can be estimated directly.

**Step 6 — Set T_ref to mean annual air temperature.** This ensures k_T is calibrated at a representative operating condition.

**Step 7 — Calibrate theta_rec from seasonal residuals.** Run with theta_rec = 0 first (k_rec = k_0 + k_T regardless of temperature). Residuals that are systematically positive in winter and negative in summer indicate insufficient seasonal signal — increase theta_rec. Residuals with no seasonal pattern indicate theta_rec = 0 is sufficient.

**Step 8 — Confirm T_freeze.** 0 deg C is the physically appropriate default. In climates with freeze-thaw cycles, RDII response in February and March should be examined — if spring peaks are under-predicted, T_freeze may need to be raised slightly.

### 9.2 Parameter Identifiability

`k_0` and `k_T` are best separated by comparing cool-season and warm-season inter-event IA recovery. If only single-season data is available, set `k_T = 0` and calibrate `k_0` alone — the model then applies temperature-independent exponential decay, still superior to the linear model.

`theta_rec` is only identifiable from multi-year records spanning contrasting seasonal conditions. For single-event or single-season calibration, set `theta_rec = 0`. Seasonal sensitivity can be added when multi-year data is available.

### 9.3 Parameter Summary

| Parameter | Typical Range | Physical Meaning |
|---|---|---|
| `k_dep` | 0.05 – 0.30 (1/mm); 1.3 – 7.6 (1/in) | IA exhaustion rate per unit depth of rainfall (project units) |
| `k_0` | 0.005 – 0.03 (1/hr) | Base recovery rate (gravity drainage, capillary redistribution) |
| `k_T` | 0.005 – 0.12 (1/hr) | Thermal recovery rate at T_ref (ET-driven drying) |
| `T_ref` | Mean annual T of catchment | Anchor for k_T calibration |
| `theta_rec` | 0.03 – 0.10 (1/deg C) | Seasonal sensitivity of thermal recovery; 0 for isothermal |
| `T_freeze` | 0 deg C default | Frozen ground recovery threshold |

---

## 10. Hot-Start Persistence

No format change required. Both linear and exponential models use the same runtime state (`ia_used` per group per response). The decay parameters are fixed inputs stored in `ctx.rdii_decay`, not evolving state. A hot-start file written under the linear model initialises the exponential model correctly from persisted `ia_used` values, and vice versa. The two models are fully interchangeable at restart boundaries.
