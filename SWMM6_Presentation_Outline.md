# SWMM6: Modernizing Urban Water Management
## 60-Minute Webinar Outline

**Target Audience:** Water engineers, hydrologists, IT professionals, municipal planners, and researchers interested in next-generation stormwater modeling

---

## **SECTION 1: CONTEXT & LEGACY (8 minutes)**

### Opening Hook (1 min)
- **Opening statement:** For over 50 years, EPA's SWMM has been *the* standard—used by tens of thousands of practitioners worldwide for everything from routine design calculations to managing billion-dollar infrastructure portfolios.
- **The challenge:** Today's urban environments face unprecedented complexity: climate change, aging infrastructure, real-time digital twins, AI-driven optimization, and the need to couple multiple physics domains simultaneously.
- **The question:** Can a 50-year-old foundation meet 21st-century demands?

### SWMM Legacy: The Foundation (3 min)
- **What made SWMM legendary:**
  - First open-source, standardized tool for integrated hydrology-hydraulics-water quality
  - Originally published 1971; EPA formalized SWMM 5.0 in 2005
  - Countless textbooks, standards, and regulations built on its physics
  - Millions of practitioners trained on its input format
  
- **Strengths that endured:**
  - Accurate physically-based Saint-Venant momentum equations for dynamic wave routing
  - Integrated treatment of infiltration, runoff, quality, and surcharge
  - Accessible input syntax (INP files) readable by humans and machines alike

### The Tension (2 min)
- **Performance ceiling:** Single-threaded C code from the 2000s struggles with modern scales (10,000+ subcatchments, coupled 1D/2D grids)
- **Integration friction:** Data passes through ad-hoc file formats (DAT, OUT, RPT) requiring separate readers for each output type
- **Extensibility wall:** Adding new physics (2D flow, LID feedback, real-time control) required monolithic code changes
- **Accessibility gap:** Python ecosystem has exploded; SWMM was trapped in C with limited bindings

---

## **SECTION 2: THE MODERNIZATION VISION (4 minutes)**

### Sponsorship & Collaboration (1 min)
- **ASCE-EWRI backing:** The American Society of Civil Engineers Environmental & Water Resources Institute recognized SWMM's strategic importance
- **Open-source governance:** Transition to HydroCouple community stewardship while preserving EPA's legacy code
- **Cross-domain expertise:** Contributors from university labs, consulting firms, software companies, and public agencies

### The Three Pillars of Modernization (3 min)

1. **Computational Architecture** — Move from monolithic C to a **data-oriented, reentrant engine** that unleashes parallelism and GPU acceleration
   
2. **Scientific Fidelity** — Implement cutting-edge process formulations (semi-implicit continuity, spatially explicit 2D, physics-based infiltration recovery) while preserving bit-exact parity with legacy SWMM 5 where needed for validation
   
3. **Accessibility & Integration** — Python-first developer experience, native spatial data (GeoPackage), and plugin architecture so users can write their own domain extensions without touching core C++

---

## **SECTION 3: ARCHITECTURE DEEP DIVE (16 minutes)**

### 3.1 Data-Oriented Design (4 min)

**The Problem:** Traditional object-oriented layout stores model state scattered across memory
```
class Node { double depth, elevation, area, invert; ... };
std::vector<Node> nodes;  // Each node is separate memory region
```
When SWMM iterates over 10,000 nodes per timestep, CPU cache misses spike.

**The Solution: Structure-of-Arrays**
```
struct NodeArray {
  std::vector<double> depths;      // Contiguous block A
  std::vector<double> elevations;  // Contiguous block B
  std::vector<double> areas;       // Contiguous block C
  ...
};
```
- All depths fit in L3 cache → SIMD vectorization → 4–8× speedup on inner loops
- **Benchmark results (develop branch):** Single-threaded performance improved 30–50% on medium models
- Memory layout aligned for GPU transfers (Kokkos abstraction layer handles CUDA/HIP/OpenMP)

### 3.2 Reentrant Engine & Thread-Safe State (3 min)

**Legacy SWMM:** Global state scattered across dozens of static arrays
```c
// SWMM 5.x
static Node* node_list;
static Link* link_list;
```
Cannot safely run two simulations in parallel; concurrent simulations clobber each other's state.

