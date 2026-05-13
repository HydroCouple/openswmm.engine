# Chapter 3: Dynamic Wave Analysis

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

The movement of water through a conveyance network of channels and pipes
is governed by the conservation of mass and momentum equations for
gradually varied, unsteady free surface flow. Dynamic wave analysis
solves the complete form of these equations and therefore produces the
most theoretically accurate results. It can account for channel storage,
backwater effects, entrance/exit losses, flow reversal, and pressurized
flow. Because it couples together the solution for both water levels at
nodes and flow in conduits it can be applied to any general network
layout, even those containing multiple downstream diversions and loops.
It is the method of choice for systems subjected to significant
backwater due to downstream flow restrictions and with flow regulation
via weirs and orifices. This generality comes at a price of having to
use small time steps to maintain numerical stability.

Dynamic wave modeling was first introduced into version 3 of SWMM in
1981 as a separate program module known as EXTRAN (Extended Transport)
(Roesner et al., 1983). The node-link solution method it uses had its
origins in the Sacramento-San Joaquin Delta Model (Shubinski et al.,
1965) and the WRE Transport Model (Kibler et al., 1975). Although more
powerful solution techniques are available (such as implicit finite
difference schemes (Cunge et al., 1980) and shock-capturing finite
volume schemes (Toro, 2001)), SWMM 5 continues to use EXTRAN's node-link
approach, with modifications made to enhance its stability, because of
its simplicity and versatility.

## 3.1 Governing Equations

The conservation of mass and momentum for unsteady free surface flow
through a channel or pipe are known as the St. Venant equations and can
be expressed as:

| | | | |
|---|---|---|---|
| $$\frac{\partial A}{\partial t} + \frac{\partial Q}{\partial x} = 0$$ | Continuity | (3-1) | |
| $$\frac{\partial Q}{\partial t} + \frac{\partial\left( \frac{Q^{2}}{A} \right)}{\partial x} + gA\frac{\partial H}{\partial x} + gAS_{f} = 0$$ | Momentum | (3-2) | |

where

| | | |
|---|---|---|
| *x* | = | distance (ft) |
| *t* | = | time (sec) |
| *A* | = | flow cross-sectional area (ft²) |
| *Q* | = | flow rate (cfs) |
| *H* | = | hydraulic head of water in the conduit (*Z* + *Y*) (ft) |
| *Z* | = | conduit invert elevation (ft) |
| *Y* | = | conduit water depth (ft) |
| *S*<sub>f</sub> | = | friction slope (head loss per unit length) |
| *g* | = | acceleration of gravity (ft/sec²) |

The derivation of these equations can be found in standard texts such as
Henderson (1966), Cunge et al. (1980) and French (1985). The assumptions
on which they are based are:

1.  flow is one dimensional

2.  pressure is hydrostatic

3.  the cosine of the channel bed slope angle is close to unity

4.  boundary friction can be represented in the same manner as for
    steady flow.

The friction slope *S*<sub>f</sub> can be expressed in terms of the Manning
equation used to model steady uniform flow:

$$S_{f} = \left( \frac{n}{1.486} \right)^{2}\frac{Q|U|}{AR^{4/3}}$$   (3-3)

where

| | | |
|---|---|---|
| *n* | = | the Manning roughness coefficient (sec/m<sup>1/3</sup>) |
| *R* | = | the hydraulic radius of the flow cross-section (ft) |
| *U* | = | flow velocity, equal to $\frac{Q}{A}$ (ft/sec). |

and 1.486 converts from m<sup>1/3</sup> to ft<sup>1/3</sup>. Use of the absolute value
sign on the velocity term makes *S*<sub>f</sub> a directional quantity (since *Q*
can be either positive or negative) and ensures that the frictional
force always opposes the flow. Manning roughness coefficients for wide
range of channel surfaces and pipe materials can be found in Appendix G.

For a specific cross-sectional geometry, the flow area *A* is a known
function of water depth *Y* which in turn can be obtained from the head
*H*. Thus the dependent variables in these equations are flow rate *Q*
and head *H*, which are functions of distance *x* and time *t*. To solve
these equations over a single conduit of length *L*, one needs a set of
initial conditions for *H* and *Q* at time 0 as well as boundary
conditions at *x* = 0 and *x* = *L* for all times *t*.

The continuity equation 3-1 can be combined with the momentum equation
3-2 to produce the following form of the momentum equation for a conduit
(see sidebar below for details):

$$\frac{\partial Q}{\partial t} = 2U\frac{\partial A}{\partial t} + U^{2}\frac{\partial A}{\partial x} - gA\frac{\partial H}{\partial x} - gAS_{f}$$                 (3-4)

> **Combining the Continuity and Momentum Equations**
> 
> The $\frac{\partial\left( \frac{Q^{2}}{A} \right)}{\partial x}$ term in the momentum equation 3-2 can be re-expressed as:
> 
> $$\frac{\partial\left( \frac{Q^{2}}{A} \right)}{\partial x} = \frac{\partial\left( U^{2}A \right)}{\partial x} = 2AU\frac{\partial U}{\partial x} + U^{2}\frac{\partial A}{\partial x}$$ (a)
> 
> Using $Q = UA$, the continuity equation 3-1 can be written as:
> 
> $$\frac{\partial A}{\partial t} + A\frac{\partial U}{\partial x} + U\frac{\partial A}{\partial x} = 0$$ (b)
> 
> Multiplying both sides of (b) by $U$ and re-arranging terms leads to:
> 
> $$AU\frac{\partial U}{\partial x} = - U\frac{\partial A}{\partial t} - U^{2}\frac{\partial A}{\partial x}$$ (c)
> 
> Substituting this into the first term on the right hand side of (a) produces:
> 
> $$\frac{\partial\left( \frac{Q^{2}}{A} \right)}{\partial x} = - 2U\frac{\partial A}{\partial t} - U^{2}\frac{\partial A}{\partial x}$$ (d)
> 
> Substituting (d) into 3-2 and re-arranging terms gives the final result:
> 
> $$\frac{\partial Q}{\partial t} = 2U\frac{\partial A}{\partial t} + U^{2}\frac{\partial A}{\partial x} - gA\frac{\partial H}{\partial x} - gAS_{f}$$ (e)

