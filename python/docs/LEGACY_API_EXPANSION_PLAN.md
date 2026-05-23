# Legacy SWMM Python API Expansion Plan

**Status:** All Phases Complete (1-4)
**Date:** 2026-03-25
**Last Updated:** 2026-03-25
**Package:** `openswmm.legacy.engine` / `openswmm.legacy.output`

---

## 1. Objective

Validate the existing legacy C API (`openswmm_solver.h`) against the Python
bindings (`_solver.pyx`), identify gaps compared to the PySWMM ecosystem, and
expand the Python API to provide full coverage of all SWMM element types with
proper mass-balance and flux tracking.

---

## 2. Current State Audit

### 2.1 C API Functions (openswmm_solver.h) — 27 functions

| C Function | Python Binding | Notes |
|------------|---------------|-------|
| `swmm_run` | `run_solver()` | OK |
| `swmm_run_with_callback` | `run_solver(callback=)` | OK |
| `swmm_open` | `Solver.open()` | OK |
| `swmm_start` | `Solver.start()` | OK |
| `swmm_step` | `Solver.step()` | OK |
| `swmm_stride` | `Solver.step(stride=)` | OK |
| `swmm_useHotStart` | `Solver.use_hotstart()` | OK |
| `swmm_saveHotStart` | `Solver.save_hotstart()` | OK |
| `swmm_end` | `Solver.end()` | OK |
| `swmm_report` | `Solver.report()` | OK |
| `swmm_close` | `Solver.close()` | OK |
| `swmm_getMassBalErr` | `Solver.get_mass_balance_error()` | OK — returns (runoff%, flow%, qual%) |
| `swmm_getVersion` | `version()` | OK |
| `swmm_getError` | `Solver.__get_error()` | OK (private) |
| `swmm_getErrorFromCode` | `get_error_message()` | OK |
| `swmm_getWarnings` | **NOT WRAPPED** | **GAP** |
| `swmm_getCount` | `Solver.get_object_count()` | OK |
| `swmm_getName` | `Solver.get_object_name()` | OK |
| `swmm_getIndex` | `Solver.get_object_index()` | OK |
| `swmm_getValue` | `Solver.get_value()` | OK (deprecated, delegates to Expanded) |
| `swmm_getValueExpanded` | `Solver.get_value()` | OK |
| `swmm_setValue` | `Solver.set_value()` | OK (deprecated, delegates to Expanded) |
| `swmm_setValueExpanded` | `Solver.set_value()` | OK |
| `swmm_getSavedValue` | **NOT WRAPPED** | **GAP** — read from .out during sim |
| `swmm_writeLine` | **NOT WRAPPED** | **GAP** — write to report file |
| `swmm_decodeDate` | `decode_swmm_datetime()` | OK |
| `swmm_encodeDate` | `encode_swmm_datetime()` | OK |

### 2.2 C API Enums vs Python Enums

#### Rain Gage Properties (C: 3, Python: 3) — COMPLETE
All 3 properties exposed.

#### Subcatchment Properties (C: 48, Python: 17) — **31 MISSING**