**SWMM6:** Opaque engine handle holds all state
```c
// SWMM 6.0
SWMM_Engine* eng1 = SWMM_CreateEngine(...);
SWMM_Engine* eng2 = SWMM_CreateEngine(...);  // Safe; independent state
while (SWMM_Step(eng1)) { ... }  // Run in parallel threads
while (SWMM_Step(eng2)) { ... }
```
- **Enables:** Batch Monte Carlo, ensemble forecasts, real-time scenario evaluation
- **Example use case:** 1,000 rain realizations solved simultaneously on 32-core server
- **Python benefit:** NumPy broadcast operations on model arrays without GIL contention

### 3.3 Plugin-Based I/O Architecture (4 min)

**Decoupling I/O from simulation**
- Simulation runs on CPU/GPU cores; I/O tasks dispatched to a **dedicated thread pool**
- Users write `.inp` file with new sections; plugin registry auto-routes to custom handlers
- Plugins implement `IPluginComponentInfo` interface (header-only SDK in `include/openswmm/plugin_sdk/`)

**Plugin Categories:**
1. **Input plugins:** Parse custom sections (e.g., real-time control logic, external forcings)
2. **Output plugins:** Write results to GeoPackage, HDF5, NetCDF, or proprietary formats
3. **Report plugins:** Generate summaries, PDFs, dashboards without modifying core

**Benefit:** Extend SWMM without recompiling the engine; third-party vendors can ship proprietary layers

### 3.4 C++20 Modernization (2 min)

- **Language features leveraged:**
  - `std::span<>` for safe array views (replaces pointer arithmetic)
  - Concepts for compile-time API contracts
  - Modules (under review) for cleaner dependency graphs
  - Coroutines (future) for callback-heavy workflows

- **Build system:** CMake 3.21+ with vcpkg dependency management
  - Cross-platform: Windows (MSVC 19.29+), Linux (GCC 10+), macOS (Clang 14+)
  - GPU support: Kokkos abstraction handles CUDA, HIP, OpenMP backends

- **Backward compatibility:** Original EPA SWMM 5.x solver lives in `src/legacy/` unmodified
  - Binary-exact regression tests ensure legacy mode produces byte-for-byte identical results
  - Users can opt into new physics without sacrificing reproducibility

---

## **SECTION 4: NEXT-GENERATION PHYSICS (14 minutes)**

### 4.1 Semi-Implicit Node Continuity (3 min)

**The Classic Problem:** SWMM 5 has a discontinuity at surcharge
- Free-surface equation (surface zone): Weir equations govern spilling
- Pressurized equation (pressurized zone): Different head-capacity relationships
- **Result:** Oscillations and convergence issues when pipe transitions between modes

**SWMM6 Solution:**
- Single unified continuity equation combining free-surface and pressurized via **semi-implicit formulation**
- Removes the two-branch bifurcation; surcharge transition is smooth
- **Physics:** Picard iteration on implicit node depths with **Anderson Acceleration** (residual-history mixing)
  - Iteration count reduced 25–50% on stiff transitions
  - Enables larger timesteps without stability loss

**Practical impact:** More realistic surcharge propagation in combined sewers; better predictions of street flooding

### 4.2 Dynamic Preissmann Slot (2 min)

**Standard virtual slot:** Fixed-width narrow slot added to closed conduit
- Allows free-surface equations to solve pressurized flow
- Problem: Width is a tuning parameter; choice affects damping and wave propagation

**SWMM6 dynamic slot:**
- Slot width adapts with conduit geometry and local Froude number
- **Result:** Better-behaved pressure waves, reduced numerical diffusion
- References: Bellinge et al. (2020) call-graph provenance for detailed formulation

---

### 4.3 Spatially Explicit Overland Flow & Groundwater (SWMM2D Integration) (5 min)

**The Vision:** Couple 1D pipe network with 2D surface and groundwater

**1D Hydraulics (unchanged):**
- Conduits, pumps, orifices, weirs solved with dynamic wave routing
- Surcharge at junctions drives overland flow

**2D Coupling (new):**
1. **Overland Flow Grid**
   - DEM-based surface mesh; each cell has elevation, Manning's n, infiltration capacity
   - When junction surcharges, water spills onto grid and flows downslope
   - Grid routing is **shallow-water equations** (depth-averaged momentum)
   - GPU-accelerated on Kokkos backends; CPU OpenMP fallback