While this equation can be used to compute the time trajectory of flow
in a conduit, another relationship is needed to do likewise for heads.
SWMM's node -- link representation of the conveyance network,
conceptualized in Figure 3-1, does this by providing a continuity
relationship at junction nodes that connect conduits together within a
conveyance network. As shown in the figure, a continuous water surface
is assumed to exist between the water elevation at a node and in the
conduits that enter and leave it. Two types of nodes are possible.
Non-storage junction nodes are assumed to be points with zero volume and
surface area while storage nodes (such as ponds and tanks) contain both
volume and surface area.

![Node-Link.bmp](VolumeII/media/media/image11.png)

**Figure 3-1 Node-link representation of a conveyance network in SWMM (from Roesner et al, 1992).**

Each "node assembly" consists of the node itself and half the length of
each link connected to it. Conservation of flow for the assembly
requires that the change in volume with respect to time equal the
difference between inflow and outflow. In equation terms:

$$\frac{\partial V}{\partial t} = \frac{\partial V}{\partial H}\frac{\partial H}{\partial t} = A_{S}\frac{\partial H}{\partial t} = \sum_{}^{}Q$$   (3-5)

where:

| | | |
|---|---|---|
| *V* | = | node assembly volume (ft³) |
| *A*<sub>S</sub> | = | node assembly surface area (ft²) |
| *ΣQ* | = | net flow into the node assembly (inflow -- outflow) (cfs) |

The $\sum_{}^{}Q$ term includes the flow in the conduits connected to
the node as well as any externally imposed inflows such as wet weather
runoff or dry weather sanitary flow.

