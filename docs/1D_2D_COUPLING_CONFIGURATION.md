# 1D–2D Coupling Configuration

How a 1D SWMM node is connected to the 2D overland mesh, what the **discharge
coefficient (`Cd`)** and **exchange `Area`** in the coupling map do, and how that
`Area` differs from a node's classic **ponded area (`Aponded`)**.

> **Audience.** Part 1 is a conceptual guide for model builders. Part 2 is a
> reference with the exact equations and source `file:line` citations.
>
> See also: [`two_dimensional_model.md`](two_dimensional_model.md) (full `[2D_*]`
> INP grammar) and [`1D_2D_COUPLING_GATE_REVIEW.md`](1D_2D_COUPLING_GATE_REVIEW.md)
> (design review).

---

# Part 1 — Concepts

## 1. The big picture

The 1D drainage network (pipes, junctions, outfalls) and the 2D overland surface
(a triangular mesh) are separate solvers that exchange water at **coupling
points** — specific mesh vertices or triangles tied to a SWMM node. You declare
those links in the INP; the engine builds the exchange automatically.

```mermaid
flowchart LR
    subgraph ONED["1D network (pipes, in feet internally)"]
        J["Junction / inlet"]
        O["Outfall"]
    end
    subgraph IFACE["Coupling interface"]
        ORI["Orifice exchange<br/>Q = Cd · A · sign(dh) · sqrt(2g·dh)"]
        BC["Outfall tailwater<br/>head boundary"]
    end
    subgraph TWOD["2D surface mesh (SI, metres)"]
        CELLS["Triangle cells"]
    end
    J <-->|"surcharge spill / inlet capture"| ORI
    ORI <--> CELLS
    O -->|"pipe discharge"| BC
    BC --> CELLS
    CELLS -->|"tailwater"| BC

    MAP["2D_VERTEX_NODE_MAP<br/>2D_TRIANGLE_NODE_MAP<br/>Node, Cd, Area"] -. configures .-> ORI
    OPT["2D_OPTIONS<br/>COUPLING_CD, DRY_DEPTH"] -. configures .-> IFACE
```

Two coupling families behave differently:

- **Junctions / inlets** exchange through a **bidirectional orifice** (this doc's
  focus). Surface water drains in; surcharge spills out.