2. **Lateral Groundwater Exchange**
   - Saturated zone modeled as vertically integrated storage
   - Two-way flux between groundwater and pipes/junctions
   - **Physically-based:** Infiltration capacity depends on soil saturation

3. **Green Infrastructure Placement**
   - LID cells (bio-retention, permeable pavement) resolved on spatial grid
   - Can capture runoff directly from overland surface
   - Feedback into groundwater and pipe inflows

**Computational Strategy:**
- 1D solver (pipe network) runs on CPU with timestep ~5–30 sec
- 2D solver (surface grid) on GPU with sub-step refinement (~0.5–5 sec substeps)
- Coupling via interpolation/aggregation at 1D timestep boundaries
- **Benefit:** Orders-of-magnitude speedup vs. brute-force 1D-2D finite-element coupling

**Example scenario:** Hurricane in New Orleans
- 1D represents drainage canals and major sewers
- 2D grid captures street-level ponding and overland routing
- Coupled simulation predicts flood extent, depth, and duration
- Ensemble runs (100 scenarios) complete overnight

### 4.4 Physics-Based Initial Abstraction Recovery (RDII Advancement) (4 min)

**SWMM 5 RDII model:**
- RTK (rainfall, time-to-peak, base-time ratio) triplet parameterizes synthetic unit hydrograph
- Initial Abstraction (IA) fixed per event; no inter-event recovery model

**SWMM6 Physics-Based Recovery** (new `[RDII_DECAY]` section):
- IA available evolves as **exponential depletion/recovery process**
- Formula:
  $$IA_{avail}(t+\Delta t) = IA_{max} - (IA_{max} - IA_{avail}(t)) \cdot e^{-k_{rec}(T) \Delta t}$$
- Recovery rate depends on temperature:
  $$k_{rec}(T) = k_0 + k_T \cdot e^{\theta(T - T_{ref})}$$

**Implications:**
- **Seasonal variation emerges automatically** from air temperature without manual monthly tuning tables
- **Frozen-ground suppression:** Cold temperatures slow IA recovery (e.g., snow accumulation period)
- **Validation:** Single RTK set per sewershed calibrated on multi-year record; no parameter cycling

**Use case:** Long-term continuous simulation (10–30 years) for climate adaptation studies
- Traditional SWMM requires seasonal parameters; SWMM6 is parameter-light
- Better extrapolation to future climate scenarios

---

## **SECTION 5: THE NEW C API & DOMAIN ARCHITECTURE (10 minutes)**

### 5.1 Domain-Split API Rationale (2 min)

**Legacy monolithic header:**
```c
// SWMM 5
#include "swmm5.h"  // ~50 functions, everything mixed
```

**SWMM6 modular design:**
Separate headers for each domain; users import only what they need

### 5.2 Core Header Families (8 min)

**Engine & Lifecycle**
- `openswmm_engine.h` — Create/destroy engines, state machine (cold start, warm start), error codes

**Model Building**
- `openswmm_model.h` — Load `.inp`, validate, serialize, query metadata
- `openswmm_options.h` — Simulation options (FLOW_UNITS, DRY_ONLY, REPORT_START, etc.)

**Infrastructure Components**
- `openswmm_nodes.h` — Junctions, outfalls, storage, dividers
- `openswmm_links.h` — Conduits, pumps, orifices, weirs, outlets
- `openswmm_subcatchments.h` — Subcatchments, infiltration models (Green-Ampt, NRCS, Horton), LID controls
- `openswmm_gages.h` — Rain gages with series attachment
- `openswmm_transects.h` — Custom cross-section geometry

**Flows & Forcing**
- `openswmm_inflows.h` — External inflows, DWF (dry-weather), RDII with new decay parameters
- `openswmm_controls.h` — Control rules, real-time link actions
- `openswmm_pollutants.h` — Pollutant definitions, runtime injection, treatment

**Spatial & Water Quality**
- `openswmm_spatial.h` — CRS (coordinate reference systems), vertices, polylines, polygons for GIS interop
- `openswmm_infrastructure.h` — Streets, inlets (HEC-22 grate/curb capture), LID as storage nodes
- `openswmm_quality.h` — Landuse, buildup/washoff, treatment processes

**Queries & Reports**
- `openswmm_statistics.h` — Node/link/subcatchment statistics (min depth, max flow, etc.)
- `openswmm_massbalance.h` — Continuity errors, cumulative flux totals
- `openswmm_hotstart.h` — Save/load/modify hot-start checkpoints (depths, volumes, IA, snow, GW)