Each node assembly's surface area consists of the node's storage surface
area *A*<sub>SN</sub> (if it's a storage node) plus the surface area contributed
by the links connected to it, $\sum_{}^{}A_{SL}$, where *A*<sub>SL</sub> is the
surface area contributed by a connecting link. Thus the node continuity
equation can be written as:

$$\frac{\partial H}{\partial t} = \frac{\sum_{}^{}Q}{A_{SN} + \sum_{}^{}A_{SL}}$$   (3-6)

The flow depth at the end of a conduit connected to a node can be
computed as the difference between the head at the node and the invert
elevation of the conduit. The node and link surface areas are computed
as functions of their respective flow depths.

Equations 3-4 and 3-6 provide a coupled set of partial differential
equations that solve for flow *Q* in the conduits and head *H* at the
nodes of the conveyance network. Because they cannot be solved
analytically a numerical solution procedure must be used instead.

## 3.2 Solution Method

The material that follows applies to networks containing only conduits.
Inclusion of flow control devices (pumps, orifices, and weirs) and other
processes (seepage, evaporation, and minor losses) will be covered in
subsequent chapters of this manual.

The spatial and temporal derivatives in equations 3-4 and 3-6 can be
replaced with the following finite difference approximations:

$$\frac{\partial A}{\partial x} = \frac{\left( A_{2} - A_{1} \right)}{L}$$                 (3-7)

$$\frac{\partial H}{\partial x} = \frac{\left( H_{2} - H_{1} \right)}{L}$$                 (3-8)

$$\frac{\partial A}{\partial t} = \frac{\mathrm{\Delta}\overline{A}}{\mathrm{\Delta}t}$$                 (3-9)

$$\frac{\partial Q}{\partial t} = \frac{\mathrm{\Delta}Q}{\mathrm{\Delta}t}$$                 (3-10)

$$\frac{\partial H}{\partial t} = \frac{\mathrm{\Delta}H}{\mathrm{\Delta}t}$$                 (3-11)

where

| | | |
|---|---|---|
| *A*<sub>1</sub> | = | flow area at the upstream end of the conduit (ft²) |
| *A*<sub>2</sub> | = | flow area at the downstream end of the conduit (ft²) |
| *H*<sub>1</sub> | = | hydraulic head at the upstream end of the conduit (ft) |
| *H*<sub>2</sub> | = | hydraulic head at the downstream end of the conduit (ft) |
| *L* | = | conduit length (ft) |
| *∆t* | = | time step (sec) |
| *∆*$\ \overline{A}$ | = | change in average flow area, $\left( {\overline{A}}^{t + \mathrm{\Delta}t} - {\overline{A}}^{\ t} \right)$, over time step *∆t* (ft²) |
| *∆Q* | = | change in conduit flow, $\left( Q^{t + \mathrm{\Delta}t} - Q^{t} \right)$, over time step *∆t* (cfs) |
| *∆H* | = | change in nodal head, $\left( H^{t + \mathrm{\Delta}t} - H^{t} \right)$, over time step *∆t* (ft). |

with the superscripts referring to time periods.

Substituting these finite difference approximations into the link
momentum Equation 3-4, replacing *S*<sub>f</sub> with Equation 3-3, and replacing
*A, U*, and *R* with their average values over the conduit length (as
indicated by over scores) allows the finite difference form of the link
momentum equation to be written as:

$$\frac{\mathrm{\Delta}Q}{\mathrm{\Delta}t} = 2\overline{U}\frac{\mathrm{\Delta}\overline{A}}{\mathrm{\Delta}t} + {\overline{U}}^{2}\frac{\left( A_{2} - A_{1} \right)}{L} - g\overline{A}\frac{\left( H_{2} - H_{1} \right)}{L} - g\eta^{2}\frac{Q\left| \overline{U} \right|}{{\overline{R}}^{4/3}}$$   (3-12)

where $\eta = \frac{n}{1.486}$. Average values for *A, U*, and *R* can
be approximated using the heads *H*<sub>1</sub> and *H*<sub>2</sub> as described later on
in section 3.3.1.

The finite difference form of the nodal continuity equation 3-6 is:

$$\frac{\Delta H}{\Delta t} = \frac{\sum Q}{A_{SN} + \sum A_{SL}}$$   (3-13)

Previous versions of SWMM used an explicit forward Euler method (or more
precisely the two-step Modified Euler method) to solve Equation 3-12,
where known values of *Q, H, A*, $\overline{A}$, $\overline{U}$, and
$\overline{R}$ at time *t* were used to solve for *Q* at time *t + ∆t*.
Then Equation 3-13 was solved with the new conduit flows to find new
head values *H* at time *t + ∆t*.

SWMM 5 uses an implicit backwards Euler method instead to provide
improved stability (Ascher and Petzold, 1998). Under this scheme
Equation 3-12 is re-written as:

$$Q^{t + \Delta t} = \frac{Q^{t} + \Delta Q_{inertia} + \Delta Q_{pressure}}{1 + \Delta Q_{friction}}$$ (3-14)

where the terms are defined as:

*   **Inertial Term (3-14a):**
    $$\Delta Q_{inertia} = 2\overline{U}( \overline{A}^{t + \Delta t} - \overline{A}^{t} ) + \overline{U}^{2}\frac{( A_{2} - A_{1} )}{L}\Delta t$$

*   **Pressure Term (3-14b):**
    $$\Delta Q_{pressure} = - g\overline{A}\frac{( H_{2} - H_{1} )}{L}\Delta t$$

*   **Friction Term (3-14c):**
    $$\Delta Q_{friction} = g\eta^{2}\frac{\lvert \overline{U} \rvert\Delta t}{\overline{R}^{4/3}}$$

and now *H* and the quantities *A*, $\overline{A}$, $\overline{U}$, and
$\overline{R}$ derived from it are all evaluated at the new time *t+∆t*.
The finite difference form of the nodal continuity equation 3-12 can be
expressed as:

$$H^{t + \mathrm{\Delta}t} = H^{t} + \frac{\frac{\Delta t}{2}\left( \sum_{}^{}{Q^{t} + \sum_{}^{}Q^{t + \mathrm{\Delta}t}} \right)}{\left( A_{SN} + \sum_{}^{}A_{SL} \right)^{t + \mathrm{\Delta}t}}$$   for non-outfall       (3-15a)

$$H^{t + \mathrm{\Delta}t} = H_{Outfall}$$   for outfall nodes     (3-15b)

*H*<sub>Outfall</sub> is a user-supplied value that sets the head at a terminal
outfall node. It can be a constant value, a value extracted from a
user-supplied time series, or the elevation of the critical or normal
flow depth in the connecting conduit. For the latter option, critical or
normal depth is computed internally as a function of the conduit's flow
rate and geometry as described in Chapter 5.

Equations 3-14 and 3-15 can be solved implicitly over a given time step
*∆t* using functional iteration (also known as successive approximations
or Picard's method). The method is described in the sidebar titled
"*Dynamic Wave Solution Procedure*". Because flows and heads are updated
one conduit and node at a time and not simultaneously, the results at
each time step are invariant to the order in which the conduits and
links are evaluated. This allows Steps 2 and 4 of the solution procedure
to be implemented using separate threads running in parallel on
multi-processor computers which can offer a significant reduction in
computation time.

## 3.3 Computational Details

### 3.3.1 Average Cross-Section Properties

Evaluation of the flow updating formula 3-14 requires values for the
average area ($\overline{A}$), hydraulic radius ($\overline{R}$), and
velocity ($\overline{U}$) for the conduit in question. These values are
computed using heads *H*<sub>1</sub> and *H*<sub>2</sub> belonging to the most recently
computed head estimates *H<sup>last</sup>* at either end of the conduit. The flow
depth *Y*<sub>1</sub> at the upstream end of the conduit is computed as:

$$0 \text{ for } H_{1} \leq Z_{1}$$
$$H_{1} - Z_{1} \text{ for } Z_{1} < H_{1} \leq Z_{1} + Y_{full}$$
$$Y_{full} \text{ for } H_{1} > Z_{1} + Y_{full}$$
(3-16)

where Z*<sub>1</sub>* is the elevation of the invert of the upstream end of the
conduit and *Y*<sub>full</sub> is the full depth of the conduit. A similar
expression using *H*<sub>2</sub> and *Z*<sub>2</sub> applies to *Y*<sub>2</sub> at the downstream
end of the conduit.

> **Dynamic Wave Solution Procedure**
> 
> The following steps are used to update link flows and nodal heads over a given time step from *t* to *t + ∆t* for dynamic wave analysis:
> 
> 1. Initially let *Q<sup>last</sup>* and *H<sup>last</sup>* be the flow in each link and the head at each node, respectively, computed at time *t*. At time 0 these values are provided by the user-supplied initial conditions.
> 
> 2. Solve Equation 3-14 for each link producing a new flow estimate *Q<sup>new</sup>* for time *t + ∆t*, basing the values of *A*, $\overline{A}$, $\overline{U}$, and $\overline{R}$ on *H<sup>last</sup>*.
> 
> 3. Combine *Q<sup>new</sup>* and *Q<sup>last</sup>* together using a relaxation factor *θ* to produce a weighted value of *Q<sup>new</sup>*:
>    $$Q^{new} = (1 - \theta)Q^{last} + \theta Q^{new}$$
> 
> 4. Compute a value for *H<sup>new</sup>* at each node from Equation 3-15 using the flows *Q<sup>new</sup>* for *Q^t+∆t^* and the heads *H<sup>last</sup>* to evaluate $A_{S}^{t + \Delta t}$.
> 
> 5. As with flows, apply a relaxation factor to combine *H<sup>last</sup>* and *H<sup>new</sup>*:
>    $$H^{new} = (1 - \theta)H^{last} + \theta H^{new}$$
> 
> 6. If *H<sup>new</sup>* is close enough to *H<sup>last</sup>* for each node then the process stops with *Q<sup>new</sup>* and *H<sup>new</sup>* as the solution for time *t+∆t*. Otherwise, *H<sup>last</sup>* and *Q<sup>last</sup>* are set equal to *H<sup>new</sup>* and *Q<sup>new</sup>*, respectively, and the process returns to step 2.
> 
> **Notes:**
> - The relaxation factor *θ* is set to 0.5.
> - The convergence tolerance and maximum number of trials can be set by the user. Their default values are 0.005 feet and 8, respectively.
> - For links whose end node heads have already converged, steps 2 and 3 can be skipped and *Q<sup>new</sup>* can be set equal to *Q<sup>last</sup>*.

Values of $\overline{A}$ and $\overline{R}$ are computed from the
conduit's cross section geometry at the average flow depth
$\frac{\overline{Y} = \left( Y_{1} + Y_{2} \right)}{2}$. Formulas for
doing so are described in Chapter 5 of this manual. The average velocity
$\overline{U}$ is found by dividing the most current flow value
*Q<sup>last</sup>* by the average area $\overline{A}$.

In addition, the average area and hydraulic radius used in the pressure
and friction terms of equation 3-14 are upstream weighted to reflect how
close a conduit's flow is to being supercritical. Supercritical flow is
influenced only by upstream conditions (i.e., wave disturbances
propagate only in the downstream direction). The weight is derived from
the Froude number *Fr* for *Q<sup>last</sup>*:

$$Fr = \frac{\left| \overline{U} \right|}{\sqrt{g\frac{\overline{A}}{\overline{W}}}}$$   (3-17)

where $\overline{W}$ is the top water surface width at the average depth
$\overline{Y}$. (*Fr* is set to 0 for closed conduits flowing full). A
factor *σ* is then computed as:

$$1 \text{ for } Fr \leq 0.5$$
$$2(1 - Fr) \text{ for } 0.5 < Fr < 1$$
$$0 \text{ for } Fr \geq 1$$
(3-18)

It is used to modify the average area in Equation 3-14b and the average
hydraulic radius in Equation 3-14c as follows:

$${\overline{A}}' = A_{1} + \ \sigma\left( \overline{A} - A_{1} \right)$$      (3-19)

$${\overline{R}}' = R_{1} + \ \sigma\left( \overline{R} - R_{1} \right)$$      (3-20)

where *A*<sub>1</sub> and *R*<sub>1</sub> are the flow area and hydraulic radius,
respectively, based on the upstream flow depth *Y*<sub>1</sub>.

### 3.3.2 Surface Area Calculations

Under normal conditions the surface area that a conduit contributes to
its upstream node (*A*<sub>SL1</sub>) is the average top width of the water
surface over the upstream half of the conduit times half of the
conduit's length. In equation form:

$$A_{SL1} = \left( \frac{W\left( Y_{1} \right) + \ W(\overline{Y})}{2} \right)\frac{L}{2}$$   (3-21)

where *W(Y)* is the flow cross-section top width at a given flow depth
*Y* and $\overline{Y} = \frac{\left( Y_{1} + Y_{2} \right)}{2}$. A
similar expression applies to the downstream surface area *A*<sub>SL2</sub>.
*W(Y)* is computed from the conduit's cross-section geometry as
described in Chapter 5.

Because sewer systems are frequently built with pipe invert
discontinuities at manholes they can encounter free-fall conditions
where the water elevation in the node receiving flow is below the pipe's
invert elevation or the flow's critical depth. Also during periods of
filling or draining, conduits can have one end or the other dry. These
conditions require that adjustments be made to the way that flow depth
is assigned and to how surface area is computed.

Figure 3-2 illustrates the various types of special flow conditions that
affect surface area calculations:

1.  Case one is the normal situation of subcritical flow where flow
    depths and surface areas are computed as previously described.

2.  Case two represents a critical downstream condition. The conduit has
    a downstream offset and the water level at the node is below the
    flow's critical depth. The downstream depth is set equal to the
    smaller of the critical depth and normal depth for the current flow
    and all of the conduit's surface area is assigned to the upstream
    node.

3.  Case three is a critical upstream condition. There is reverse flow
    with a free-fall discharge into the upstream node. Adjustments
    equivalent to those for case two are made but with the definitions
    of upstream and downstream reversed.

4.  Case four depicts an upstream dry condition. The upstream end of the
    conduit is dry and the water level at the downstream end is below
    the upstream conduit invert. If there is an upstream invert offset
    then no surface area is assigned to the upstream node. A
    complementary set of rules applies to the opposite case of a
    downstream dry condition.

Table 3-1 summarizes the various flow conditions and the adjustments
that are made for each. Procedures for computing the critical depth and
normal depth for a given flow rate and cross-section geometry are
discussed in Chapter 5 of this manual.

Finally, to guard against the nodal head change formula 3-15 from
becoming unbounded as surface area becomes vanishingly small, a global
minimum surface area *A*<sub>Smin</sub> is imposed as follows:

$$A_{S} = max\left( A_{Smin},\ A_{SN} + \sum_{}^{}A_{SL} \right)$$   (3-22)

Its default value is 12.56 sq ft (i.e., the area of a 4-ft diameter
manhole) which can be overridden by the user. This is strictly a
computational device and does not add volume to a junction node (where
*A~SN~ = 0*) nor change it into a storage node.

![Pipe.bmp](VolumeII/media/media/image12.png)

![SurfaceArea2.bmp](VolumeII/media/media/image13.png)

**Figure 3‑2 Special flow conditions for dynamic wave analysis**

**Table 3-1 Surface area adjustments for various dynamic wave flow conditions**

| Condition | Criteria | Adjustments |
|---|---|---|
| Upstream Dry | *Y*<sub>1</sub> = 0<br>*Z*<sub>1</sub> > *E*<sub>1</sub> | *A*<sub>SL1</sub> = 0* if $H_{2} \leq Z_{1}$<br>otherwise use Upstream Critical adjustment |
| Downstream Dry | *Y*<sub>2</sub> = 0<br>*Z*<sub>2</sub> > *E*<sub>2</sub> | *A*<sub>SL2</sub> = 0* if $H_{1} \leq Z_{2}$<br>otherwise use Downstream Critical adjustment |
| Upstream Critical | *Q* < 0<br>*Z*<sub>1</sub> > *E*<sub>1</sub><br>*H*<sub>1</sub> -- *Z*<sub>1</sub> < *Y*\* | *Y*<sub>1</sub> = *Y*\*<br>*H*<sub>1</sub> = *Y*\* + *Z*<sub>1</sub><br>*A*<sub>SL1</sub> = 0<br>$$A_{SL2} = L\frac{\left( \overline{W} + W_{2} \right)}{2}$$ |
| Downstream Critical | *Q* > 0<br>*Z*<sub>2</sub> > *E*<sub>2</sub><br>*H*<sub>2</sub> -- *Z*<sub>2</sub> < *Y*\* | *Y*<sub>2</sub> = *Y*\*<br>*H*<sub>2</sub> = *Y*\* + *Z*<sub>2</sub><br>*A*<sub>SL2</sub> = 0<br>$$A_{SL1} = L\frac{\left( \overline{W} + W_{1} \right)}{2}$$ |
| Notes: | | |
| 1. *E*<sub>1</sub> = upstream node invert elevation, *E*<sub>2</sub> = downstream node invert elevation. | | |
| 2. *Z*<sub>1</sub> = upstream conduit invert elevation, *Z*<sub>2</sub> = downstream conduit invert elevation. | | |
| 3. *Y*\* = smaller of critical depth and normal depth at current conduit flow rate. | | |
| 4. Adjusted *H* values are only used in the flow updating Equation 3-14 and do not replace nodal head values. | | |

### 3.3.3 Inertial Damping

It has been found that reducing the contribution of the inertial terms
in the Saint Venant equation as the flow shifts between sub-critical and
supercritical states improves the solution's stability (see Fread et al.
(1996) where it is referred to as the Local Partial Inertia technique).
SWMM 5 offers the option to use the aforementioned σ factor to dampen
the inertial term ${\mathrm{\Delta}Q}_{inertia}$ in the flow updating
formula 3-14. As seen by equation 3-18, the factor is 1 for Froude
numbers up to 0.5, 0 for Froude numbers at 1 or higher, and varies
linearly in between. The damping factor σ is computed and applied on a
conduit by conduit basis.

Another option offered by SWMM 5 is to ignore the inertial term
completely. This corresponds to the so-called local inertial formulation
of the St. Venant equation (de Almeida and Bates, 2013). It drops the
convective acceleration term
$\left( \frac{\partial\left( \frac{Q^{2}}{A} \right)}{\partial x} \right)$
of the momentum equation 3-2 altogether resulting in
${\mathrm{\Delta}Q}_{inertia}$ being 0 in all conduits. (This is not the
same as the diffusion wave formulation which also drops the local
acceleration term $\left( \frac{\partial Q}{\partial t} \right)$ of the
momentum equation as well.) This option can also result in improved
stability particularly during periods of rapid flow change.

### 3.3.4 Flow Limitations

Each time a new flow is computed using Equation 3-14 it is checked to
see if it should be limited by the normal flow value for the upstream
flow depth and conduit slope. The following criteria are used to perform
this check:

1.  The computed flow is positive.

2.  The conduit is not flowing full.

3.  The conduit does not fall into any of the categories listed in Table
    3-1 (upstream / downstream dry or upstream / downstream critical).

4.  The water surface slope is less than the conduit's slope or the
    flow's Froude number based on upstream velocity and depth is greater
    than 1.

The last criterion can be limited to just slope, just Froude number or
either slope or Froude number as a program option. When all of these
criteria are satisfied the flow is limited to be no greater than that
found by the Manning equation (*Q*<sub>norm</sub>) using upstream conditions:

$$Q_{norm} = \frac{1.49}{n}A_{1}R_{1}^{2/3}\sqrt{S_{0}}$$   (3-23)

where *S*<sub>0</sub> is the conduit slope. Two other flow limiting conditions
are also checked. If the conduit was assigned an upper flow limit then
the flow is not allowed to exceed that value. If the conduit contains a
flap gate and the computed flow is negative then the flow is set to 0.

### 3.3.5 Surcharge Conditions

SWMM defines a node to be in a surcharged condition when all conduits
connected to it are full or when the node's water level exceeds the
crown of the highest conduit connected to it (see Figure 3-3). It should
be noted that surcharged (or pressurized) flow can occur in a closed
conduit without either of its end nodes being surcharged. For example,
if the node water level in Figure 3-3 was above the invert of pipe N+1
but below its crown, then pipes N and N-1 would remain pressurized
(assuming they were also full at their upstream ends) while the node
itself would no longer be surcharged.

**Figure 3-3 Illustration of a surcharged node**

![Surcharge4.bmp](VolumeII/media/media/image14.png)

When a node becomes surcharged there is no more volume available in the
conduits forming the node's assembly to absorb the difference between
inflow and outflow at the node. Thus $\frac{\partial V}{\partial t}$ in
the flow continuity Equation 3-5 is 0 and the surcharged nodal
continuity condition becomes:

$$\sum_{}^{}Q = 0$$                                        (3-24)

By itself, this equation is insufficient to update nodal heads at the
new time step since it only contains flows. In addition, because the
flow and head updating equations for the system are not solved
simultaneously, there is no guarantee that the condition will hold at
the surcharged nodes after a flow solution has been reached.

To enforce the surcharge flow continuity condition, it can be expressed
in the form of a perturbation equation:

$$\sum_{}^{}\left\lbrack Q + \frac{\partial Q}{\partial H}\mathrm{\Delta}H \right\rbrack = 0$$   (3-25)

where *∆H* is the adjustment to the node's head that must be made to
achieve a flow balance. Solving for *∆H* yields:

$$\mathrm{\Delta}H = \frac{- \sum_{}^{}Q}{\sum_{}^{}\frac{\partial Q}{\partial H}}$$   (3-26)

where the summations are made over all conduits that are connected to
the node in question.

The gradient of flow in a conduit with respect to the head at either end
node can be evaluated by differentiating the flow updating equation 3-14
resulting in:

$$\frac{\partial Q}{\partial H} = \frac{\frac{- g\overline{A}\mathrm{\Delta}t}{L}}{1 + \mathrm{\Delta}Q_{friction}}$$   (3-27)

The numerator of $\frac{\partial Q}{\partial H}$ has a negative sign in
front of it because when evaluating ΣQ flow directed out of a node is
considered negative while flow into the node is positive. It is computed
for each link at the same time that the link's flow is updated at Step 2
of the iterative process described in Section 3.3. The surcharge
equation 3-26 is analogous to the head updating formula used in the
Hardy Cross method for pressurized water distribution networks (Bhave,
1991).

To accommodate node surcharging, Step 4 of the iterative process that
updates a node's head is modified as follows. First the node is checked
to see if it is in a surcharged state, i.e., that it is not a storage or
outfall node and has *H<sup>last</sup>* greater than the top of the highest
connecting conduit *H*<sub>crown</sub>. If it is not surcharged then Equation
3-15 is used as before to update its head. Otherwise the following
modified form of Equation 3-26 is used to estimate the new head *H*<sup>new</sup>
for time *t + ∆t*:

$$H^{new} = H^{last} + \frac{\alpha\sum_{}^{}Q^{new}}{(1 - \beta)\sum_{}^{}\left( \frac{\partial Q}{\partial H} \right)^{last} + \frac{\beta A_{S}^{last}}{\mathrm{\Delta}t}}$$   (3-28)

where

| | | |
|---|---|---|
| *α* | = | 0.6 for upstream terminal nodes with only outflow links and 1.0 otherwise |
| *β* | = | $exp( - 15.0f_{H})$ |
| *f*<sub>H</sub> | = | $$\frac{\left( H^{last} - E \right)}{\left( H_{crown} - E \right) - \ 1}$$ |
| *H*<sub>crown</sub> | = | elevation of the crown of the node's highest connecting flowing conduit (ft) |
| *E* | = | elevation of the node's invert (ft) |
| $$A_{S}^{last}$$ | = | surface area of the node the last time it was not surcharged (ft²) |

The *α* factor is used to reduce oscillations in head at upstream
terminal nodes that have only outflow links (Roesner et al., 1992). The
*β* factor helps to reduce fluctuations in head when the node first
begins to surcharge (Roesner et al., 1980). At low surcharge depths it
makes the denominator in the head update formula be a weighted
combination of the pure surcharge formula 3-26 and the surface area
formula 3-15. By the time that the water level rises 25% above the
highest conduit, the equation is 98% pure surcharge.

The flow values used for $\sum_{}^{}Q$ are the new flow estimates found
from Step 3 of the solution procedure. The
$\frac{\partial Q}{\partial H}$ values are those that were last
evaluated at Step 2. And finally, empirical testing has shown that more
robust performance is obtained when under-relaxation is not applied to
*H*<sup>new</sup> at Step 5 of the solution procedure when surcharging occurs.

### 3.3.6 Preissmann Slot

As an alternative to the surcharge algorithm described in the previous
section, SWMM can utilize the Preissmann Slot Method (Cunge and Wegner,
1964) for handling pressurized flow in closed conduits. In this case the
conduit's cross-section is assumed to have a thin open slot at its top
which runs down its length. This permits the water level in the conduit
to exceed its full depth while only slightly increasing its flow area.
It thus becomes possible to compute a surface area contribution to the
conduit's end nodes once it reaches full depth. As a result, SWMM is
able to use its regular procedure for solving the open channel flow
equations 3-14 and 3-15 for all flow conditions without having to resort
to the surcharge algorithm.

In theory the width of the slot should be determined based on having the
celerity of an open channel gravity wave equal the speed of a pressure
wave affected by the compressibility of the elastic pipe wall. This
would result in a slot width *w*<sub>slot</sub> equal to:

> $w_{slot} = gA/c^{2}$ (3-29)

where *g* is the acceleration of gravity, *A* is the conduit's
cross-sectional area when full and *c* is the speed of the pressure
wave. The latter quantity depends on the conduit's diameter, wall
thickness, and modulus of elasticity and typically ranges from a few
hundred to several thousand ft/sec (Yen, 2001).

Some care is needed in choosing a slot width since too large a value
will result in reduced accuracy while too small a value can cause
numerical instabilities. There is also the issue of maintaining a smooth
transition between almost full flow and slot flow. The choice used by
SWMM is a modified version of a formula proposed by Sjőberg (1982) and
is given by:

> $\frac{w_{slot}}{W_{\max}} = 0.5423\exp\left( - \left( \frac{Y}{Y_{full}} \right)^{2.4} \right)$
> (3-30)

where *W*<sub>max</sub> is the conduit's maximum width, *Y*<sub>full</sub> is its full
depth, and Y is depth of flow. This equation applies to
$\frac{Y}{Y_{full}}$ values between 0.985257 and 1.7. Below this range
the slot is not used while above it the slot width relative to *W*<sub>max</sub>
is clamped at 0.01. The range's lower limit was chosen so that the width
computed from equation 3-30 is the same as the width across a circular
pipe at that flow depth. This helps produce a smooth transition between
open channel and pressurized flow regimes.

When the slot method is employed, equation 3-16 is modified so that *Y*
is no longer limited by *Y*<sub>full</sub>. When *Y* reaches the limit at which
the slot formula applies, its resulting width is used to compute the
surface area that a conduit contributes to its end nodes as described in
Section 3.3.2. It also contributes to the conduit's flow area when it
rises above the full depth. It is not used when computing the conduit's
hydraulic radius.

### 3.3.7 Flooding and Ponding

Each non-outfall node is assigned a maximum allowable head *H*<sub>max</sub> by
the user. It consists of both a maximum free water surface elevation
that can exist at the node plus an optional "surcharge" depth that
allows for pressurization. For example, if the node were a manhole
junction *H*<sub>max</sub> would typically be the ground surface elevation. If it
were a storage unit it would be the water surface elevation when the
unit is full. For a junction between natural channels it would be the
top of the highest channel. For a fitting that connects pipe segments
together it would be the top of the highest pipe. In the latter case a
large surcharge depth (such as several hundred feet) should be assigned
to the fitting junction so that the connected pipes can pressurize if
need be. A manhole junction might also be assigned a surcharge depth if
it has a bolted cover.

Normally when the new head estimate *H*<sup>new</sup> at a node computed at Step
5 of the iterative solution process exceeds *H*<sub>max</sub> it is set equal to
*H*<sub>max</sub> and the node becomes flooded. The overflow rate *Q*<sub>ovfl</sub>
associated with this condition is the average net flow rate (inflow --
outflow) seen by the node over the current time step:

$$Q_{ovfl} = 0.5\left( \sum_{}^{}{Q^{t} + \sum_{}^{}Q^{t + \mathrm{\Delta}t}} \right)$$   (3-31)

This flow is then lost from the system, the same as the flow entering a
terminal outfall node.

The option exists for a junction node with no surcharge depth (and thus
always maintaining a free surface) to have excess flooded water pond
atop the node (see Figure 3-4). In this case the user assigns the node a
"ponded area" parameter, *A*<sub>P</sub>, that creates a virtual storage area on
top of the node and *H*<sup>new</sup> is no longer limited to *H*<sub>max</sub> . When
*H*<sup>new</sup> exceeds *H*<sub>max</sub> the ponded node is treated as a normal storage
node whose head is updated using the normal, non-surcharge formula
Equation 3-15 with *A*<sub>SN</sub> = *A*<sub>P</sub>. The only exception to this is when
the node transitions between having a head below *H*<sub>max</sub> to a flooded
head above *H*<sub>max</sub> (or vice versa) within a time step. In this case the
updated head is restricted to be just a small value above *H*<sub>max</sub> (or
below it in the opposite case) to avoid wide swings in head during the
transition.

![Surcharge5.bmp](VolumeII/media/media/image15.png)

**Figure 3-4 Ponding of excess water above a junction**

When a node is allowed to pond, flooded water is not lost from the
system. The ponded depth above the node will rise during periods of flow
excess (i.e., inflow greater than outflow) and fall during periods of
flow deficit. A node with a large ponded area will see smaller changes
in ponded depth for a given flow excess (or deficit) than will one with
a small ponded area. Selection of which nodes can pond and their
respective ponded areas would depend on local topography, typically
occurring along flat sections or at sag points of the drainage system.

### 3.3.8 Summary of Special Conditions

Here is a summary of the special conditions that are applied to the
basic iterative solution process for dynamic wave analysis described
earlier in Section 3.2:

1.  Upstream weighting, based on the current flow's Froude number, is
    applied to the average area in the pressure term and to the average
    hydraulic radius in the friction term of the flow updating formula
    3-14 (see Section 3.3.1).

2.  Optional inertial damping, again based on the Froude number, is
    applied to the inertial term of the flow updating formula 3-14 (see
    Section 3.3.2).

3.  The surface area contributed by a conduit to its end nodes in the
    head updating formula 3-15 is modified when either critical flow
    depth or dry conditions occur (see Section 3.3.3).

4.  A conduit's updated flow is limited to the Manning normal flow if
    warranted by water surface slope and/or Froude number criteria (see
    Section 3.3.4).

5.  If SWMM's Surcharge Algorithm is used then the head updating formula
    3-15 is replaced with equation 3-28 when a node is in a surcharged
    state (see Section 3.3.5).

6.  If SWMM's Slot Method is used then no adjustment to equation 3-15 is
    necessary as the computed slot width will be used to compute surface
    and flow areas in a full flowing conduit (see Section 3.3.6).

7.  If a node is assigned a ponded area then a virtual storage unit of
    constant surface area is used along with equation 3-15 to update its
    head when it exceeds the node's maximum value. Otherwise a node's
    head cannot exceed its maximum value and any excess inflow it
    receives is lost from the system (see Section 3.3.7).

## 3.4 Numerical Stability

The numerical stability of SWMM's dynamic wave results can be affected
by the choice of the simulation time step. Numerical instability is
characterized by oscillations in flow and water surface elevation that
do not dampen out over time. Another indicator of numerical instability
is a node which continues to "dry up" on each time-step despite a
constant or increasing inflow from upstream sources.

Aside from examining the results for each conduit and node, SWMM 5
provides two metrics in its Status Report that can help determine if a
solution shows signs of instability. One is the overall flow continuity
error for the system. This is the difference between inflow and outflow
for the entire system over the duration of the simulation. If this
number is greater than 5 to 10 percent then the cause may be numerical
instability (although other factors can affect the continuity error as
well).

A second metric is a link's Flow Instability Index (FII). This index
counts the number of times that the flow value in a link is higher (or
lower) than the flow in both the previous and subsequent time periods.
The index is normalized with respect to the expected number of such
'turns' that would occur for a purely random series of values and can
range from 0 to 150. The Status Report identifies the links having the
five highest FII's. Unfortunately since the FII does not take into
account the magnitude of the flow fluctuations it cannot determine
whether the instability is of engineering significance or not.

Stable explicit solutions of the St. Venant equations require that the
time step be no longer than the time it takes for a dynamic wave to
travel the length of the conduit (Cunge et al., 1980). This is known as
the Courant-Friedrichs-Lewy (CFL) condition and can be expressed as:

$$\mathrm{\Delta}t \leq \frac{L}{\left| \overline{U} + c \right|}$$   (3-30)

where *c* is the wave celerity given by:

$$c = \sqrt{g\frac{\overline{A}}{\overline{W}}}$$          (3-31)

An equivalent form of this condition can be written as:

$$\mathrm{\Delta}t \leq \frac{L}{\left| \overline{U} \right|}\left( \frac{Fr}{1 + Fr} \right)Cr$$   (3-32)

where *Fr* is the flow's Froude number (see Equation 3-17) and *Cr* is
the Courant number. The latter serves as an adjustment parameter that
determines how conservative (*Cr* < 1) or liberal (*Cr* > 1) one
wishes to be in strictly meeting the CFL condition (*Cr* = 1).

Although the SWMM 5 solution method uses an iterative implicit procedure
in time to update flows and heads, it does so one conduit and node at a
time, not simultaneously. There is no spatial coupling between elements
as would occur in an unconditionally stable implicit solution scheme.
Thus the CFL condition would still apply but perhaps not as strictly (by
allowing one to use a *Cr* value greater than 1).

One can estimate a *∆t* for each conduit by using the conduit's full
depth *Y*<sub>full</sub> in place of $\frac{\overline{A}}{\overline{W}}$ in
Equation 3-31 and ignoring the velocity in Equation 3-30. The solution
time step would then be determined by the conduit with the smallest
value of $\frac{L}{\sqrt{gY_{full}}}$ . Short conduits lead to small
time steps and longer computational times. Time steps of 10 to 30
seconds should suffice for conduit lengths of 200 to 400 feet (the
typical spacing between sewer manholes) and full depths from 1 to 4
feet.

An option is available to artificially lengthen short conduits so that
the CFL condition for a given user-supplied time step *∆t* is met. The
modified length $L'$ is given by

$$ L' = \max\{ L, \Delta t ( \sqrt{gY_{full}} + \frac{Q_{full}}{A_{full}} ) \} $$ (3-33)

where *Q*<sub>full</sub> is the Manning's normal flow value (Equation 3-23)
evaluated at full depth *Y*<sub>full</sub> and *A*<sub>full</sub> is the flow area at
full depth. This modified length is used in place of the original length
in the equations presented in section 3.4. To make the artificially
lengthened conduit have a flow resistance equivalent to the original
length, its slope *S*<sub>0</sub> and roughness coefficient *n* are adjusted so
that the Manning equation produces an equal head loss across both the
original and lengthened conduit for any given flow. The modified slope
$S_{0}'$ for the lengthened conduit is:

$$S_{0}' = S_{0}\sqrt{\frac{L}{L'}}$$                      (3-34)