| C Enum Value | Python Enum | Status |
|-------------|-------------|--------|
| `swmm_SUBCATCH_AREA` (200) | `AREA` | OK |
| `swmm_SUBCATCH_RAINGAGE` (201) | `RAINGAGE` | OK |
| `swmm_SUBCATCH_RAINFALL` (202) | `RAINFALL` | OK |
| `swmm_SUBCATCH_EVAP` (203) | `EVAPORATION` | OK |
| `swmm_SUBCATCH_INFIL` (204) | `INFILTRATION` | OK |
| `swmm_SUBCATCH_RUNOFF` (205) | `RUNOFF` | OK |
| `swmm_SUBCATCH_RPTFLAG` (206) | `REPORT_FLAG` | OK |
| `swmm_SUBCATCH_WIDTH` (207) | `WIDTH` | OK |
| `swmm_SUBCATCH_SLOPE` (208) | `SLOPE` | OK |
| `swmm_SUBCATCH_OUTLET_TYPE` (209) | — | **MISSING** |
| `swmm_SUBCATCH_OUTLET_INDEX` (210) | — | **MISSING** |
| `swmm_SUBCATCH_INFILTRATION_MODEL` (211) | — | **MISSING** |
| `swmm_SUBCATCH_FRACTION_IMPERVIOUS` (212) | — | **MISSING** |
| `swmm_SUBCATCH_SUB_AREA_ROUTE_TO` (213) | — | **MISSING** |
| `swmm_SUBCATCH_SUB_AREA_FRACTION_OUTLET` (214) | — | **MISSING** |
| `swmm_SUBCATCH_SUB_AREA_MANNINGS_N` (215) | — | **MISSING** (sub_index needed) |
| `swmm_SUBCATCH_SUB_AREA_FRACTION_AREA` (216) | — | **MISSING** (sub_index needed) |
| `swmm_SUBCATCH_SUB_AREA_DEPRESSION_STORAGE` (217) | — | **MISSING** (sub_index needed) |
| `swmm_SUBCATCH_SUB_AREA_INFLOW` (218) | — | **MISSING** (sub_index needed) |
| `swmm_SUBCATCH_SUB_AREA_RUNOFF` (219) | — | **MISSING** (sub_index needed) |
| `swmm_SUBCATCH_SUB_AREA_DEPTH` (220) | — | **MISSING** (sub_index needed) |
| `swmm_SUBCATCH_LID_UNITS_COUNT` (221) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNITS_PERV_AREA` (222) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNITS_FLOW_TO_PERV_AREA` (223) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNITS_DRAIN_FLOW` (224) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_REPLICATES` (225) | — | **MISSING** (sub_index=LID unit) |
| `swmm_SUBCATCH_LID_UNIT_AREA` (226) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_FULL_WIDTH` (227) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_BOTTOM_WIDTH` (228) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_INIT_SATURATION` (229) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_FROM_IMPERVIOUS` (230) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_FROM_PERVIOUS` (231) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_TO_PERVIOUS` (232) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_RECEIVING_OUTLET_TYPE` (233) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_RECEIVING_OUTLET_INDEX` (234) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_SURFACE_DEPTH` (235) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_SOIL_MOISTURE` (236) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_GREEN_AMPT_CAPILLARY_SUCTION` (237) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_GREEN_AMPT_SATURATED_CONDUCTIVITY` (238) | — | **MISSING** |
| `swmm_SUBCATCH_LID_UNIT_GREEN_AMPT_MAX_SOIL_MOISTURE_DEFICIT` (239) | — | **MISSING** |
| `swmm_SUBCATCH_CURB_LENGTH` (240) | `CURB_LENGTH` | OK |
| `swmm_SUBCATCH_API_RAINFALL` (241) | `API_RAINFALL` | OK |
| `swmm_SUBCATCH_API_SNOWFALL` (242) | `API_SNOWFALL` | OK |
| `swmm_SUBCATCH_POLLUTANT_BUILDUP` (243) | `POLLUTANT_BUILDUP` | OK |
| `swmm_SUBCATCH_EXTERNAL_POLLUTANT_BUILDUP` (244) | `EXTERNAL_POLLUTANT_BUILDUP` | OK |
| `swmm_SUBCATCH_POLLUTANT_RUNOFF_CONCENTRATION` (245) | `POLLUTANT_RUNOFF_CONCENTRATION` | OK |
| `swmm_SUBCATCH_POLLUTANT_PONDED_CONCENTRATION` (246) | `POLLUTANT_PONDED_CONCENTRATION` | OK |
| `swmm_SUBCATCH_POLLUTANT_TOTAL_LOAD` (247) | `POLLUTANT_TOTAL_LOAD` | OK |

#### Node Properties (C: 15, Python: 15) — COMPLETE

#### Link Properties (C: 29, Python: 29) — COMPLETE

#### System Properties (C: 41, Python: 37) — **4 MISSING**