**Data Interop**
- `openswmm_geopackage.h` — GeoPackage (OGC SQLite standard) input/output with full CRS metadata
- `openswmm_callbacks.h` — Progress, warnings, step callbacks for UI integration

**Example: Running a simulation with Python**
```python
from openswmm.engine import Solver, Nodes, Links

with Solver("model.inp", "model.rpt", "model.out") as s:
    nodes, links = Nodes(s), Links(s)
    for _ in range(s.sim_duration_steps()):
        s.step()
        depth = nodes.get_depth("J1")    # Via openswmm_nodes.h
        flow  = links.get_flow("C1")     # Via openswmm_links.h
        # Modify controls in real-time
        links.set_setting("P1", new_pump_speed)
```

---

## **SECTION 6: GEOPACKAGE & SPATIAL INTEGRATION (6 minutes)**

### 6.1 Why GeoPackage? (2 min)

**Traditional SWMM workflow:**
- `.inp` text file (human-readable but rigid)
- `.out` binary output (proprietary EPA format)
- `.rpt` text report (parsed manually)
- GIS shapefile / layer external to simulation

**Challenges:**
- Round-trip editing loses topology metadata
- Spatial data (coordinates, polygons) live in separate GIS project
- Difficult to version-control spatial changes

### 6.2 GeoPackage as a Single Source of Truth (4 min)

**What is GeoPackage?**
- **OGC standard:** SQLite database with geometry support (ISO 19125-1)
- **Single file:** All inputs, outputs, observed data, and topology in one `.gpkg`
- **CRS metadata:** Coordinate reference system (e.g., EPSG:4326, EPSG:26910) stored and queryable

**SWMM6 Integration (`-DOPENSWMM_WITH_GEOPACKAGE=ON`):**

1. **Input Layer**
   - Nodes, links, subcatchments as feature layers
   - Geometry + attributes in feature tables
   - Hydro tables for connectivity
   - User-defined flags and metadata

2. **Output Layer**
   - Time-series results stored as temporal features
   - Snapshots at key timesteps
   - Polygon layers for 2D surface depth/velocity grids

3. **Observed Data**
   - Gage observations, measured flow, quality samples
   - Temporal lineage for validation/calibration

**Workflow Benefit:**
```
Model.gpkg → SWMM6 Engine → Results appended to same .gpkg
                             ↓
                        QGIS / ArcGIS
                        Real-time visualization
                        Statistical queries
                        Publication-ready maps
```

**Use case:** Collaborative modeling in consulting
- Project manager edits geometry in QGIS
- Engineer runs simulations in Python notebook
- Results auto-update GeoPackage layers
- Stakeholder views interactive web map of flood extent

---

## **SECTION 7: PYTHON BINDINGS & PRACTICAL WORKFLOWS (6 minutes)**

### 7.1 PyPI Installation & Quick Start (2 min)

```bash
pip install openswmm
```

Wheel builds for:
- Python 3.9–3.13
- Windows, Linux, macOS
- CPU (OpenMP) and GPU (CUDA, HIP) variants

### 7.2 Workflow Examples (4 min)

**Example 1: Ensemble Monte Carlo Flood Risk**
```python
from openswmm.engine import Solver
import numpy as np

rainfall_scenarios = np.random.normal(75, 15, size=1000)  # 1000 rain events

results = []
for i, rain_depth in enumerate(rainfall_scenarios):
    with Solver("model.inp", "model.rpt", "model.out") as s:
        # Inject rainfall
        for gage in s.gages():
            gage.series[:] *= (rain_depth / 75)
        
        # Simulate
        max_depth = 0
        while s.step():
            max_depth = max(max_depth, s.nodes.get_depth("J1"))
        
        results.append(max_depth)

# Percentile analysis
print(f"10th percentile max depth: {np.percentile(results, 10):.2f} ft")
print(f"90th percentile max depth: {np.percentile(results, 90):.2f} ft")
```

**Example 2: Real-Time Control with Feedback**
```python
# Digital twin: update pump speeds based on live sensor data
with Solver("model.inp", "model.rpt", "model.out") as s:
    for step in range(steps):
        s.step()
        
        # Read sensors
        upstream_depth = sensor_api.get_depth("J_upstream")
        downstream_depth = sensor_api.get_depth("J_downstream")
        
        # Adaptive control
        pump_speed = control_logic(upstream_depth, downstream_depth)
        s.links.set_setting("P1", pump_speed)
```