while its modified roughness $n'$ is:

$$n' = n\sqrt{\frac{L}{L'}}$$                              (3-35)

The conduit lengthening option is applied to all conduits whenever the
user supplies a non-zero value for the "lengthening" time step to be
used in equation 3-33. This time step does not have to be the same as
the computational time step used to solve the dynamic wave equations.

Another option available in SWMM 5 is to have the program use a variable
computational time step that is adjusted throughout the simulation. The
user supplies values of the smallest allowable time step (*∆t*<sub>min</sub>),
the largest allowable time step (*∆t*<sub>max</sub>) and a desired Courant number
(*Cr*) to be met. At any time *t*, the next time step is computed from
the smaller of:

1.  The smallest value of

$$\frac{L}{\left| \overline{U} \right|}\left( \frac{Fr}{1 + Fr} \right)Cr$$   

for all conduits with non-negligible Fr.

2.  The smallest value of

$$\frac{0.25\left( H_{crown} - E \right)}{{\mathrm{\Delta}H}^{t}}$$   

for all non-outfall nodes that are not surcharged.

The second condition guards against an excessive change in node head
over a single time step. Both conditions are evaluated using the flow
and head solutions found at time *t* (${\mathrm{\Delta}H}^{t}$ is the
change in head found from the prior time step). The resulting time step
is not allowed to be less than *∆t*<sub>min</sub> nor greater than *∆t*<sub>max</sub>. The
initial time step used at time 0 is *∆t*<sub>min</sub>.

