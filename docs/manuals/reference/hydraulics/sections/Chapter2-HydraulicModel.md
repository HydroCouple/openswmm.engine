@page hydraulics_ref_ch2_hydraulic_model Chapter 2: SWMM's Hydraulic Model

@tableofcontents

________________________________________________________________________________

As mentioned in @ref hydraulics_ref_ch1_overview "Chapter 1", SWMM models the conveyance portion of a
drainage system as a network of links connected together at nodes.
External flows from various sources enter the network at specific nodes,
are transported along links, are combined together and split apart at
internal nodes while filling and emptying the volume of storage nodes,
and exit the system at terminal nodes. Figure 2-1 shows how a physical
system of sewer lines and their appurtenances are abstracted into a
network of nodes and links of different types (pipe and pump links;
junction, storage and outfall nodes for this particular example).

![SewerSystem2.png](hydraulics/media/media/image7.jpg)

**Figure 2-1 Node-link representation of a sewer system**
**(Background from http://www.sewerhistory.org/photosgraphics/japan/)**

Table 1-2 has already summarized the different types of node and link
objects that can appear in a SWMM conveyance network model. The
remainder of this chapter provides more details on the properties of
network objects, briefly describes and compares the capabilities of the
two principal methods used for analyzing the unsteady hydraulic behavior
of a network, and discusses the boundary and initial conditions needed
to compute network hydraulics.

## 2.1 Network Components

The two principal components of a SWMM conveyance system network are
nodes and links. Nodes represent the end points of conveyance links that
form the connection between two or more links. They are also the points
where external inflows (runoff, dry weather flows, etc.) can enter the
network or where internal flows leave the network. Links are conveyance
elements that transport flow between nodes. The following paragraphs
describe the different types of nodes and links that SWMM can model.

### 2.1.1 Junction Nodes

Junction nodes are points in the drainage system where conveyance links
join together. Physically they can represent the confluence of natural
surface channels, manholes in a sewer system, or pipe connection
fittings. Excess water at a junction can become partially pressurized
when connecting conduits are surcharged and can either be lost from the
system or be allowed to pond atop the junction and subsequently drain
back into the junction.

The principal input parameters for a junction node are:

- invert (channel or manhole bottom) elevation

- height between its invert and the ground surface

- additional pressure head that can be accepted before flooding occurs

- ponded surface area when flooded.

### 2.1.2 Outfall Nodes

Outfall nodes are terminal nodes of the drainage system used to define
final downstream boundary locations. The boundary conditions at an
outfall can be described by any one of the following stage
relationships:

- the critical or normal flow depth in the connecting conduit

- a fixed stage elevation

- a tidal stage described in a table of tide height versus hour of the
  day

- a user-defined time series of stage versus time.

The principal input parameters for an outfall node are:

- invert elevation

- type of boundary condition and its associated stage data

- presence of a flap gate to prevent backflow through the outfall.

### 2.1.3 Flow Divider Nodes

Flow divider nodes divert inflows to a specific link in a prescribed
manner. A flow divider can have no more than two conduit links on its
discharge side. There are four types of flow dividers, defined by the
manner in which inflows are diverted:

- *Cutoff* diverts all inflow above a defined cutoff value.

- *Overflow* diverts all inflow above the flow capacity of the
  non-diverted conduit.

- *Tabular* uses a table that expresses diverted flow as a function of
  total inflow.

- *Weir* uses a weir equation to compute diverted flow.

The principal input parameters for a flow divider node are:

- junction parameters (see above)

- name of the link receiving the diverted flow

- method used for computing the amount of diverted flow.

### 2.1.4 Storage Unit Nodes

Storage unit nodes are the only type of node that can provide storage
volume and possess surface area. Physically they could represent storage
facilities as small as a catch basin or as large as a lake. The
volumetric properties of a storage unit are described by a function or
table of surface area versus height. In addition to receiving inflows
and discharging outflows to other nodes in the drainage network, storage
nodes can also lose water from surface evaporation and from seepage into
native soil. Unlike other nodes, storage nodes are not allowed to
pressurize (i.e., they always maintain a free surface).

The principal input parameters for a storage unit are:

- invert (bottom) elevation

- maximum depth

- depth-surface area data

- evaporation potential

- seepage parameters.

#### 2.1.4.1 Layered LIDs on storage nodes {#hydraulics_ref_lid_storage_formulation}

**Open-Source SWMM 6 extension.** A layered LID can be assigned to an ordinary
storage node with Dynamic Wave routing. Its footprint comes from the storage
geometry and its thicknesses from the ordered SURFACE, MEDIA and AGGREGATE
layers. MEDIA is divided into five numerical cells per authored layer.
These cells describe retained moisture; they are not separate hydraulic nodes.
Connections, controls and downstream stages belong to the ordinary network.
Unlike the single underdrain definition and opening/closing threshold pair
of a conventional subcatchment LID, a storage-node LID can have any number
of connections at different elevations. Each controllable link can have
independent rules and settings. An orifice used as a valve permits partial
openings between settings 0 and 1; its discharge still follows its hydraulic
law and the heads at both ends.

For cell geometric volume \f$G_i\f$, void fraction \f$\phi_i\f$, retained
fraction \f$\theta_i\f$ and mobile water-table depth \f$h\f$ above the invert:

\f[
 V_r=\sum_i\theta_iG_i,\qquad
 V_m(h)=\sum_i(\phi_i-\theta_i)G_i\,s_i(h),\qquad
 V=V_r+V_m.
\f]

Here \f$s_i(h)\f$ is the fraction of cell thickness below the mobile water
table, bounded to [0,1]. Below that table, mobile water fills the pores left
by the retained fraction, making physical moisture equal to porosity.
Surface void fraction includes vegetation displacement. Reported node volume
includes both inventories; the routing continuity equation uses mobile volume.
Internal drainage debits one store and credits the other with the same volume:

\f[
 \frac{dV}{dt}=Q_{in}-Q_{out}-Q_{overflow}-Q_{flood}-E-Q_{seep}.
\f]

The flow terms use accepted directions. Rainfall must enter through a
contributing subcatchment or external inflow; assigning a LID is not an
additional rainfall source.

**Gravity drainage.** MEDIA uses the exponential conductivity law of legacy
SWMM LIDs, evaluated from the donor cell:

\f[
 q_i=\begin{cases}
 K_{s,i}\exp[-m_i(\phi_i-\theta_i)],&\theta_i>\theta_{FC,i},\\
 0,&\theta_i\leq\theta_{FC,i},
 \end{cases}
 \qquad Q_i=A_iq_i,\quad A_i=G_i/\Delta z_i.
\f]

The dimensionless, nonnegative \f$m_i\f$ is **conductivity slope**, not a
power exponent. Zero slope gives constant saturated conductivity above field
capacity. AGGREGATE uses its specified conductivity and zero field capacity.
With downward-positive coordinate \f$\xi\f$, Darcy–Buckingham gives
\f$q=K(\theta)(1-\partial\psi/\partial\xi)\f$. The node drainage law takes
the unit-gradient limit; it does not compute intercell matric-head gradients.

**Surface entry.** SURFACE directly over MEDIA reuses modified Green–Ampt.
For an existing ponded downward front, its instantaneous capacity is:

\f[
 f_{cap}=K_s\left[1+\frac{(\psi_f+h_s)\,\Delta\theta}{F}\right],
 \qquad h_s=\frac{\theta_s\Delta z_s}{\phi_s},\quad F>0.
\f]

The positive \f$\psi_f\f$ is front suction, \f$\Delta\theta\f$ is its
moisture deficit and \f$F\f$ is active-front cumulative infiltration depth.
The native routine handles initial wetting at F=0, integrated infiltration
and supply limits. Increased ponded head increases capacity at the same
front state. A trial history supplies potential flux; only accepted
infiltration increments F and upper-zone wetness. Surface-to-aggregate entry
uses aggregate conductivity; deeper MEDIA layers have no separate fronts.

**Accepted transfer.** Explicit substeps are at most one second. For a
retained receiver above the water table:

\f[
 \Delta V_i=\min\{Q_i\delta t,
 (\theta_i-\theta_{FC,i})_+G_i,
 (\phi_{i+1}-\theta_{i+1})_+G_{i+1}\}.
\f]

All trial fluxes use a common pre-update state. A receiver intersected by the
mobile table routes to mobile storage instead, without a retained-capacity
bound. Donors below that table skip free drainage. Surface entry has zero
field capacity and the same volume bounds. Evaporation is bounded so media
cannot fall below wilting point. BOTTOM seepage is an external loss.

**Ports, reversal and resaturation.** Link ports use their physical offset
and local available water. In particular, a surface overflow uses ponding
head, which can be higher than the reported mobile water table. Shared
interface ownership tolerates only roundoff-sized elevation differences;
it is used by both hydraulics and pollutant routing. Fully submerged orifices
without a flap gate can reverse according to the sign of their head difference;
see @ref hydraulics_ref_ch6_pumps_regulators for the full structure equations.

Reverse flow enters its actual port. Media receipts fill local retained
capacity before excess joins mobile storage. The infiltration upper-zone
deficit is averaged over the finite first-media zone, counting submerged
pores as saturated. Rising backwater or accepted media-port wetting reduces
the deficit and raises wetness, without adding reverse water to F. Full
media-top submergence clears the old downward front. During gradual recession,
the history follows remaining moisture until accepted surface entry starts
a new approximate front. Previously submerged cells retain field-capacity
moisture through conservative retained/mobile transfers. Dry history recovery
cannot make the zone drier than its physical moisture and is suppressed when
an empty surface overlies an upper zone intersected by backwater.

Native V10 hotstarts preserve that history, last reconciled head, moisture
and retained pollutant mass. Compatible older files reconstruct missing
infiltration history and warn that exact continuation is unavailable.

This reduced gravity-drainage/front model is distinct from a Richards solve
or the gravity-plus-matric-diffusivity block formulation of
[Tu, Wadzuk and Traver (2020)](https://doi.org/10.1371/journal.pone.0235528).
It omits upward capillary redistribution, retention hysteresis and capillary
barriers. Its applicability requires assessment against observations or a
richer vadose-zone model when those processes matter. Check routing-step
sensitivity of timing, peaks and treatment, as well as continuity. Treatment
and signed mass transfers are defined in @ref quality_ref_lid_storage_formulation;
configuration and restart compatibility are in @ref engine_manual_lid_storage.

### 2.1.5 Conduit Links

Conduit links are pipes or channels that move water from one node to
another in the conveyance network. Their cross-sectional shapes can be
selected from a variety of standard open and closed geometries. Custom
closed shapes for pipes and irregular cross-section profiles for open
channels can also be specified. Conduit geometry is discussed in more
detail in @ref hydraulics_ref_ch5_cross_section "Chapter 5".

The required input parameters for a conduit link are:

- identities of the inlet and outlet nodes

- offset height or elevation above the inlet and outlet node inverts

- conduit length

- Manning's roughness coefficient

- cross-section shape and dimensions.

![Link_offset.bmp](hydraulics/media/media/hydraulics-image8.png)

SWMM allows conduits to be offset some distance above the invert of their connecting end nodes as shown in the figure on the right. The offset can be specified as either a distance above the invert (i.e., the distance between points 1 and 2 in the figure) or as the elevation of the conduit's invert (i.e., the elevation of point 1). Internally the offset is maintained as an elevation.

![slope.png](hydraulics/media/media/hydraulics-image9.png)

SWMM also makes use of a conduit's slope in its hydraulic calculations. Slope is not provided directly as an input variable but is instead computed from the elevation of a conduit's end node inverts and its offsets. Let *L* be the length of the conduit, *∆y* be the difference in elevation and *∆x* the horizontal distance between the invert at each end of the conduit. Then from the diagram on the right:

*∆x* = √(*L*<sup>2</sup> - *∆y*<sup>2</sup>)   (2-1)

and the conduit slope *S*<sub>0</sub> is:

*S*<sub>0</sub> = *∆y*/*∆x*      (2-2)

SWMM does not allow a slope of 0. Therefore it imposes a minimum value
of 0.001 ft on *∆y*. It also allows the user to set a non-zero value for
minimum slope which will override any smaller computed slope.

SWMM uses the Manning equation to relate conduit flow rate to flow depth
and conduit bed or friction slope. It therefore requires the user to
supply a Manning's "*n*" coefficient that represents the roughness
characteristics of the conduit's surface. Values of the coefficient for
a wide range of channel types and pipe materials can be found in
Appendix G.

Conduits can also include the following optional parameters:

- presence of a flap gate to prevent reverse flow

- entrance/exit loss coefficients

- seepage rate

- inlet geometry code number if the conduit acts as a culvert.

The latter three properties are employed by the advanced modeling
features covered in Chapter 7 of this manual.

### 2.1.6 Pump Links

Pump links are used to lift water from an inlet node to an outlet node
at higher elevation. The principal input parameters for a pump include:

- identities of its inlet and outlet nodes

- pump curve data

- initial on/off status

- startup and shutoff depths.

A pump curve describes the relation between a pump's flow rate and the
head at its inlet and outlet nodes. The inlet node's startup and shutoff
water depths are monitored continuously during the course of a
simulation to allow for automated control of the pump's on/off status.

Pumps are directional devices that are not allowed to have reverse flow
through them. Their hydraulic performance is described in more detail in
@ref hydraulics_ref_ch6_pumps_regulators "Chapter 6".

### 2.1.7 Flow Regulator Links

Flow regulator links model structures or devices used to control and
divert flows within a conveyance system. They are typically used to
control releases from storage facilities, prevent unacceptable
surcharging, and divert flow to treatment facilities and interceptors.

SWMM can model the following types of flow regulators: orifices, weirs,
and outlets. The hydraulic behavior of orifices and weirs is modeled
using standard rating curves (the nonlinear relation between hydraulic
head applied to the regulator and the flow rate through it). Outlets
utilize a user-supplied rating curve.

The principal input parameters for a flow regulator link include:

- identities of its inlet and outlet nodes

- offset above the invert of its inlet node

- dimensions of its opening (for orifices and weirs)

- parameters that describe its rating curve

- presence of a flap gate to prevent reverse flow.

The hydraulic performance of regulator links is described in more detail
in @ref hydraulics_ref_ch6_pumps_regulators "Chapter 6".

### 2.1.8 Control Rules

Each pump and flow regulator has a setting property that can adjust:

- a pump's on/off status

<!-- -->

- a pump's speed

- the size of an orifice opening

- the crest height of a weir

- the flow through an outlet link

The setting can be changed during a simulation by using control rules.
These specify conditions, such as water elevation at certain nodes, flow
in certain links, and simulation time, that trigger a specified change
in a link's setting. SWMM's hydraulic analysis methods take into account
the current setting for each pump and flow regulator in the conveyance
network. More details on the formats used for control rules can be found
in the SWMM 5 Users Manual (US EPA, 2010).

## 2.2 Analysis Methods

SWMM's hydraulics solves the equations of one-dimensional, gradually
varied, unsteady flow throughout a node-link network to determine the
water level at each node and the flow rate and flow depth within each
link at each time step of an extended simulation period. Flow routing of
inflow hydrographs along channels and sewers entails wave dispersion,
wave attenuation or amplification, and wave retardation or acceleration.
These wave characteristics constitute the hydraulics of flow routing or
propagation and are greatly affected by the geometric characteristics of
the conduits, the characteristics of sources and/or sinks, and by
initial and boundary conditions.

The hydraulics of unsteady non-uniform flow is represented in SWMM by a
pair of partial differential equations of conservation of mass and
momentum known as the St. Venant equations. Simultaneous solution of
these equations for each conduit, coupled with a conservation of volume
at each node, provides information on the spatial and temporal variation
of water levels and discharge rates throughout the network. SWMM offers
the user two principal alternative methods for solving these equations -
dynamic wave or kinematic wave analysis

Dynamic wave analysis solves the complete form of the St. Venant flow
equations and therefore produces the most theoretically accurate
results. It can account for channel storage, backwater effects,
entrance/exit losses, culvert flow, flow reversal, and pressurized flow.
Because it couples together the solution for both water levels at nodes
and flow in conduits it can be applied to any general network layout,
even those containing multiple downstream diversions and loops. It is
the method of choice for systems subjected to significant backwater due
to downstream flow restrictions and with flow regulation via weirs and
orifices. This generality comes at a price of having to use small time
steps to maintain numerical stability.

Kinematic wave analysis solves the continuity equation along with a
simplified form of the momentum equation in each conduit. It cannot
account for backwater effects, entrance/exit losses, flow reversal, or
pressurized flow. It is most applicable to steeply sloped (e.g., \>
0.1%) conduits with shallow flow with high velocity. It can usually
maintain numerical stability with much larger large time steps than are
required for dynamic wave analysis. If the aforementioned effects are
not expected to be significant then this alternative can be an accurate
and efficient hydraulic analysis method, especially for long-term
simulations.

Because kinematic wave analysis ignores both inertial and pressure
forces there are limits on its applicability:

1.  It can only analyze directed acyclic networks (where conduits are
    oriented in the direction of positive slope and there are no paths
    that start and end at the same node).

2.  Junction nodes can only have at most one outlet link which must be a
    conduit.

3.  Divider nodes must have two outlet links which must be conduits.

4.  Storage nodes can have any number of outlet links of any type.

5.  Upstream offsets for conduits are ignored except at storage nodes.

SWMM also offers a steady flow analysis option which assumes that within
each computational time step flow is uniform and steady. It simply
translates inflow hydrographs at the upstream end of a conduit to its
downstream end, with no delay or change in shape. The Manning equation
is used to relate flow rate to flow area (or depth). It is subject to
the same limitations as the kinematic wave method. Because it ignores
the dynamics of free surface wave propagation it is only appropriate for
rough preliminary analysis of long-term continuous simulations.

Table 2-1 compares the features and limitations of the dynamic wave and
kinematic wave methods of hydraulic analysis. Dynamic wave solutions
tend to attenuate and disperse an inflow hydrograph as it routed
downstream through a series of conduits while kinematic wave solutions
show no attenuation, no dispersion, and some distortion of the
hydrograph shape. This behavior is depicted in Figure 2-2 from Miller
(1984) which shows the results of routing an inflow hydrograph down a
100-foot wide rectangular channel of 1% slope with a Manning's *n* of
0.06.

| Feature | Dynamic Wave | Kinematic Wave |
|---|---|---|
| Network topology | branched and looped | branched only |
| Flow splits | yes | with flow divider nodes |
| Adverse slopes | yes | no |
| Invert offsets | yes | ignored |
| Pumping | yes | only from storage nodes |
| Weirs and orifices | yes | only from storage nodes |
| Ponded overflows | yes | yes |
| Lateral seepage | yes | yes |
| Evaporation | yes | yes |
| Minor losses | yes | no |
| Culvert analysis | yes | no |
| Hydrograph attenuation | yes | no |
| Backwater effects | yes | no |
| Surcharge / Pressurization | yes | no |
| Reverse flow | yes | no |
| Tidal effects | yes | no |

![KWvsDW.png](hydraulics/media/media/hydraulics-image10.png)

**Figure 2-2 Comparison of dynamic wave and kinematic wave solutions (from Miller, 1984)**

## 2.3 Boundary and Initial Conditions

### 2.3.1 Boundary Conditions

There are two types of boundary conditions that a user must supply to a
SWMM conveyance network model:

1.  the hydraulic head to be maintained at each outfall node of the
    network,

2.  the external inflow received by specific nodes of the network.

Both types of conditions can vary with time. Outfall node heads are only
required for dynamic wave analysis. The options available for specifying
their values were described in Section 2.1.2. External inflows can
originate from any of the following sources:

- subcatchment runoff

- groundwater discharges

- rainfall-dependent infiltration/inflow (RDII)

- user-defined values.

Time-dependent runoff, groundwater, and RDII inflows are normally
provided by SWMM's hydrology module (see Volume I). It automatically
links the computed flow from each of these sources at each time period
to their designated receiving node. (Each SWMM subcatchment object that
generates runoff is assigned a conveyance system node that receives this
runoff. See Figure 1-2.)

User-defined external inflows can be attached to any node of the
network. They are typically used to describe dry weather sewage flows in
sanitary sewer systems, base flows in natural stream channels, or
inflows in the absence of any hydrologic modeling. They are expressed in
the following general format:

Flow rate at time *t* = (baseline value) × (baseline pattern factor) + (scale factor) × (time series value at time *t*)

The baseline value is some constant. The baseline pattern is a
combination of repeating hourly, daily, and monthly multiplier factors
applied to the baseline value. The time series value is a time varying
value and the scale factor is a constant multiplier applied to each time
series value. Time series values can be specified at unequal intervals
of time with interpolation used to obtain values at intermediate times.

### 2.3.2 Initial Conditions

A set of initial conditions at time 0 for all node heads and link flows
in the conveyance network must be specified before a hydraulic analysis
can begin. The default is to set all these values to 0, with the user
having the option to specify initial heads at selected nodes and initial
flow rates in selected conduit links.

Any initial flow rate assigned to a conduit link is assumed to represent
a uniform steady flow. Therefore its flow depth can be set to the normal
depth determined by the Manning equation as described in Section 5.5.2.
From this depth an initial cross-section flow area for the conduit can
be found which is required for kinematic wave analysis.

For dynamic wave analysis, if a non-storage, non-outfall node has not
had an initial head assigned to it then it's initial head is set equal
to the average elevation of the initial flow depths in the conduits that
deliver flow into it.