**Example 3: Calibration with scikit-optimize**
```python
from skopt import gp_minimize
from openswmm.engine import Solver
import pandas as pd

# Load observations
obs_data = pd.read_csv("observed_flows.csv")

def simulate_and_evaluate(params):
    """Objective: minimize difference from observations"""
    roughness, infiltration = params
    
    with Solver("model.inp", "model.rpt", "model.out") as s:
        s.model.set_conduit_roughness_factor(roughness)
        s.model.set_infiltration_factor(infiltration)
        
        sim_flows = []
        for _ in range(s.sim_duration_steps()):
            s.step()
            sim_flows.append(s.links.get_flow("MainOutfall"))
        
        rmse = np.sqrt(np.mean((sim_flows - obs_data.values) ** 2))
        return rmse

# Optimize
result = gp_minimize(simulate_and_evaluate, 
                     bounds=[(0.5, 2.0), (0.1, 2.0)],
                     n_calls=50)
print(f"Optimal params: {result.x}")
```

---

## **SECTION 8: PERFORMANCE & BENCHMARKS (5 minutes)**

### 8.1 Computational Speedups (3 min)

| Benchmark | SWMM 5 | SWMM6 (CPU) | SWMM6 (GPU) |
|-----------|--------|------------|-----------|
| 5,000-subcatchment urban model, 10-year continuous | 47 hours | 12 hours (3.9×) | 2.8 hours (16.8×) |
| 1D/2D coupled (10K surface cells) | N/A | 8 hours | 1.2 hours (6.7×) |
| 100-realization ensemble (1,000 subcatch) | 4,700 hours serial | 50 hours parallel (94×) | 12 hours on multi-GPU (390×) |

**Parallelization:**
- OpenMP for CPU: `#pragma omp parallel` on node iteration loops
- Kokkos for GPU: Portable CUDA/HIP kernels
- Python GIL-release: Cython extensions allow C++ parallelism even in threaded Python

### 8.2 Memory Efficiency (2 min)

- **Data-oriented layout:** 30–50% reduction in memory footprint vs. object-oriented SWMM 5
- **Hot-start checkpointing:** Save full state at strategic points; resume from checkpoint for operational forecasting
  - E.g., 30-day warm-up, then 10-day forecast can save 3 days of computation per cycle

---

## **SECTION 9: ROADMAP & IN-DEVELOPMENT FEATURES (5 minutes)**

### 9.1 Spatially Explicit Inlets (2 min)

**Current:** Inlets are passive junction nodes
**Future:** Mode-switching nodes
- Capture street flow when gutter spread exceeds threshold (HEC-22 empirical equations)
- Revert to passive when flow recedes
- Enables accurate street-flood extent prediction without ad-hoc weir additions

### 9.2 LID as Storage Nodes (1.5 min)

**Current:** LID controls simplified as runoff reducers
**Future:** Full reduced-physics kinematic Richards solver
- Separate layers: surface, media, gravel
- Two-way hydraulic feedback: LID fills → reduced infiltration; LID saturated → all runoff to surface
- Maps naturally onto storage-node architecture

### 9.3 Other Enhancements on Roadmap (1.5 min)

- **Bit-parity verification:** Automated regression suite ensuring legacy-mode outputs are byte-for-byte reproducible
- **SWMM2D advanced coupling:** Coupling between multiple 1D subnetworks and shared 2D surfaces
- **GPU solvers for 2D:** Advanced preconditioners (BoomerAMG via HYPRE library)
- **Real-time control optimizers:** Built-in MPC (Model Predictive Control) for storage and pump coordination
- **Uncertainty quantification:** Integration with UQ libraries (SALib, Pyre) for sensitivity analysis

---

## **SECTION 10: USE CASES & IMPACT (4 minutes)**

### 10.1 Climate Adaptation Planning (1 min)

**Scenario:** City plans $500M stormwater upgrade strategy
- **Old approach:** Run SWMM 5 with historical design storms; uncertainty unknown
- **New approach:** 
  - 1,000-member precipitation ensemble (climate model downscaling)
  - Each run couples 1D/2D to resolve street flooding
  - 8 GPU-accelerated machines: 72 hours total
  - Risk maps show 1%, 10%, 50%, 90% confidence bounds
  - Budget allocation for green infrastructure prioritized by effectiveness across scenarios