To illustrate these concepts consider a 2 ft x 2 ft rectangular conduit
that is 2,000 ft long with a 0.05% slope and has a Manning's roughness
of 0.015 (see Figure 3-5). When divided into 10 equal length sections of
200 ft each the estimated stable time step is
$\frac{200}{\sqrt{32.2 \times 2} = 25}$ seconds. When analyzed as just a
single 2,000 ft long section it increases to 250 seconds.

![Example1a.png](VolumeII/media/media/image16.png)

**Figure 3-5 Profile view of example rectangular conduit (not to scale)**

Figure 3-6 shows the outflow hydrographs for these two analysis options
for a 1-hour sinusoidal inflow hydrograph with peak flow of 10 cfs (the
dotted curve in the figure). Both results are completely stable. The
option with the higher spatial resolution produces a more skewed
hydrograph with a slightly lower peak.

<figure>
<img src="VolumeII/media/media/image17.png"
style="width:5.30407in;height:3.95493in" alt="Example1b.png" />
</figure>

**Figure 3-6 Outflow hydrographs for example conduit -I**

Now consider what happens when the 10-section conduit is analyzed with a
fixed time step of 120 seconds which is much larger than the stable
fixed step of 25 seconds. As shown in Figure 3-7 the solution becomes
completely unstable. When 120 seconds is used as the upper limit of a
variable time step a stable result is produced. In this case SWMM's
Status Report shows that the variable time step ranged from 24 to 120
seconds with the average being 42.

<figure>
<img src="VolumeII/media/media/image18.png"
style="width:5.30407in;height:3.95493in" alt="Example1c.png" />
</figure>

**Figure 3-7 Outflow hydrographs for example conduit – II**