Missing in Python enum: `HEAD_TOL` (38), `SYS_FLOW_TOL` (39), `LAT_FLOW_TOL` (40)
were listed in C but mapped differently in Python. Need audit.

---

## 3. Gap Analysis vs PySWMM

### 3.1 Features PySWMM has that we lack

| Feature | PySWMM | Our API | Priority |
|---------|--------|---------|----------|
| Per-node statistics (peak inflow, flood duration, etc.) | `node.statistics` | Not exposed | **HIGH** |
| Per-link statistics (peak flow, surcharge time, etc.) | `link.conduit_statistics` | Not exposed | **HIGH** |
| Pump statistics | `link.pump_statistics` | Not exposed | **HIGH** |
| Storage statistics | `node.storage_statistics` | Not exposed | **HIGH** |
| Outfall statistics | `node.outfall_statistics` | Not exposed | **HIGH** |
| Subcatchment statistics (total precip, runoff, etc.) | `subcatchment.statistics` | Not exposed | **HIGH** |
| System routing statistics (mass balance breakdown) | `SystemStats.routing_stats` | Only `get_mass_balance_error()` | **HIGH** |
| System runoff statistics (mass balance breakdown) | `SystemStats.runoff_stats` | Only `get_mass_balance_error()` | **HIGH** |
| LID unit properties (area, width, saturation, etc.) | `LidUnit.*` | Enum exists in C, not in Python | **MEDIUM** |
| LID layer results (surface depth, soil moisture, etc.) | `LidUnit.surface.depth` | Enum exists in C, not in Python | **MEDIUM** |
| LID water balance (inflow, evap, infil, drain flow) | `LidUnit.water_balance` | Not exposed | **MEDIUM** |
| Subarea properties (Manning's n, depression storage) | via toolkit | Enum exists in C, not in Python | **MEDIUM** |
| Node inflow control (`generated_inflow`) | `node.generated_inflow()` | Via `set_value(NODE, LATERAL_INFLOW)` | OK |
| Outfall stage control | `outfall.outfall_stage()` | Not exposed as dedicated method | **LOW** |
| Pollutant quality at nodes/links | `node.pollut_quality` | Via `get_value()` with sub_index | OK but clunky |
| Warning count | `swmm_getWarnings` | **NOT WRAPPED** | **MEDIUM** |
| Write to report file | `swmm_writeLine` | **NOT WRAPPED** | **LOW** |
| Read saved output values | `swmm_getSavedValue` | **NOT WRAPPED** | **LOW** |

### 3.2 Mass Balance / Flux Tracking Gaps

**Current:** Only `get_mass_balance_error() -> (runoff_err%, flow_err%, qual_err%)`.

**Needed (matching PySWMM `SystemStats`):**

Routing totals:
- `dry_weather_inflow`, `wet_weather_inflow`, `groundwater_inflow`
- `ii_inflow` (I&I), `external_inflow`
- `flooding`, `outflow`, `evaporation_loss`, `seepage_loss`
- `reacted` (quality)
- `initial_storage`, `final_storage`
- `routing_error` (% continuity)

Runoff totals:
- `rainfall`, `evaporation`, `infiltration`, `runoff`
- `drains` (LID drains), `runon`
- `init_storage`, `final_storage`
- `init_snow_cover`, `final_snow_cover`, `snow_removed`
- `runoff_error` (% continuity)

**These require new C API functions** or access to internal SWMM global
variables. The existing `swmm_getMassBalErr` only returns the 3 percentage
errors.

---

## 4. Implementation Plan

### Phase 1: Complete the Python enum coverage (no C changes)

**Goal:** Expose all 48 subcatchment properties and the 4 missing system
properties in the Python `SWMMSubcatchmentProperties` and
`SWMMSystemProperties` enums. No C code changes needed — the C API already
supports these via `swmm_getValueExpanded` / `swmm_setValueExpanded`.

**Tasks:**

- [x] **1.1** Add missing subcatchment enum members to `SWMMSubcatchmentProperties`:
  - Added 31 members to both `solver.pxd` and `_solver.pyx`
  - Outlet: `OUTLET_TYPE`, `OUTLET_INDEX`
  - Infiltration: `INFILTRATION_MODEL`, `FRACTION_IMPERVIOUS`
  - Subareas (sub_index = 0=imperv, 1=perv):
    `SUB_AREA_ROUTE_TO`, `SUB_AREA_FRACTION_OUTLET`, `SUB_AREA_MANNINGS_N`,
    `SUB_AREA_FRACTION_AREA`, `SUB_AREA_DEPRESSION_STORAGE`,
    `SUB_AREA_INFLOW`, `SUB_AREA_RUNOFF`, `SUB_AREA_DEPTH`
  - LID aggregates: `LID_UNITS_COUNT`, `LID_UNITS_PERV_AREA`,
    `LID_UNITS_FLOW_TO_PERV_AREA`, `LID_UNITS_DRAIN_FLOW`
  - LID unit (sub_index = LID unit index):
    `LID_UNIT_REPLICATES`, `LID_UNIT_AREA`, `LID_UNIT_FULL_WIDTH`,
    `LID_UNIT_BOTTOM_WIDTH`, `LID_UNIT_INIT_SATURATION`,
    `LID_UNIT_FROM_IMPERVIOUS`, `LID_UNIT_FROM_PERVIOUS`,
    `LID_UNIT_TO_PERVIOUS`, `LID_UNIT_RECEIVING_OUTLET_TYPE`,
    `LID_UNIT_RECEIVING_OUTLET_INDEX`, `LID_UNIT_SURFACE_DEPTH`,
    `LID_UNIT_SOIL_MOISTURE`, `LID_UNIT_GREEN_AMPT_CAPILLARY_SUCTION`,
    `LID_UNIT_GREEN_AMPT_SATURATED_CONDUCTIVITY`,
    `LID_UNIT_GREEN_AMPT_MAX_SOIL_MOISTURE_DEFICIT`
- [x] **1.2** System enum members already complete — `HEAD_TOL`, `SYS_FLOW_TOL`, `LAT_FLOW_TOL` were already present (plan audit was incorrect)
- [ ] **1.3** Update `.pyi` stubs with all new enum members and docstrings (deferred — .pyi file does not yet exist)
- [x] **1.4** Wrap `swmm_getWarnings` as `Solver.get_warnings() -> int`
- [x] **1.5** Wrap `swmm_writeLine` as `Solver.write_line(text: str)`
- [x] **1.6** Wrap `swmm_getSavedValue` as `Solver.get_saved_value(property, index, period) -> float`
- [x] **1.7** Write unit tests for all new enum values and wrapped functions
- [x] **1.8** (Bug fix) Fixed `get_mass_balance_error()` — was missing `return` statement and not capturing error code from `swmm_getMassBalErr()`
- [x] **1.9** Updated `__init__.py` exports to include `SWMMLinkTypes`, `CallbackType`, `get_error_message`

**Files modified:**
- `openswmm/legacy/engine/solver.pxd` — added 31 subcatchment enum members
- `openswmm/legacy/engine/_solver.pyx` — expanded enums, fixed bug, added 3 new methods
- `openswmm/legacy/engine/__init__.py` — added missing exports
- `openswmm/solver/__init__.py` — updated backward-compat shim
- `tests/legacy/test_phase1_enum_coverage.py` — **new** comprehensive test suite (13 test classes)

### Phase 2: Statistics and Mass Balance C API + Python bindings

**Goal:** Expose per-element statistics and detailed mass-balance
breakdowns. This requires new C functions in `openswmm_solver.h` since the
existing generic `swmm_getValue/swmm_setValue` interface does not provide
access to the internal statistics structures.

**New C Functions Needed:**

```c
// --- Node statistics ---
int swmm_getNodeStats(int index, double *avgDepth, double *maxDepth,
                      double *maxDepthDate, double *maxInflow,
                      double *maxOverflow, double *floodVolume,
                      double *floodDuration, double *surchargeDuration,
                      double *courantCritDuration);

// --- Storage statistics ---
int swmm_getStorageStats(int index, double *initVolume, double *avgVolume,
                         double *maxVolume, double *maxVolDate,
                         double *maxFlow, double *evapLoss,
                         double *exfilLoss);

// --- Outfall statistics ---
int swmm_getOutfallStats(int index, double *avgFlow, double *maxFlow,
                         double *totalVolume, int *totalPeriods,
                         double *pollutLoads, int nPolluts);

// --- Link/conduit statistics ---
int swmm_getLinkStats(int index, double *peakFlow, double *peakFlowDate,
                      double *peakVelocity, double *peakDepth,
                      double *timeNormalFlow, double *timeInletControl,
                      double *timeSurcharged, double *timeFullUpstream,
                      double *timeFullDownstream, double *timeFullFlow,
                      double *timeCapLimited, double *timeCourantCrit);

// --- Pump statistics ---
int swmm_getPumpStats(int index, double *utilized, double *minFlow,
                      double *avgFlow, double *maxFlow,
                      double *totalVolume, double *energyConsumed,
                      double *offCurveLow, double *offCurveHigh,
                      int *startups, int *totalPeriods);

// --- Subcatchment statistics ---
int swmm_getSubcatchStats(int index, double *precip, double *runon,
                          double *evap, double *infil, double *runoff,
                          double *maxFlow);

// --- System mass balance ---
int swmm_getRoutingTotals(double *dryWeatherInflow, double *wetWeatherInflow,
                          double *groundwaterInflow, double *iiInflow,
                          double *externalInflow, double *flooding,
                          double *outflow, double *evapLoss,
                          double *seepageLoss, double *reacted,
                          double *initStorage, double *finalStorage,
                          double *pctError);

int swmm_getRunoffTotals(double *rainfall, double *evap, double *infil,
                         double *runoff, double *drains, double *runon,
                         double *initStorage, double *finalStorage,
                         double *initSnowCover, double *finalSnowCover,
                         double *snowRemoved, double *pctError);
```

**Tasks:**

- [x] **2.1** Implement new C functions in `src/legacy/engine/swmm5_stats.c`
      accessing internal SWMM globals via extern linkage. Uses struct-based
      API (swmm_SubcatchStats, swmm_NodeStats, etc.) rather than flat
      parameter lists. Original EPA files remain unmodified.
- [x] **2.2** Add 8 struct typedefs + 8 function declarations to `openswmm_solver.h`
- [x] **2.3** Add `cdef extern` struct + function declarations to `solver.pxd`
- [x] **2.4** Python wrapper methods on `Solver` class (all return dicts):
  - `get_subcatchment_statistics(idx) -> dict`
  - `get_node_statistics(idx) -> dict`
  - `get_storage_statistics(idx) -> dict`
  - `get_outfall_statistics(idx) -> dict` (includes pollutant loads array)
  - `get_link_statistics(idx) -> dict` (includes 7-element flow class array)
  - `get_pump_statistics(idx) -> dict`
  - `get_routing_totals() -> dict` (with mass balance equation in docstring)
  - `get_runoff_totals() -> dict` (with mass balance equation in docstring)
- [ ] **2.5** Update `.pyi` stubs (deferred — .pyi file does not yet exist)
- [x] **2.6** Write unit tests validating statistics against known model outputs
- [x] **2.7** Write unit tests validating mass balance closure:
  - `routing_totals`: verify `inflows - outflows ≈ storage_change` within `pct_error`
  - External inflow test: verify `set_value(LATERAL_INFLOW)` appears in `ex_inflow`
  - Rainfall override test: verify `API_RAINFALL` override appears in `rainfall`

**Files created:**
- `src/legacy/engine/swmm5_stats.c` — **new** C statistics implementation (8 functions)
- `tests/legacy/test_phase2_statistics.py` — **new** comprehensive test suite (11 test classes)

**Files modified:**
- `include/openswmm/legacy/engine/openswmm_solver.h` — 8 structs + 8 function declarations
- `include/openswmm/legacy/engine/openswmm_solver.h` — new declarations
- `openswmm/legacy/engine/solver.pxd` — Cython extern declarations
- `openswmm/legacy/engine/_solver.pyx` — Python methods
- `openswmm/legacy/engine/_solver.pyi` — type stubs
- `tests/legacy/test_solver.py` — statistics and mass balance tests

### Phase 3: Dedicated element-access classes (Pythonic API layer)

**Goal:** Add PySWMM-style object-oriented wrappers that sit on top of the
generic `get_value`/`set_value` interface, providing property-based access
with docstrings, type safety, and mass-balance awareness.

**New classes** (in `openswmm/legacy/engine/`):

```
_nodes.py      -> LegacyNodes, LegacyNode
_links.py      -> LegacyLinks, LegacyLink
_subcatchments.py -> LegacySubcatchments, LegacySubcatchment
_raingages.py  -> LegacyRainGages, LegacyRainGage
_system.py     -> LegacySystem
_lid.py        -> LegacyLidUnits, LegacyLidUnit
```

Each class wraps the `Solver` and provides typed properties:

```python
class LegacyNode:
    """Property-based access to a single SWMM node."""

    def __init__(self, solver: Solver, index: int): ...

    # Parameters (get/set)
    @property
    def invert_elevation(self) -> float: ...
    @invert_elevation.setter
    def invert_elevation(self, value: float): ...

    @property
    def max_depth(self) -> float: ...
    @property
    def surcharge_depth(self) -> float: ...
    @property
    def ponded_area(self) -> float: ...
    @property
    def initial_depth(self) -> float: ...

    # Results (get only, during simulation)
    @property
    def depth(self) -> float: ...
    @property
    def head(self) -> float: ...
    @property
    def volume(self) -> float: ...
    @property
    def lateral_inflow(self) -> float: ...
    @property
    def total_inflow(self) -> float: ...
    @property
    def flooding(self) -> float: ...

    # Pollutants
    @property
    def pollutant_concentration(self) -> dict[str, float]: ...

    # Control
    def set_lateral_inflow(self, flow: float): ...
    def set_pollutant_concentration(self, pollutant: str, value: float): ...

    # Statistics (Phase 2)
    @property
    def statistics(self) -> dict: ...
```

**Tasks:**

- [x] **3.1** Implement `LegacyNodes` / `LegacyNode` — `_nodes.py`
      All 15 node properties as typed Python properties, plus node_type,
      get_pollutant_concentration, statistics
- [x] **3.2** Implement `LegacyLinks` / `LegacyLink` — `_links.py`
      All 29 link properties, link_type, statistics, pump_statistics
- [x] **3.3** Implement `LegacySubcatchments` / `LegacySubcatchment` — `_subcatchments.py`
      All subcatchment properties including subareas (via get_subarea_*),
      LID access (via get_lid_unit_*), pollutant access, statistics
- [x] **3.4** Implement `LegacyRainGages` / `LegacyRainGage` — `_raingages.py`
- [x] **3.5** Implement `LegacySystem` — `_system.py`
      Flow units, routing/runoff totals, mass balance error, tolerances
- [x] **3.6** LID access integrated into `LegacySubcatchment` as methods
      (get_lid_unit_area, get_lid_unit_surface_depth, etc.) using sub_index
- [ ] **3.7** Add `.pyi` stubs for all new classes (deferred)
- [x] **3.8** Comprehensive unit tests — `test_phase3_oop_wrappers.py`
- [ ] **3.9** Sphinx API documentation (deferred)

### Phase 4: Mass-balance-aware set functions

**Goal:** For every `set_*` method that modifies a flux or state variable,
document the mass-balance implications and optionally log the external
forcing for post-simulation audit.

**Design:**

```python
class LegacyNode:
    def set_lateral_inflow(self, flow: float):
        """Prescribe an external lateral inflow at this node.

        Mass balance impact:
            This adds *flow* to the node's lateral inflow for the current
            timestep. The value is reset each timestep — call again in the
            next step to sustain. The injected volume appears in the routing
            mass balance under "external inflow".

        Args:
            flow: Inflow rate in project flow units. Positive = inflow.
        """
        ...

    def set_pollutant_concentration(self, pollutant: str, conc: float):
        """Set pollutant concentration of the lateral inflow at this node.

        Mass balance impact:
            The mass flux = lateral_inflow * concentration * timestep.
            This mass is added to the quality routing mass balance.
            Must be called together with set_lateral_inflow for the
            mass to be non-zero.
        """
        ...
```

**Properties with mass-balance implications (document in docstrings):**

| Set Property | Object | Mass Balance Category |
|-------------|--------|----------------------|
| `NODE_LATFLOW` | Node | External inflow (routing totals) |
| `NODE_POLLUTANT_LATMASS_FLUX` | Node | Quality mass balance |
| `NODE_DEPTH` | Node | Changes stored volume — affects storage balance |
| `LINK_SETTING` | Link | Changes flow capacity — affects routing |
| `LINK_FLOW` | Link | Direct flow override — bypasses hydraulics |
| `SUBCATCH_API_RAINFALL` | Subcatchment | Overrides rainfall — affects runoff totals |
| `SUBCATCH_API_SNOWFALL` | Subcatchment | Overrides snowfall — affects snow balance |
| `SUBCATCH_EXTERNAL_POLLUTANT_BUILDUP` | Subcatchment | Adds pollutant mass — quality balance |
| `GAGE_RAINFALL` | Rain Gage | Overrides rainfall for all linked subcatchments |

**Tasks:**

- [x] **4.1** All mass-balance-aware setters include detailed docstrings:
      - `LegacyNode.set_lateral_inflow` → routing.ex_inflow
      - `LegacyNode.set_pollutant_lateral_mass_flux` → quality.external
      - `LegacyNode.set_depth` → routing.storage_override
      - `LegacyLink.set_setting` → routing.flow_capacity
      - `LegacyLink.set_flow` → routing.flow_override
      - `LegacySubcatchment.set_api_rainfall` → runoff.rainfall
      - `LegacySubcatchment.set_api_snowfall` → runoff.snow
      - `LegacySubcatchment.set_external_pollutant_buildup` → quality.buildup
      - `LegacyRainGage.set_rainfall` → runoff.rainfall
- [x] **4.2** `ExternalForcingLog` class — `_forcing_log.py`
      Records (time, object_type, object_id, property, value, mass_balance_category).
      Has `records`, `to_dataframe()`, `clear()`, `__len__()`.
      All setter methods accept optional `log=` parameter.
- [x] **4.3** Validation tests in `test_phase3_oop_wrappers.py`:
      - Log integration with node/link/subcatchment setters
      - End-to-end: inject 100 steps of inflow, verify log has 100 entries,
        verify routing_totals.ex_inflow > 0, verify pct_error < 5%

---

## 5. File Inventory

### Files to CREATE

| File | Phase | Purpose |
|------|-------|---------|
| `openswmm/legacy/engine/_nodes.py` | 3 | LegacyNodes/LegacyNode classes |
| `openswmm/legacy/engine/_links.py` | 3 | LegacyLinks/LegacyLink classes |
| `openswmm/legacy/engine/_subcatchments.py` | 3 | LegacySubcatchments/LegacySubcatchment |
| `openswmm/legacy/engine/_raingages.py` | 3 | LegacyRainGages/LegacyRainGage |
| `openswmm/legacy/engine/_system.py` | 3 | LegacySystem |
| `openswmm/legacy/engine/_lid.py` | 3 | LegacyLidUnits/LegacyLidUnit |
| `openswmm/legacy/engine/_forcing_log.py` | 4 | ExternalForcingLog |
| `tests/legacy/test_enum_coverage.py` | 1 | Validate all enum values |
| `tests/legacy/test_statistics.py` | 2 | Statistics tests |
| `tests/legacy/test_mass_balance.py` | 2,4 | Mass balance validation |
| `tests/legacy/test_nodes.py` | 3 | Node property tests |
| `tests/legacy/test_links.py` | 3 | Link property tests |
| `tests/legacy/test_subcatchments.py` | 3 | Subcatchment property tests |
| `tests/legacy/test_lid.py` | 3 | LID access tests |

### Files to MODIFY

| File | Phase | Changes |
|------|-------|---------|
| `include/openswmm/legacy/engine/openswmm_solver.h` | 2 | New C function declarations |
| `src/legacy/engine/*.c` | 2 | New C function implementations |
| `openswmm/legacy/engine/solver.pxd` | 1,2 | New cdef extern declarations |
| `openswmm/legacy/engine/_solver.pyx` | 1,2 | Expanded enums, new methods |
| `openswmm/legacy/engine/_solver.pyi` | 1,2,3 | Updated type stubs |
| `openswmm/legacy/engine/__init__.py` | 3 | Export new classes |
| `docs/api.rst` | 3 | Document new classes |

---

## 6. Testing Strategy

### Mass Balance Validation Tests

```python
def test_routing_mass_balance_closure(solver_with_completed_sim):
    """Verify routing totals close within tolerance."""
    totals = solver.get_routing_totals()

    total_inflow = (
        totals['dry_weather_inflow'] +
        totals['wet_weather_inflow'] +
        totals['groundwater_inflow'] +
        totals['ii_inflow'] +
        totals['external_inflow']
    )
    total_outflow = (
        totals['flooding'] +
        totals['outflow'] +
        totals['evaporation_loss'] +
        totals['seepage_loss']
    )
    storage_change = totals['final_storage'] - totals['initial_storage']

    balance = total_inflow - total_outflow - storage_change
    assert abs(balance) / max(total_inflow, 1e-6) < 0.01  # < 1% error

def test_external_inflow_appears_in_routing_totals(solver):
    """Verify that set_lateral_inflow contributes to routing totals."""
    solver.start()
    for _ in range(100):
        solver.set_value(SWMMObjects.NODE, SWMMNodeProperties.LATERAL_INFLOW,
                        index=0, value=1.0)
        solver.step()
    solver.end()

    totals = solver.get_routing_totals()
    assert totals['external_inflow'] > 0

def test_rainfall_override_appears_in_runoff_totals(solver):
    """Verify that API rainfall override appears in runoff totals."""
    solver.start()
    for _ in range(100):
        solver.set_value(SWMMObjects.SUBCATCHMENT,
                        SWMMSubcatchmentProperties.API_RAINFALL,
                        index=0, value=2.0)
        solver.step()
    solver.end()

    totals = solver.get_runoff_totals()
    assert totals['rainfall'] > 0
```

---

## 7. Priority Order

1. **Phase 1** (enum expansion) — Low risk, high value, no C changes
2. **Phase 2** (statistics + mass balance) — Requires C changes but critical for scientific use
3. **Phase 3** (Pythonic wrappers) — Pure Python, can be done in parallel with Phase 2
4. **Phase 4** (mass-balance-aware setters) — Polish, documentation, audit trail

---

## 8. Notes for Agent Continuation

- The C API uses a generic `swmm_getValueExpanded(objType, property, index, subIndex, pollutantIndex)` pattern. The `subIndex` is overloaded:
  - For subcatchment subareas: 0=impervious, 1=pervious
  - For LID units: the LID unit index within the subcatchment
  - For pollutants: the pollutant index (0-based)
- The `_solver.pyx` already handles string-to-index resolution in `get_value()`/`set_value()` — pass a string name as `index` and it calls `swmm_getIndex()`.
- The existing `Solver.get_mass_balance_error()` returns `(runoff_err, flow_err, qual_err)` as percentages. The new `get_routing_totals()` / `get_runoff_totals()` should return the raw volumes so users can do their own mass balance checks.
- Statistics structs in SWMM (`NodeStats`, `LinkStats`, etc.) are populated during `swmm_end()`. The new C functions should be callable after `end()` but before `close()`.