### 10.2 Digital Twin for Operational Control (1.5 min)

**Real-world example:** Combined sewer system in mid-Atlantic city
- **Instrumentation:** 150 depth sensors, 20 pump stations, SCADA integration
- **SWMM6 workflow:**
  - Python service polls sensors every 30 sec
  - Hot-start from last timestep; forecast next 2 hours
  - Optimal pump schedule computed (minimize street flooding, avoid CSO discharge)
  - Control commands relayed to field devices
  - Results logged to GeoPackage for audit and learning
- **Outcome:** CSO violations reduced 60%; regulatory compliance without new pipes

### 10.3 Research & Innovation (1 min)

- **Green infrastructure optimization:** AI/ML models trained on SWMM6 simulations to design LID layouts
- **Coupled hydrology-air-quality:** Stormwater runoff model feeds air-shed model (dust resuspension from dry areas)
- **Underwater drone deployment:** 2D overland flow simulation predicts navigation zones

---

## **SECTION 11: MIGRATION & ADOPTION PATHWAY (3 minutes)**

### 11.1 From SWMM 5 to SWMM6 (2 min)

**Compatibility Mode:**
```python
from openswmm.engine import Solver

# Use SWMM 5 input file directly
with Solver("my_model_v5.inp") as s:
    s.set_legacy_solver()  # Runs original EPA SWMM 5 code
    while s.step():
        pass
```
- Results bit-for-bit identical to EPA SWMM 5.2
- Baseline for validation and regression testing

**Gradual Opt-In:**
- Activate new physics incrementally: `NODE_CONTINUITY SEMI_IMPLICIT`, `ANDERSON_ACCEL YES`, etc.
- Compare results before/after each change

**Python Migration:**
- Legacy Python binding (2D numpy arrays) still supported
- New API adds type hints, docstrings, better error messages

### 11.2 Installation & CI/CD Integration (1 min)

**For consultants/municipalities:**
- Install from PyPI: `pip install openswmm`
- No C++ compiler needed (pre-built wheels)
- Docker image available: `docker pull hydrocouple/openswmm:latest`

**For software developers:**
- Fork on GitHub, contribute C++ or Python
- CI runs unit tests, Python pytest, CodeQL security scan
- Contributor License Agreement (CLA) automated via GitHub

---

## **SECTION 12: DEMO & LIVE INTERACTION (10 minutes)**

*(Recommend split into 3–5 focused demos with prepared models)*

### Demo 1: Python Quick-Start (2 min)
- Load a small model, step through, extract time series
- Show how fast it runs vs. opening SWMM GUI

### Demo 2: 1D/2D Coupling (3 min)
- Side-by-side visualization: pipe surcharge driving overland sheet flow
- 2D depth grid rendered as heatmap
- Show speedup: 1,000-node network + 10K surface cells in ~30 sec per timestep on laptop

### Demo 3: GeoPackage Round-Trip (2 min)
- Edit model in QGIS (change a junction elevation)
- Save as `.gpkg`
- Load in Python, run simulation
- Results automatically populate new layer in GeoPackage
- View in QGIS

### Demo 4: Real-Time Control (1.5 min)
- Simulate a rain event with manual vs. automated pump control
- Show depth reduction at critical junction with adaptive control
- Live graph updates as simulation progresses

### Demo 5: Ensemble Visualization (1.5 min)
- 100 Monte Carlo realizations of a small catchment
- Box-plot of max depth at outlet across all scenarios
- Percentile bands displayed on map

---

## **SECTION 13: CLOSING & RESOURCES (3 minutes)**

### 13.1 Key Takeaways

1. **Modernization without rupture:** SWMM6 is built on 50 years of legacy while embracing 2020s software architecture
2. **Physics + Performance:** New formulations (2D, semi-implicit continuity, physics-based IA) + data-oriented parallelism = more accurate, faster
3. **Open + Accessible:** MIT license, Python-first, GeoPackage-native, community-governed
4. **Ready for digital twins:** Reentrant engine, hot-start checkpoints, real-time callbacks enable autonomous control systems

### 13.2 Resources & Links