- **Outfalls** couple through a **prescribed tailwater head** (the 2D water
  surface becomes the outfall's downstream boundary) plus injection of the pipe's
  discharge onto the mesh. See §6.

## 2. Wiring a node to the mesh

Add the node to one of the coupling-map sections:

```
[2D_VERTEX_NODE_MAP]
;;Vertex  Node  Cd    Area
0         J1    0.65  1.0

[2D_TRIANGLE_NODE_MAP]
;;Triangle  Node  Cd    Area
12          J2    0.65  0.5
```

| Column | Meaning | Units | Default |
|--------|---------|-------|---------|
| Vertex / Triangle | Mesh vertex index/tag, or triangle index/tag, to couple | — | required |
| Node | SWMM node name the cell exchanges with | — | required |
| `Cd` | Orifice **discharge coefficient** of the connection | – | `0.65` (or `COUPLING_CD`) |
| `Area` | Effective **exchange (orifice throat) area** of the connection | m² (SI) | `1.0` |

**Vertex** coupling shares the exchange across the ring of cells around that
vertex (the *stencil*); **triangle** coupling uses the single cell. A node may be
mapped to several vertices — each becomes its own coupling point.

```mermaid
flowchart TB
    subgraph V["Vertex coupling (node ↔ vertex stencil)"]
        VN["Node J1"] --- VV(("vertex 0"))
        VV --- T0["cell A"]
        VV --- T1["cell B"]
        VV --- T2["cell C"]
    end
    subgraph T["Triangle coupling (node ↔ one cell)"]
        TN["Node J2"] --- TC["cell 12"]
    end
```

## 3. What `Cd` and `Area` do

When the connection carries flow, it is metered like an orifice:

$$Q = C_d \cdot A_{\text{eff}} \cdot \operatorname{sign}(\Delta h) \cdot \sqrt{2g}\,\sqrt{|\Delta h|}$$

- **`Cd`** scales the flow linearly — the head-loss coefficient of the grate /
  manhole connection. Lower `Cd` ⇒ a more throttled, lossier inlet.
- **`Area`** (`A_eff`) is the **hydraulic opening** the water passes through — the
  inlet-grate / manhole-throat area. It scales the flow but **stores no water**.
- `Δh = h_2d − h_1d` is the head difference that drives the flow, gated by the
  capped-pipe rim — see §4.

`A_eff` widens automatically from the configured inlet area to a manhole opening
(`2 × Area`) once the water surcharges past the rim, modelling a popped cover.

> **Picking values.** `Cd ≈ 0.6–0.65` is a typical sharp-edged orifice
> coefficient. Set `Area` to the physical inlet/grate open area (m²). They affect
> *how fast* water exchanges, never *how much* the surface stores.

## 4. The capped-pipe exchange model

A coupled junction is modelled as a **pipe sealed by a cover at the crown** — the
same depth at which the 1D dynamic-wave solver begins to surcharge (engages its
Preissmann slot):

```
z_top = invert + full_depth   (= the crown; the surcharge / slot-engagement point)
```

Below the crown the pipe flows sub-full and there is **no exchange** across the
interface. The cover only connects the two domains once water **reaches the crown**;
above it the exchange is the **bidirectional** orifice on the head difference.

```mermaid
flowchart TD
    A{"max(h1d, h2d) vs crown"}
    A -->|"below crown"| B["NO EXCHANGE<br/>pipe flows sub-full<br/>(no spilling, no capture)"]
    A -->|"above crown"| C["ORIFICE EXCHANGE<br/>bidirectional, driven by<br/>head diff h2d − h1d"]
    C -->|"pipe higher"| E["SPILL OUT onto the mesh"]
    C -->|"surface higher"| F["DRAIN IN to the pipe"]
```

- **Capped (below the crown):** the pipe is sealed. It flows sub-full and neither
  spills onto the surface nor captures surface water. This is the user's *"capped
  pipe allowing pressurisation but no spilling"*.
- **Overtopped (at/above the crown):** once water reaches the crown the cover is
  submerged and the orifice connects the domains. Flow is **bidirectional** on
  `h2d − h1d` — spilling out when the pipe is higher, draining in when the surface
  is higher (so when `h2d > h1d` with both above the crown, flow is still passed
  *into* the 1D node).

A C¹ Hermite ramp on `max(h1d, h2d)` across a 5 cm band above the crown opens the
gate without a flux or derivative jump (CVODE/BDF stability).

```mermaid
flowchart TB
    Z3["z_top = invert + full_depth ── crown / cover (gate opens here)"]
    Z2["above the crown: 1D surcharge storage lives in the slot (sur_depth headroom)"]
    Z1["invert"]
    Z3 --- Z2 --- Z1
    Z3 -. "exchange only above the crown; A widens to manhole (2·Area)" .-> Z3
```

> Elevation ladder: **invert → crown (`+full_depth`, the gate) → slot headroom
> (`+sur_depth` above the crown)**. The gate is tied to the **crown**, *not*
> `crown + sur_depth`: it opens exactly when the pipe surcharges, keeping the inlet
> consistent with the slot. `sur_depth` is left **free to size the 1D slot's
> storage headroom** above the crown (where the surcharge volume is stored by the
> dynamic-wave solver), and no longer shifts the exchange threshold.
>
> **Geometry consistency.** The crown should match the **2D mesh bed elevation** at
> the coupling node — that is where surface water meets the cover. For a culvert
> that daylights at grade, the crown coincides with the road surface.

## 5. Exchange `Area` vs ponded area (`Aponded`) — the key distinction

These two parameters are often confused. They are completely different.

| | **Exchange `Area`** (coupling map) | **Ponded area `Aponded`** (`[JUNCTIONS]`) |
|---|---|---|
| Role | Orifice **throat** the water flows *through* | Surface **footprint** the water is *stored on* above the rim |
| Governs | Exchange **flow rate** (with `Cd`) | How fast the **HGL rises** above the crown (`dH = dV / Aponded`) |
| Stores water? | No | Yes (a flat pond on top of the node) |
| Units | m² (SI) | project area units (ft² / m²) |
| Where | `[2D_VERTEX_NODE_MAP]` / `[2D_TRIANGLE_NODE_MAP]` | `[JUNCTIONS]` column 6 |

```mermaid
flowchart LR
    SURF["Surface water column<br/>(storage footprint = ponded area)"]
    THROAT["Orifice throat<br/>(opening = exchange Area, loss = Cd)"]
    PIPE["Pipe HGL"]
    SURF -->|"flows THROUGH the throat"| THROAT --> PIPE
```

### Auto-aligned ponded area for coupled nodes

For an **uncoupled** junction, `Aponded` is a user-supplied flat pond that only
acts when `ALLOW_PONDING` is on. For a **2D-coupled** junction the surface storage
*is* the 2D mesh, so the engine manages `Aponded` for you:

- It **overrides** any user `Aponded` on a coupled junction with the **footprint of
  the surrounding 2D cells** (the median-dual area, `Σ incident cell area / 3`),
  converted to internal units.
- It lets that node **pond above its crown regardless of the global
  `ALLOW_PONDING`** flag, so the 1D HGL can rise above the crown and track the
  overlying 2D water surface once the exchange gate opens at the crown (§4). This
  is the storage that lets surcharge spill and inlet capture work; the 1D
  Preissmann slot (sized by `sur_depth`) provides the additional pressurised
  surcharge store inside the pipe.

> **Why this matters.** Earlier the coupling *zeroed* `Aponded` on coupled nodes.
> That pinned the 1D head at the crown, so the node could neither store its
> surcharge nor track the 2D surface. Auto-sizing `Aponded` to the 2D footprint
> frees the HGL to rise above the crown and align with the 2D surface.

> **Tradeoff (bounded).** Above the crown the 1D pond and the stencil 2D cells
> exchange and equilibrate, so they represent the same near-manhole surface and
> storage there is mildly double-counted. The median-dual share keeps that area
> small, and a real flood spreads onto the broader mesh (cells beyond the
> stencil), which stays single-counted. You do **not** set `Aponded` yourself on
> coupled nodes — a value
> in the INP is overridden (with a warning).

## 6. Junctions vs outfalls

| | **Junction / inlet** | **Outfall** |
|---|---|---|
| Exchange | Capped-pipe gated orifice (§4): no exchange below the crown, bidirectional above | Pipe discharge injected onto the mesh |
| 2D → 1D | Drain in once surface rises above the crown (`h2d > h1d`) | **Tailwater head BC**: the 2D surface sets the outfall stage (only when the cell is genuinely wet; a dry surface ⇒ free discharge) |
| `Aponded` | Auto-set to 2D footprint (HGL rises above the crown to track the 2D surface) | Not used (outfalls don't pond) |

## 7. `[2D_OPTIONS]` coupling keys

| Key | Meaning | Default |
|-----|---------|---------|
| `COUPLING_CD` | Default `Cd` when a map row omits it | `0.65` |
| `COUPLING_INTERVAL` | SWMM steps between exchanges (`0` = every step) | `0` |
| `DRY_DEPTH` | Wet/dry threshold (m); a cell shallower than this neither captures nor receives | `0.001` |

## 8. Worked example

```
[OPTIONS]
FLOW_UNITS    CMS
ALLOW_PONDING NO              ; coupled nodes still pond — the engine handles it

[JUNCTIONS]
;;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded
J1      0.0   1.0       0          0         0       ; Aponded auto-set from 2D
```
Here the exchange gate sits at the 1 m crown (set the 2D mesh bed there to ~1.0,
the inlet daylighting at grade). `SurDepth` does **not** move the gate; raise it
(e.g. `SurDepth 2.0`) to give the 1D Preissmann slot headroom to store surcharge
inside the pipe above the crown.
```
[2D_OPTIONS]
DRY_DEPTH     0.002
COUPLING_CD   0.7

[2D_VERTEX_NODE_MAP]
;;Vertex  Node  Cd   Area
0         J1    0.7  1.0          ; 0.7 loss, 1 m² inlet throat
```

When rain ponds on the cells around vertex 0 and the surface rises above the crown
(= 1 m here), water is drained into `J1` through the orifice. If the pipe network
surcharges `J1`, the HGL rises above the crown and spills back onto those same
cells; with `SurDepth > 0` the slot stores part of that surcharge inside the pipe.

---

# Part 2 — Reference

All paths are under `src/engine/`. Heads/areas/flows on the **1D** side are in
internal **feet / ft² / cfs** for every project; the **2D** solver is **SI**
(m, m², m³/s, g = 9.80665). The coupling always converts feet⇄metres
([`SolverOptions2D.hpp`](../src/engine/2d/data/SolverOptions2D.hpp), `len_*_to_*`,
`flow_*_to_*`; `area = len²`).

### Orifice flow and effective area
`2d/coupling/NodeCoupling.cpp`
- `orificeFlow` / `orificePhi` (≈30–44): `Q = Cd·A·sign(Δh)·√(2g)·φ(|Δh|)`, with
  `φ` a C¹-regularized √ below a 2 cm head (bounded sensitivity at `Δh → 0`).
- `effectiveArea` (≈47–53): `A_inlet` (= map `Area`) below `z_top`, ramping to
  `A_manhole = 2·Area` over a 5 cm band above it.

### Capped-pipe junction exchange
`2d/coupling/NodeCoupling.cpp` `computeCouplingExchange` (≈218–388):
- `crown = (invert_elev + full_depth)·len_1d_to_2d`; `z_top = crown` (the gate is
  tied to the crown — the slot-engagement point — *not* `crown + sur_depth`).
- Driver `Q = orificeFlow(h_2d − h_1d, Cd, A_eff)` — bidirectional head difference.
- **Capped-pipe gate** `capRamp` = Hermite smoothstep of `(max(h_1d,h_2d) − crown)`
  over a 5 cm band: `Q *= capRamp`. Below the crown ⇒ `Q = 0` (pipe flows sub-full,
  no spilling); above ⇒ full bidirectional exchange. `sur_depth` no longer shifts
  the gate — it sizes the 1D slot's surcharge-storage headroom above the crown.
- Source-side wet/dry `wetRamp` on `DRY_DEPTH` suppresses dry-cell exchange and
  kills spurious inflow from a dry low-spot vertex.
- Node-capacity and 2D-cell-volume caps throttle 2D→1D drainage conservatively.
- `scatterCouplingFlux` (≈55–121) distributes the exchange across the vertex
  stencil weighted by upwind HGL slope (conservative; geometric fallback on flat
  surfaces); a sink is drawn from the surrounding cells.

### Coupling map parsing and options
`2d/input/SectionHandlers2D.cpp` — `[2D_VERTEX_NODE_MAP]` / `[2D_TRIANGLE_NODE_MAP]`
(`Cd` default 0.65, `Area` default 1.0 m²), `COUPLING_CD`, `COUPLING_INTERVAL`.
`2d/data/SolverOptions2D.hpp` — `coupling_cd`, `coupling_interval`, `dry_depth`.

### Auto-aligned ponded area and the coupled-node pond exception
- `2d/SurfaceRouter2D.cpp` (≈264–320): overrides `nodes.ponded_area` on coupled
  non-outfall nodes with the median-dual footprint (`Σ stencil tri_area / 3`,
  converted by `len_2d_to_1d²`); sets `ctx.coupled_node[ni] = 1`.
- `hydraulics/DynamicWave.cpp`: `initNodeStates` (≈856) and `setNodeDepth` (≈2238)
  treat a `coupled_node` as pond-capable regardless of `ALLOW_PONDING`, so the HGL
  rises above the crown over `ponded_area` instead of being capped.

### Ponded-area mechanics (for contrast)
- `data/NodeData.hpp` `ponded_area`; `hydraulics/Node.cpp` `getPondedArea`
  (returns `ponded_area` only when flooded above the rim).
- Legacy `Aponded` is a flat pond that raises the head by `excess_volume /
  ponded_area`; gated by the `[OPTIONS] ALLOW_PONDING` flag for uncoupled nodes.

### Outfall coupling
`2d/coupling/NodeCoupling.cpp` `updateOutfallBoundaries` / `transferOutfallDischarges`;
`hydraulics/Outfall.cpp` `setAllOutfallDepths` (tailwater head BC with a wet/dry
ramp so a dry surface yields free discharge).