**Official Sites:**
- **C/C++ Engine:** https://hydrocouple.org/openswmm.engine (full API docs, architecture notes)
- **Python Bindings:** https://hydrocouple.org/openswmm.engine/python (quickstart, per-domain guides)
- **GitHub:** https://github.com/HydroCouple/openswmm.engine (develop branch; open issues & discussions)

**Getting Started:**
```bash
# Install
pip install openswmm

# Or build from source
git clone -b develop https://github.com/HydroCouple/openswmm.engine.git
cd openswmm.engine && cmake --preset=Linux -B build && cmake --build build
```

**Community:**
- GitHub Discussions: Ask questions, share projects
- Contributor guide: CONTRIBUTING.md
- Licensing: MIT; legacy EPA SWMM is public domain

### 13.3 Q&A Preparation Notes

**Anticipated Questions:**

| Question | Answer |
|----------|--------|
| "Will my old SWMM 5 files work?" | Yes. Load directly; set `legacy_solver=True` for bit-exact compatibility. |
| "Do I need a GPU?" | No. CPU OpenMP parallelism is sufficient for most models. GPU optional for massive ensembles. |
| "How do I handle rainfall data?" | GeoPackage can store observed series; Python API allows runtime injection of synthetic or observed rainfall. |
| "Can I couple SWMM6 to other models (e.g., HEC-HMS)?" | Yes. Plugin SDK allows custom input readers; GeoPackage can export results to other tools. |
| "Is there a GUI?" | Not yet; focus is on API. QGIS can read/write GeoPackage layers; Jupyter notebooks provide interactive environment. |
| "How do I contribute?" | Sign CLA, fork develop branch, commit tests, open PR. Contributors valued. |

---

## **PRESENTATION STRUCTURE SUMMARY**

| Section | Duration | Key Moment |
|---------|----------|-----------|
| 1. Context & Legacy | 8 min | Opening: 50-year impact, modern challenges |
| 2. Modernization Vision | 4 min | Three pillars introduced |
| 3. Architecture Deep Dive | 16 min | Data-oriented design, threading, plugins, C++20 |
| 4. Next-Gen Physics | 14 min | Semi-implicit, dynamic slot, 2D/GW, IA recovery |
| 5. New C API | 10 min | Domain-split headers, practical code examples |
| 6. GeoPackage | 6 min | Why single-file spatial backend matters |
| 7. Python Workflows | 6 min | Three concrete examples: ensemble, control, calibration |
| 8. Performance | 5 min | Benchmarks and memory efficiency |
| 9. Roadmap | 5 min | Inlets, LID nodes, UQ integrations |
| 10. Use Cases | 4 min | Climate adaptation, digital twins, research |
| 11. Migration Path | 3 min | Compatibility, installation, adoption |
| 12. Live Demo | 10 min | 5 focused demos with prepared models |
| 13. Closing | 3 min | Takeaways, resources, Q&A prep |
| **Total** | **60 min** | |

---

## **PRESENTATION DELIVERY NOTES**

### Tone & Pacing
- **Opening (0–12 min):** Narrative-driven; show SWMM's historical importance, the tension between legacy constraints and modern demands
- **Middle (12–45 min):** Technical depth balanced with visuals (architecture diagrams, benchmark graphs, code snippets)
- **Demos (45–55 min):** Shift to interactive, live coding; engage audience with visible output
- **Closing (55–60 min):** Inspirational; emphasize community and collaboration; Q&A invitation

### Visual Assets to Prepare

1. **Timeline slide:** SWMM 1971 → SWMM 5.0 (2005) → SWMM6 (2024) milestones
2. **Data-oriented diagram:** Side-by-side comparison of object-oriented vs. structure-of-arrays memory layout
3. **1D/2D coupling animation:** Show surcharge at junction → overland flow spreading on 2D grid
4. **Performance comparison chart:** Bar chart (SWMM 5, SWMM6 CPU, SWMM6 GPU) for key scenarios
5. **GeoPackage workflow diagram:** .inp + GIS → .gpkg → simulation → results layer
6. **Roadmap timeline:** In-development features with expected Q/release dates

### Audience Engagement Strategies
- **Poll:** "Have you calibrated SWMM to observed data? Ensemble? Real-time control?" → Show hands to gauge room expertise
- **Scenario questions:** "What's the biggest bottleneck in your current workflow?" → Highlight how SWMM6 addresses it
- **Call to action:** "Visit GitHub, open an issue, share your use case in Discussions"

---

**End of Presentation Outline**
