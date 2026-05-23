# Chapter 6: Pumps and Regulators

\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_\_

Pumps are used in stormwater and wastewater collection systems to lift
water to a higher elevation so that gravity flow at a reasonable
velocity can be maintained. They are also used to produce pressurized
flow within force mains. Regulators act like valves that restrict the
flow rate along a conduit or out of a storage unit. They can also serve
as diverters that split flow between different branches of a conveyance
system (e.g., between the interceptor and an overflow pipe in a combined
sewer system). Specific types of regulators include orifices, weirs, or
general outlets that differ in their geometry and relationship between
flow and head. This chapter describes how the flow rate through pumps
and regulators is computed for both the dynamic and kinematic wave
models.

## 6.1 Pumps

SWMM treats pumps as links that have a pre-defined relationship between
flow rate *Q* and head *H* or some suitable surrogate. This relationship
is defined by a user-supplied Pump Curve. Table 6-1 depicts the five
types of pump curves that SWMM recognizes. Although not a requirement, a
pump's inlet node would typically be a storage node that represents a
pump station's wet well. An exception would be an inline booster pump
placed inside a force main line under dynamic wave analysis. A sixth
type of pump, called an Ideal pump, does not use a pump curve but
instead has its flow rate equal the inflow rate into its inlet node. It
must be the only outflow link from its inlet node and is used mainly for
preliminary design.

A single point on a Type1 or Type2 curve would typically represent an
operating point for a constant flow positive displacement pump.
Additional points might represent flow rates at different pump speeds or
contributions from additional constant speed pumps running in parallel.
The Type3 curve represents the characteristic curve of a centrifugal
pump operating at some fixed speed, where there is a continuous range of
flows available depending on the head required. The Type4 curve could be
a positive displacement pump with continuous speed control or a
centrifugal pump that lifts water to a more or less fixed elevation so
that the required head depends only on the water level at its inlet
node. A Type5 pump is a variable speed version of the Type3 pump. As the
pump's impeller speed varies relative to some nominal value, flow
changes in direct proportion while head changes in proportion to speed
squared.

Whenever a pump link is encountered in either the dynamic wave or
kinematic wave methods its new flow is found directly from its pump
curve using whatever values were last computed for nodal heads and
volumes.

**Table 6‑1 Pump curves recognized by SWMM**

| **Type** | **Description** | **Image** |
|---|---|---|
| **Type1** | Consists of a series of constant flow rates that apply over a corresponding series of volume intervals at the pump's inlet node. | ![pump1.bmp](VolumeII/media/media/image40.png) |
| **Type2** | Similar to a Type1 pump except that the fixed flow rate levels vary over a set of depth intervals at the pump's inlet node. | ![pump2.bmp](VolumeII/media/media/image41.png) |
| **Type3** | A centrifugal pump characteristic curve at some nominal impeller speed represented in a piecewise linear fashion. Flow is a function of the head difference between the inlet and outlet nodes. | ![pump3.bmp](VolumeII/media/media/image42.png) |
| **Type4** | A variable speed in-line pump where flow varies continuously with inlet node depth. | ![pump4.bmp](VolumeII/media/media/image43.png) |
| **Type5** | A variable speed version of the Type3 pump where the head v. flow curve shifts position depending on the pump's speed setting. | ![Pump5.bmp](VolumeII/media/media/image44.png) |

For Type1 and Type 2 curves, the curve is searched in step-wise fashion
for the first point whose volume or depth exceeds the volume or depth at
the pump's inlet node. The pump's flow is the flow associated with that
point. For the Type3 and Type5 curves, the flow is determined by first
finding the pair of adjacent data points that bracket the difference in
head between the pump's outlet and inlet nodes and then interpolating a
flow between these points for the given head difference. A similar
lookup procedure is used for the Type4 curve except that water level at
the pump's inlet node is used instead of head difference. A pump's flow
is not allowed to be outside the minimum and maximum values defined by
its pump curve and is not allowed to be negative.

The Type5 pump curve is shifted depending on what relative speed setting
*ω* the pump is currently operating under, where a setting of 1.0
applies to the original user-supplied curve. Following the pump affinity
laws (Sanks et al., 1998), a point with head *H* and flow *Q* on the
original curve becomes $\omega^{2}H$ and $\omega Q$, respectively on the
speed-adjusted curve. For the other pump types only the flow value found
from the original curve is multiplied by the speed setting.

Speed settings can be changed during a simulation by using control
rules. The setting can also be used to control pump operation based on
wet well level (e.g., set *ω* to 1 when the level is above a startup
depth and to 0 when below a shutoff depth). The adjusted pump flow is
checked to insure it does not cause the water level at the inlet node to
drop below 0 over the current time step. If the node is a storage node
then the pumping rate cannot exceed *Q*<sub>max</sub> where

$$Q_{\max} = Q_{in} + \frac{V_{N}}{\Delta t}$$                 
(6-1)
 
and *Q*<sub>in</sub> is the most recently computed total inflow to the node,
*V*<sub>N</sub> is the node volume at the start of the time step, and *∆t* is the
current time step interval. If the inlet node is not a storage node and
dynamic wave analysis is being made, Equation 3-15a is used with the
current pumping rate to estimate what the inlet node head at the end of
the time step would be. If this head is below the node's invert
elevation then the pumping rate is set equal to the node's current
inflow.

Some additional computational details regarding pumps are as follows:

1.  If the inlet node of a Type1 (flow v. volume) pump is not a storage
    node then it is assigned a virtual wet well whose volume varies
    linearly with depth up to the highest volume on the pump curve at
    full node depth. While the normal non-storage node methods are used
    to update the node's water level, the virtual wet well volume
    corresponding to the node's water level is used to determine the
    pumping rate. Equation 6-1 is also used to limit the pump flow to
    the maximum flow that the node can release.

2.  For dynamic wave modeling:

    a.  Pumps do not contribute any surface area to the node-link
        assemblies at their inlet and outlet nodes.

    b.  For Type3, Type4 and Type5 pump curves the
        $\frac{\partial Q}{\partial H}$ term used for evaluating a
        surcharged node is the negative of the slope of the line segment
        on which the pumping rate lies. For the other pump types it is
        zero since their line segments have zero slope.

    c.  No under-relaxation is applied to consecutive pump flows at Step
        3 of the iterative solution method described in Section 3.2.

3.  SWMM computes the power consumed in kilowatt-hours by each pump over
    each time step *∆t* as:

$$Kwh = 0.7457\left( H_{2} - H_{1} \right)\frac{Q\left( \frac{\Delta t}{3600} \right)}{8.814}$$   
(6-2)

where heads *H*<sub>1</sub> and *H*<sub>2</sub> are in feet, flow *Q* is in cfs, and
time step *∆t* is in seconds. The pump's wire to water efficiency is
not included in this calculation. The power consumption in each time
period is totaled up and reported for each pump in SWMM's Pumping
Summary Report. Also reported are the percent of time each pump is
online and operates at either the lower or upper end of its pump
curve.

## 6.2 Orifices

Orifices are regularly shaped, submerged openings through which flow is
proportional to the square root of the head across the opening. Orifices
are typically used to:

- regulate flow out of detention ponds and other storage facilities

- regulate flow through channels in the form of sluice gates

- divert excess flow from interceptor sewers to overflow structures

- model storm drain inlets.

### 6.2.1 Representation

SWMM represents an orifice as a link between two nodes. The opening can
be oriented either in a vertical plane for a side orifice or in a
horizontal plane for a bottom orifice (see Figure 6-1) and be elevated
some distance above the inlet node's invert. A riser pipe or inlet box
used as an outlet structure in a detention pond can be modeled as a
bottom orifice with a vertical offset. For kinematic wave analysis the
inlet node must be a storage node since this is the only type of node
for which a true hydraulic head is computed. For dynamic wave analysis
it can be any type of node.

<figure>
<img src="VolumeII/media/media/image45.png"
style="width:4.59318in;height:1.81227in" alt="Orifice2.png" />
<figcaption><p><span id="_Toc484694735"
class="anchor"></span><strong>Figure 6‑1 Orifice
orientations</strong></p></figcaption>
</figure>

The properties of an orifice link include:

- the height of its opening above the invert of its upstream node

- the shape of its opening which can be either circular or rectangular

- the dimensions of its opening (the diameter for a circular orifice or
  the height and width for a rectangular orifice)

- its discharge coefficient (described in more detail below)

- whether or not it contains a flap gate that prevents reverse flow.

The size of the orifice opening can be changed during a simulation by
having its setting adjusted using control rules. An orifice's setting is
the fraction of its full height that remains open (such as would occur
due to the action of lowering or raising a sluice gate above a side
orifice). In this case an additional optional parameter is the time it
takes to fully close a completely open (or fully open a completely
closed) orifice.

### 6.2.2 Flow Rate for Submerged Inlet

Whenever an orifice link is encountered in the dynamic wave or kinematic
wave solution procedure with its inlet side fully submerged its flow
rate *Q* (cfs) can be found using Torricelli's equation (Brater et al.,
1996):

$$Q = C_{d}A_{O}\sqrt{2gH_{e}}$$                            
(6-3)

where *C*<sub>d</sub> is a dimensionless orifice discharge coefficient, *A*<sub>O</sub> is
the area of the opening (ft<sup>2</sup>), and *He* is the effective head seen by
the orifice (ft). The following paragraphs describe how each of these
parameters is evaluated.

<u>Discharge Coefficient (*C*<sub>d</sub>)</u>

The most commonly cited value for *C*<sub>d</sub> is 0.6 while 0.4 is recommended
for ragged edge orifices (Federal Highway Administration, 2009). Brater
et al. (1996) review a number of experimental studies that show the
coefficient varying between 0.59 and 0.67 depending on orifice shape,
size, and effective head.

<u>Area of Opening (*A*<sub>O</sub>)</u>

The area of the orifice's opening depends on what its setting is. Let
*ω* be the orifice setting (between 0 and 1) in place at the end of the
previous routing time step and *ω\** be the target setting that was
established the last time that a control rule involving the orifice was
activated. If the time to close/open the orifice, *∆t~O\ ~*, is 0 then
for the current time step ω = *ω\**. Otherwise let *∆ω* be defined as
*ω\* - ω* and *ω* gets updated as follows:

| **Condition** | **Value** | **Equation** |
|---|---|---|
| if $\frac{\Delta t}{{\Delta t}_{O}} < \Delta\omega$ | $\omega + \frac{sgn(\Delta\omega)\Delta t}{{\Delta t}_{O}}$ | (6-4) |
| otherwise | $\omega^{*}$ | |

where *∆t* is the length of the current time step. With the setting
established, the area of the orifice opening is determined using the
methods described in Chapter 5 to find the area of either a circular or
rectangular cross section, depending on orifice shape, at a fraction *ω*
of its full height.

<u>Effective Head (*H*<sub>e</sub>)</u>

The effective head across the orifice depends on whether the water level
on its outflow side is below the orifice opening or not. Let *H*<sub>1</sub> be
the most recently computed head at the orifice's nominal upstream node
and *H*<sub>2</sub> be the same at the nominal downstream node. For kinematic
wave analysis, since the upstream node must be a storage node, *H*<sub>1</sub> is
the water surface elevation in the storage unit while *H*<sub>2</sub> is the
invert elevation of the downstream node. If $H_{1} < H_{2}$ and the
orifice does not have a flap gate then the head values are reversed (so
*H*<sub>1</sub> has the higher value) and the computed flow will be opposite to
the nominal downstream direction.

With *H*<sub>1</sub> and *H*<sub>2</sub> established the following rules are used to
determine the effective head across the orifice, where *Z*<sub>O</sub> is the
elevation of the bottom of the orifice opening and *Y*<sub>full</sub> is its full
height:

1.  For a side orifice:

| **Condition** | **Value** | **Equation** |
|---|---|---|
| for $H_{2} < Z_{O} + \omega\frac{Y_{full}}{2}$ | $H_{1} - \left( Z_{O} + \omega\frac{Y_{full}}{2} \right)$ | (6-5) |
| otherwise | $H_{1} - H_{2}$ | |

2.  For a bottom orifice:

| **Condition** | **Value** | **Equation** |
|---|---|---|
| for $H_{2} \leq Z_{O}$ | $H_{1} - Z_{O}$ | (6-6) |
| otherwise | $H_{1} - H_{2}$ | |

Figure 6-2 illustrates how *H*<sub>e</sub> is evaluated for a side orifice.

### 6.2.3 Flow Rate for Unsubmerged Inlet

When the water level at the inlet to a side orifice is below the top of
its opening the orifice behaves more like a weir and Equation 6-3 no
longer applies (see Figure 6-3). A similar situation occurs when the
head above a bottom orifice is below some threshold level. For these
cases SWMM determines what the threshold head for weir behavior is and
what the equivalent weir coefficient and crest length should be when
using the standard rectangular weir formula to compute the orifice's
flow rate. The details differ for side and bottom orifices as described
below.

   ![Orifice4.png](VolumeII/media/media/image46.png)   ![Orifice5.png](VolumeII/media/media/image47.png)
                                                                    
     Submerged Upstream Only                            Submerged Both Up and Downstream

**Figure 6‑2 Determination of effective head for an orifice**

<figure>
<img src="VolumeII/media/media/image48.png"
style="width:4.03288in;height:2.79833in" alt="Orifice6.png" />
<figcaption><p><span id="_Toc484694737"
class="anchor"></span><strong>Figure 6‑3 Orifice with unsubmerged
inlet</strong></p></figcaption>
</figure>

<u>Side Orifices</u>

For a side orifice, weir behavior occurs when the inlet water level is
below the top of the orifice opening. Thus the threshold head *H\** is:

$$H^{*} = Z_{O} + \omega Y_{full}$$                         
(6-7)

When the inlet head *H*<sub>1</sub> is below this height the flow through the
orifice can be approximated by using the general weir formula:

$$Q = C_{W}L\left( H_{1} - Z_{O} \right)^{1.5}$$                 
(6-8)

where *C*<sub>W</sub> is a weir coefficient (ft<sup>1/2</sup>/sec) and *L* is the crest
length of the equivalent weir (ft). Equating the flow from this equation
to that from the orifice equation 6-3 when $H_{1} = H^{*}$ and solving
for *C*<sub>W</sub>*L* results in:

$$C_{W}L = \frac{C_{d}A_{O}\sqrt{g}}{\omega Y_{full}}$$                 
(6-9)

Thus whenever the upstream head *H*<sub>1</sub> is below *H*\**, flow through the
side orifice can be found using the weir formula 6-8 with *C*<sub>W</sub>*L* given
by Equation 6-9.

<u>Bottom Orifices</u>

For a bottom orifice it is assumed that the threshold inlet head *H\**
for weir flow will be at a point where the flow through the orifice
using both the orifice and general weir equations will be the same. In
equation terms:

$$C_{d}A_{O}\sqrt{2g}\left( H^{*} - Z_{O} \right)^{0.5} = C_{W}L\left( H^{*} - Z_{O} \right)^{1.5}$$   
(6-10)

Solving for *H\** results in:

$$H^{*} = Z_{O} + \frac{C_{d}A_{O}\sqrt{2g}}{C_{W}L}$$      
(6-11)

In order to evaluate *H*\** values for *C*<sub>W</sub> and *L* must be assigned.
*C*<sub>W</sub> can be set to the commonly cited value of 3.33 ft<sup>0.5</sup>/sec used
for sharp crested weirs (Mays, 2001). *L* can be set to the
circumference of the opening as follows:

| **Opening Type** | **Value** | **Equation** |
|---|---|---|
| for a circular opening | $\pi\omega Y_{full}$ | (6-12) |
| for a rectangular opening | $2\left( b + \omega Y_{full} \right)$ | |

where *b* is the fixed width of the rectangular opening. Now *H*\** can
be determined for a given orifice coefficient and opening dimensions.
Whenever the upstream head *H*<sub>1</sub> is below *H*\**, flow through the
bottom orifice can be found using the general weir formula 6-8 with
*C*<sub>W</sub> = 3.33 and *L* given by Equation 6-12.

<u>Tailwater Submergence Correction</u>

As described later on in Section 6.3, whenever the downstream water
level is above a weir's crest the Villemonte equation is applied to
account for the effects of submergence (Brater et al., 1996). So when
the general weir equation 6-8 is used to compute orifice flow and the
downstream head *H*<sub>2</sub> is above the bottom of the orifice opening
*Z*<sub>O</sub>, the following submergence adjustment factor *f*<sub>S</sub> is applied to
the computed flow value:

$$f_{S} = \left\lbrack 1 - \left( \frac{H_{2}}{H_{1}} \right)^{1.5} \right\rbrack^{0.385}$$   
(6-13)

### 6.2.4 Flap Gate Head Loss Adjustment

When an orifice has a flap gate it adds a small amount of head loss for
flow through the gate. An empirical formula for this head loss was
derived from experiments performed at Iowa State University in the
1930's and published by Armco (1978):

$$\Delta H = \frac{4U^{2}}{g}\exp\left( - 1.15\frac{U}{\sqrt{H_{e}}} \right)$$   
(6-14)

where *∆H* is the head loss added by the flap gate (ft) and *U* is the
velocity through the orifice (ft/sec) which equals $\frac{Q}{A_{O}}$.
After the orifice's flow is first computed without this additional head
loss, *∆H* is computed with Equation 6-14 and subtracted from *H*<sub>e</sub>.
Then the flow is re-computed, this time using the adjusted value of
effective head.

### 6.2.5 Dynamic Wave Considerations

Dynamic wave modeling uses the surface area of the links attached to a
node to update the node's head when it is not in a surcharged state (see
Chapter 3). As an orifice has no length, its contribution to a node's
surface area should be zero. However in older versions of SWMM an
orifice was represented as an equivalent pipe that contributed surface
area to its end nodes just as a real conduit did. To maintain
compatibility with previous versions, SWMM 5 computes a surface area
*A*<sub>SL</sub> for an orifice as

| **Orifice Type** | **Value** | **Equation** |
|---|---|---|
| for a side orifice | $W(Y_{O})L_{O}$ | (6-15) |
| for a bottom orifice | $A\left( \omega Y_{full} \right)$ | |

where:

  *Y*<sub>O</sub>      =   depth of flow through the orifice (ft), equal to
                  $\min\left( H_{1} - Z_{O}\ ,\ \omega Y_{full} \right)$

  *L*<sub>O</sub>      =   equivalent conduit length of the orifice (ft), equal to 
$$\max\left( 2{\Delta t}_{\max}\sqrt{gY_{full}}\ ,200 \right)$$

  *∆t*<sub>max</sub>   =   maximum time step assigned by the user to the simulation (sec)

  *W(Y)*      =   width of orifice opening at flow depth *Y* (ft)

  *A(Y)*      =   area of orifice opening at flow depth *Y* (ft)

*W(Y)* and *A(Y)* are evaluated using the formulas from Chapter 5 for
either a circular or closed rectangular cross section shape. Half of
*A*<sub>SL</sub> is assigned to each of the orifice's end nodes providing that
the node is not a storage unit nor has its head below the orifice
opening.

Dynamic wave analysis also needs a value for the derivative of a link's
flow rate with respect to head (*dQ/dH*) when updating the head for a
surcharged node connected to the link (see section 3.3.5). For submerged
headwater orifices that use Equation 6-3 to compute flow rate *Q*, this
derivative is:

$$\frac{dQ}{dH} = 0.5\frac{Q}{H_{e}}$$                      
(6-16)

while for unsubmerged headwater orifices that use Equation 6-8 to
compute *Q* it is:

$$\frac{dQ}{dH} = 1.5\frac{Q}{\left( H_{1} - Z_{O} \right)}$$   
(6-17)

### 6.2.6 Summary of Orifice Computations

The computational steps used to compute flow through an orifice link can
be summarized as follows. At the start of a time step:

1.  If the orifice setting has not yet reached its target value or the
    target value has been changed by a control rule then update the
    setting using Equation 6-4.

2.  If the orifice setting has changed then compute the effective area
    of its opening *A*<sub>O</sub>. For side weirs use Equation 6-7 to compute
    its critical head *H*\** for weir behavior and Equation 6-9 to
    compute its equivalent weir constant *C*<sub>W</sub>*L*. For bottom weirs, use
    Equation 6-12 to find an equivalent weir length *L*, Equation 6-11
    to find the critical head *H*\**, and set the equivalent weir
    constant to 3.33*L*.

For each iteration within a time step that requires computing flow
through the orifice:

1.  Let *H*<sub>1</sub> denote the most recently computed head at the orifice's
    upstream node and *H*<sub>2</sub> be the same at the downstream node. (For
    kinematic wave analysis *H*<sub>2</sub> is the downstream node's invert
    elevation.)

2.  If *H*<sub>1</sub> \< *H*<sub>2</sub> reverse the values so that *H*<sub>1</sub> as the higher
    head and note that reverse flow will occur. If the orifice has a
    flap gate or *H*<sub>1</sub> is below the orifice opening then set its flow
    to 0.

3.  If the orifice is not submerged on its upstream side (*H*<sub>1</sub> \< *H*\**)
    then use Equation 6-8 to find its flow rate along with Equation 6-13
    to correct for any tailwater submergence. Otherwise use Equation 6-5
    (for side orifices) or 6-6 (for bottom orifices) to find the
    effective head *H*<sub>e</sub> on the orifice and then use Equation 6-3 to
    compute its flow rate.

4.  If the orifice has a flap gate then use Equation 6-14 to reduce its
    effective head and repeat the flow calculation of step 2.

5.  If the orifice has reverse flow then make the computed flow
    negative.

6.  Under dynamic wave analysis use Equation 6-15 to assign a surface
    area to the orifice and use Equation 6-16 (for side orifices) or
    6-17 (for bottom orifices) to compute $\frac{dQ}{dH}$ for the
    orifice.

## 6.3 Weirs

A transverse weir is a barrier with a cut-out placed across a conduit
perpendicular to the direction of flow. A side weir is a cut-out along
the side wall of a conduit parallel to the direction of flow. Flow
through a weir is proportional to the height of water above the weir's
crest raised to a power greater than one. Weirs are used for the same
types of reasons as orifices: to regulate flow out of storage
facilities, to regulate flow through channels, and to divert excess flow
from interceptor sewers to overflow structures. While orifices normally
operate with their inlet sides submerged, weirs normally maintain a free
surface above them.

### 6.3.1 Representation

SWMM represents a weir as a link between two nodes. For kinematic wave
analysis the inlet node must be a storage node since this is the only
type of node for which a true hydraulic head is computed. For dynamic
wave analysis it can be any type of node.

The properties of a weir link include:

- the height of its crest above the invert of its upstream node

- its orientation (transverse or side flow)

- the shape and dimensions of its opening

- the number of end contractions

- its effective weir coefficient

- whether or not it contains a flap gate that prevents reverse flow.

Figure 6-4 shows the different shapes of transverse weirs modeled by
SWMM. The only shape allowed for a side weir is rectangular.

<figure>
<img src="VolumeII/media/media/image49.png"
style="width:6.5in;height:3.18056in" alt="Weirs1.png" />
<figcaption><p><span id="_Toc484694738"
class="anchor"></span><strong>Figure 6‑4 Transverse weir
shapes</strong></p></figcaption>
</figure>

A suppressed rectangular weir has its opening extended across the entire
channel while a contracted weir does not. Weirs are also classified as
being sharp-crested or broad-crested. Sharp-crested weirs have a
relatively short crest thickness so that water springs clear of the
crest as it flows over the weir. The crest of a broad-crested weir is
thick enough so that the overflow remains in contact with the crest
surface.

The elevation of a weir's crest can be changed during a simulation by
having its setting adjusted using control rules. A weir's setting *ω* is
the fraction of its full height that remains open after it's crest is
moved up or down, as might occur with a downward opening weir gate or
inflatable dam. At a setting of 1 the weir's crest position is at its
lowest possible value and the full height of its opening is available
for flow. At a value of 0 the crest has been raised so that no opening
height remains and no flow can pass through the weir. At intermediate
settings the crest elevation equals its lowest possible value plus 1 -
*ω* times its full opening height.

### 6.3.2 Transverse Weirs

<u>General Equations</u>

The general equation for free flow over a transverse rectangular weir is
(Brater et al., 1996):

$$Q = C_{W}L_{e}H_{e}^{3/2}$$                               
(6-18)

and for a triangular weir is (Brater et al., 1996):

$$Q = C_{W}\tan\left( \frac{\theta}{2} \right)H_{e}^{5/2}$$   
(6-19)

In these equations *Q* is the flow rate (cfs), *L*<sub>e</sub> is the effective
crest length (ft), *θ* is the slot angle of a triangular weir, *H*<sub>e</sub> is
the effective head seen by the inflow side of the weir (ft), and *C*<sub>W</sub>*
is a weir coefficient (ft<sup>1/2</sup>/sec). A trapezoidal weir can be treated
as a combination of a rectangular weir and two half-triangular weirs
(Featherstone and Nalluri, 1982) leading to the equations:

$$Q = Q_{R} + Q_{T}$$                                     
(6-20a)

$$Q_{R} = C_{WR}L_{e}H_{e}^{3/2}$$                        
(6-20b)

$$Q_{T} = C_{WT}sH_{e}^{5/2}$$                            
(6-20c)

where *s* is the slope (run / rise) of the trapezoidal side wall and
*C*<sub>WR</sub> and *C*<sub>WT</sub> are the coefficients that apply to the rectangular
and triangular portions of the weir, respectively.

<u>Effective Head</u> (*H*<sub>e</sub>)

The effective head seen by a weir, accounting for its current setting,
is:

$$H_{e} = H_{1} - \left( Z_{W} + (1 - \omega)Y_{full} \right)$$   
(6-21)

where *H*<sub>1</sub> is the higher of the heads at the weir's end nodes, *Z*<sub>W</sub>*
is the elevation of the weir's crest when fully open (i.e., when *ω* =
1), and *Y*<sub>full</sub> is the full height of the weir's opening. If *H*<sub>1</sub>*
corresponds to the downstream node of the weir then reverse flow occurs
through the weir unless a flap gate is present in which case the flow is
0. Flow will also be 0 if $H_{e} \leq 0.$

<u>Effective Crest Length</u> (*L*<sub>e</sub>)

The effective crest length of a rectangular weir is reduced by the
number of end contractions as follows (Mays, 2001):

$$L_{e} = L - 0.1nH_{e}$$                                   
(6-22)

where *L* is the actual crest length and *n* = 1 if the weir is placed
away from one side wall, *n* = 2 if it is placed away from both side
walls and *n* = 0 if it occupies the entire width of the conduit (see
Figure 6-4).

When the setting *ω* for a triangular weir is less than 1 its opening
takes the shape of a trapezoidal weir. In this case the trapezoidal weir
equation 6-18 is used with both *C*<sub>WR</sub> and *C*<sub>WT</sub> set equal to the
weir's original coefficient, the side wall slope *s* set equal to
$\tan\left( \frac{\theta}{2} \right)$ and the effective length becomes:

$$L_{e} = 2s(1 - \omega)Y_{full}$$                          
(6-23)

This equation is also used for a trapezoidal weir whose setting is less
than 1.

<u>Weir Coefficient</u> (*C*<sub>W</sub>)

The standard weir coefficient *C*<sub>W</sub> for a sharp crested rectangular
weir is 3.33 ft<sup>1/2</sup>/sec (Mays, 2001). For
$\frac{H_{W}}{L} > \frac{1}{3}$ the coefficient has been found to vary
with effective head and weir sizing and placement (Bureau of
Reclamation, 2001). The Kindsvater-Carter method (Bureau of Reclamation,
2001) expresses this dependence with the following formula:

$$C_{W} = c1\left( \frac{H_{W}}{Z_{W}} \right) + c2$$       
(6-24)

where the constants *c1* and *c2* vary with the ratio of the crest
length *L* to the full width *b* of the cross section in which the weir
is placed as listed in Table 6-2.

Broad crested weir behavior is considered to occur when the ratio of the
water level above the crest to the crest thickness exceeds a certain
limit. Limits of 1 to 2 have been proposed by Brater et al. (1996), 15
by French (1985), and 2 to 20 by the Bureau of Reclamation (2001). Table
6-3 is a compilation of broad-crested weir coefficients synthesized by
Brater and King (1976) from several different experimental studies. It
shows the dependence of the coefficient on both head and breadth of
crest. Above a ratio of about 2 the weir behaves as sharp-crested with a
coefficient of 3.32. For ratios below 0.5 the coefficient approaches
2.63.

**Table 6‑2 Kindsvater-Carter constants for rectangular weir coefficient**

| **L/b** | **c1 (ft<sup>1/2</sup>/sec)** | **c2 (ft<sup>1/2</sup>/sec)** |
|---|---|---|
| 0.2 | -0.0087 | 3.152 |
| 0.4 | 0.0317 | 3.164 |
| 0.5 | 0.0612 | 3.173 |
| 0.6 | 0.0995 | 3.178 |
| 0.7 | 0.1602 | 3.182 |
| 0.8 | 0.2376 | 3.189 |
| 0.9 | 0.3447 | 3.205 |
| 1.0 | 0.4000 | 3.220 |

The standard value for the triangular weir coefficient *C*<sub>W</sub> is 2.5
ft<sup>1/2</sup>/sec (Mays, 2001). Figure 6-5 shows the variation of *C*<sub>W</sub> (in
ft<sup>1/2</sup>/sec ) with head over the weir *H*<sub>W</sub> (in feet) presented by
Brater and King (1976). The range of coefficients is rather small, from
2.5 up to 2.8.

> ![CWT.png](VolumeII/media/media/image50.png)

**Figure 6‑5 Coefficient for triangular weirs (from Brater and King, 1976)**

**Table 6‑3 Rectangular broad-crested weir coefficients (ft<sup>1/2</sup>/sec)**

| **Head (ft)** | **0.5** | **0.75** | **1.00** | **1.5** | **2.0** | **2.5** | **3.00** | **4.00** | **5.00** | **10.00** | **15.00** |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **0.2** | 2.80 | 2.75 | 2.69 | 2.62 | 2.54 | 2.48 | 2.44 | 2.38 | 2.34 | 2.49 | 2.68 |
| **0.4** | 2.92 | 2.80 | 2.72 | 2.64 | 2.61 | 2.60 | 2.58 | 2.54 | 2.50 | 2.56 | 2.70 |
| **0.6** | 3.08 | 2.89 | 2.75 | 2.64 | 2.61 | 2.60 | 2.68 | 2.69 | 2.70 | 2.70 | 2.70 |
| **0.8** | 3.30 | 3.04 | 2.85 | 2.68 | 2.60 | 2.60 | 2.67 | 2.68 | 2.68 | 2.69 | 2.64 |
| **1.0** | 3.32 | 3.14 | 2.98 | 2.75 | 2.66 | 2.64 | 2.65 | 2.67 | 2.68 | 2.68 | 2.63 |
| **1.2** | 3.32 | 3.20 | 3.08 | 2.86 | 2.70 | 2.65 | 2.64 | 2.67 | 2.66 | 2.69 | 2.64 |
| **1.4** | 3.32 | 3.26 | 3.20 | 2.92 | 2.77 | 2.68 | 2.64 | 2.65 | 2.65 | 2.67 | 2.64 |
| **1.6** | 3.32 | 3.29 | 3.28 | 3.07 | 2.89 | 2.75 | 2.68 | 2.66 | 2.65 | 2.64 | 2.63 |
| **1.8** | 3.32 | 3.31 | 3.31 | 3.07 | 2.88 | 2.74 | 2.68 | 2.66 | 2.65 | 2.64 | 2.63 |
| **2.0** | 3.32 | 3.30 | 3.30 | 3.03 | 2.85 | 2.76 | 2.72 | 2.68 | 2.65 | 2.64 | 2.63 |
| **2.5** | 3.32 | 3.31 | 3.31 | 3.28 | 3.07 | 2.89 | 2.81 | 2.72 | 2.67 | 2.64 | 2.63 |
| **3.0** | 3.32 | 3.32 | 3.32 | 3.32 | 3.20 | 3.05 | 2.92 | 2.73 | 2.66 | 2.64 | 2.63 |
| **3.5** | 3.32 | 3.32 | 3.32 | 3.32 | 3.32 | 3.19 | 2.97 | 2.76 | 2.68 | 2.64 | 2.63 |
| **4.0** | 3.32 | 3.32 | 3.32 | 3.32 | 3.32 | 3.32 | 3.07 | 2.79 | 2.70 | 2.64 | 2.63 |
| **4.5** | 3.32 | 3.32 | 3.32 | 3.32 | 3.32 | 3.32 | 3.32 | 2.88 | 2.74 | 2.64 | 2.63 |
| **5.0** | 3.32 | 3.32 | 3.32 | 3.32 | 3.32 | 3.32 | 3.32 | 3.07 | 2.79 | 2.64 | 2.63 |
| **5.5** | 3.32 | 3.32 | 3.32 | 3.32 | 3.32 | 3.32 | 3.32 | 3.32 | 2.88 | 2.64 | 2.63 |


### 6.3.3 Rectangular Side Weirs

Flow through a rectangular side weir is a case of spatially varied flow
with decreasing discharge and varying flow depth with distance along the
weir. Mays (2001) cites a number of different studies that have
developed discharge equations for side weirs where both the head and
weir coefficient vary spatially. Unfortunately these approaches are too
complex to implement in a program like SWMM. The empirical Engels
equation (Metcalf & Eddy, Inc. 1972) is used instead:

$$Q = C_{W}L_{e}^{0.83}H_{e}^{1.67}$$                       
(6-25)

Where flow *Q* is in cfs, length *L*<sub>e</sub> and head *H*<sub>e</sub> are in feet, and
*C*<sub>W</sub> is in ft<sup>1/2</sup>/sec. (It should be noted that previous versions of
SWMM used an incorrect form of this equation that had the exponent on
*L*<sub>e</sub> equal to 1.0)

Equation 6-25 applies to positive flow through the weir. For reverse
flow the standard rectangular weir equation 6-18 is used. *C*<sub>W</sub> was
assigned a value of 3.32 in the original Engels equation. Brunner (2014)
notes that side weir coefficients should be lower than the typical
values used for transverse weirs, and suggests a range of 1.5 to 2.6 for
weirs that model levees or roadways along natural channels.

### 6.3.4 Submerged Weir Flow 

As shown in Figure 6-6, submerged weir flow occurs when the water level
on the downstream side of the weir (*H*<sub>2</sub>) is above the crest elevation
(*Z*<sub>W</sub>). Under this condition weir flow is related not only to the head
on the upstream side of the weir (*H*<sub>1</sub>) but also to *H*<sub>2</sub> and *Z*<sub>W</sub>*
(Brater et al., 1996). These effects are commonly accounted for by
applying an adjustment factor *f*<sub>S</sub> developed by Villemonte (1947) to
the flow computed using the free flow equation:

$$f_{S} = \left\lbrack 1 - \left( \frac{H_{2}}{H_{1}} \right)^{n} \right\rbrack^{0.385}$$   
(6-26)

where *n* is the exponent on head used in the weir flow equation and the
heads *H*<sub>1</sub> and *H*<sub>2</sub> are in feet. For transverse rectangular weirs
(Equation 6-18) it is 3/2, for side weirs (Equation 6-25) it is 1.67,
and for triangular weirs (Equation 6-19) it is 5/2. For trapezoidal
weirs separate submergence factors are computed for the rectangular flow
portion (*Q*<sub>R</sub> in Equation 6-20b with *n* = 3/2) and for the triangular
flow portion (*Q*<sub>T</sub> in Equation 6-20c with *n* = 5/2).

<figure>
<img src="VolumeII/media/media/image51.png"
style="width:4.87568in;height:2.33366in" alt="Weirs2b.png" />
<figcaption><p><span id="_Toc484694740"
class="anchor"></span><strong>Figure 6‑6 Definitions of submerged and
surcharged weir flow</strong></p></figcaption>
</figure>

### 6.3.5 Surcharged Weir Flow

As shown in Figure 6-4, the weirs modeled by SWMM assume that the top of
the flow opening extends to the top of the structure that houses the
weir. If this structure is an open channel then the highest head that
the weir can see is ${\omega Y}_{full}$ where *ω* reflects the weir's
current setting. If the structure encloses the weir from above, such as
in a sewer pipe, then the head on the upstream side of the weir can
exceed the structure's crown elevation causing the weir to become
surcharged (see Figure 6-6). In this case the weir acts as an orifice
and its flow should be evaluated using the equivalent of Equation 6-3:

$$Q = C_{d}A_{O}\sqrt{2gH_{e}} = C_{O}\sqrt{H_{e}}$$                 
(6-27)

where *C*<sub>O</sub> is an equivalent orifice constant with units of
ft<sup>5/2</sup>/sec.

*C*<sub>O</sub> can be evaluated by setting Equation 6-27 equal to the
appropriate weir equation (6-18, 6-19, 6-20, or 6-25 depending on weir
type) evaluated at a weir head 
$$H_{e} = {\omega Y}_{full}$$ 
for which the corresponding orifice head would be 
$$\frac{{\omega Y}_{full}}{2}$$
. The result is:

$$C_{O} = \frac{Q_{W}\left( {\omega Y}_{full} \right)}{\sqrt{\frac{{\omega Y}_{full}}{2}}}$$                 
(6-28)

where 
$$Q_{W}\left( {\omega Y}_{full} \right)$$
is the flow in cfs from the relevant weir equation for a head of ${\omega Y}_{full}$ feet. The
constant *C*<sub>O</sub> is re-evaluated each time a weir's setting changes.

Thus if the user indicates that a weir is allowed to surcharge, then
whenever the upstream head *H*<sub>1</sub> is above *Z*<sub>W</sub> + *Y*<sub>full</sub> its flow is
computed using Equation 6-27. The head *H*<sub>e</sub> to be used in this
equation is computed as follows. Let *H\** be the head corresponding to
the elevation at half of the weir's opening height, i.e.:

$$H^{*} = Z_{W} + (1 - \omega)Y_{full} + \frac{\omega Y_{full}}{2}$$   
(6-29)

Then

| Condition | Value | Equation |
|---|---|---|
| for $H_{2} < H^{*}$ | $H_{1} - H^{*}$ | (6-30) |
| otherwise | $H_{1} - H_{2}$ | |

In addition, the correction for weir submergence is not applied.

### 6.3.6 Flap Gate Head Loss Adjustment

When a weir has a flap gate it adds a small amount of head loss for flow
through the gate. The same Armco formula used for orifices is used to
compute this head loss for weirs:

$$\Delta H = \frac{4U^{2}}{g}\exp\left( - 1.15\frac{U}{\sqrt{H_{e}}} \right)$$   
(6-31)

where *∆H* is the head loss added by the flap gate (ft) and *U* is the
velocity through the weir (ft/sec). To evaluate the velocity term in
this equation one needs to know the effective area *A*<sub>e</sub> of flow over
the weir. This area equals

| Condition | Value | Equation |
|---|---|---|
| for normal weir flow | $A\left( H_{W} + y_{C} \right) - A\left( y_{C} \right)$ | (6-32) |
| for surcharged weir flow | $A\left( Y_{full} \right) - A\left( y_{C} \right)$ | |

where *Y*<sub>full</sub> is the full height of the weir opening, *y*<sub>C</sub> is the
distance that the weir crest has been raised due to the current setting
(equal to $(1 - \omega)Y_{full}$ ), and *A(y)* is the area of the weir
opening at flow depth *y*. The latter quantity is found using the
geometry functions described in Chapter 5 for either a rectangular,
triangular, or trapezoidal shape. Knowing *A*<sub>e</sub>, $U = \frac{Q}{A_{e}}$,
where the flow *Q* has been determined using the methods described in
the previous sections.

After the orifice's flow is first computed without this additional head
loss, *∆H* is computed with Equation 6-31 and subtracted from *H*<sub>e</sub>.
Then the flow is re-computed, this time using the adjusted value of
effective head.

### 6.3.7 Dynamic Wave Considerations

A weir does not contribute any surface area to its end nodes under
dynamic wave modeling. The derivative of its flow rate with respect to
head *(dQ/dH)*, used when updating the head of a surcharged end node
(see section 3.3.5), is computed using the formulas in Table 6-4.

**Table 6‑4 Formulas for flow derivatives of various types of weirs**

| **Weir Type** | **Flow Derivative *(dQ/dH)*** |
|---|---|
| Transverse Rectangular | $1.5\frac{\vert Q \vert}{H_{e}}$ |
| Side Flow Rectangular: | |
| &nbsp;&nbsp;&nbsp;&nbsp;a. $Q \geq 0$ | $1.67\frac{\vert Q \vert}{H_{e}}$ |
| &nbsp;&nbsp;&nbsp;&nbsp;b. $Q < 0$ | $1.5\frac{\vert Q \vert}{H_{e}}$ |
| Transverse Triangular: | |
| &nbsp;&nbsp;&nbsp;&nbsp;a. Fully open (*ω* = 1) | $2.5\frac{\vert Q \vert}{H_{e}}$ |
| &nbsp;&nbsp;&nbsp;&nbsp;b. Partly open (*ω* < 1) | $1.5\frac{\vert Q_{R} \vert}{H_{e}} + 2.5\frac{\vert Q_{T} \vert}{H_{e}}$ |
| Transverse Trapezoidal | $1.5\frac{\vert Q_{R} \vert}{H_{e}} + 2.5\frac{\vert Q_{T} \vert}{H_{e}}$ |

Note: For trapezoidal openings, *Q*<sub>R</sub> is the flow through the central
rectangular portion and *Q*<sub>T</sub> is the flow through the triangular end
portions (see Equation 6-20).

### 6.3.8 Summary of Weir Computations

The computational steps used to compute flow through a weir link can be
summarized as follows. If the weir is allowed to surcharge and its
setting *ω* changes at the start of a time step then use Equation 6-28
to compute an equivalent orifice coefficient *C*<sub>O</sub> to use during
surcharge conditions. For each iteration within a time step that
requires computing flow through the weir:

1.  Let *H*<sub>1</sub> denote the most recently computed head at the weir's
    upstream node and *H*<sub>2</sub> be the same at the downstream node. (For
    kinematic wave analysis *H*<sub>2</sub> is the downstream node's invert
    elevation.)

2.  If *H*<sub>1</sub> \< *H*<sub>2</sub> reverse the values so that *H*<sub>1</sub> as the higher
    head and note that reverse flow will occur. If the weir has a flap
    gate or *H*<sub>1</sub> is below the weir crest then set its flow to 0.

3.  If the head *H*<sub>1</sub> is above the top of the weir's opening and the
    weir is allowed to surcharge then use the equivalent orifice
    equation 6-27 to find its flow where the effective head is found
    from Equations 6-29 and 6-30.

4.  Otherwise use Equation 6-21 to find the effective head on the weir
    and either Equation 6-18, 6-19, 6-20, or 6-25, depending on weir
    type, to find its flow rate.

5.  If the weir has a flap gate then use Equation 6-31 to adjust its
    effective head and repeat the flow calculation of steps 3 and 4.

6.  If the weir is not surcharged use Equation 6-26 to correct the flow
    for any tailwater submergence.

7.  If the weir has reverse flow then make the computed flow negative.

8.  Under dynamic wave analysis use the formulas in Table 6-4 to compute
    $\frac{dQ}{dH}$ for the weir.

## 6.4 Outlets

SWMM's outlet link is a generic type of flow regulator with a user
defined rating curve that relates flow rate to effective head. It can be
used in cases where the head-flow relationships that SWMM uses for
orifice or weir links do not apply. Some examples would be:

- a side orifice using the Smith and Coleman weir equation, where flow
  rate varies with head raised to the 1.645 power (Metcalf & Eddy, Inc.,
  1972),

- a perforated riser pipe with a grate on top used as a detention pond
  outlet structure,

- a vortex-type flow regulator (Hydro International, 2009; Faram et
  al., 2010) that provides more precise flow control than do standard
  orifices (see Figure 6-7).

For kinematic wave analysis the outlet's upstream node must be a storage
node since this is the only type of node for which a true hydraulic head
is computed. For dynamic wave analysis it can be any type of node.

The properties of an outlet link include:

- the height of its offset above the invert of its upstream node

- a rating curve that defines the relationship between head and the
  resulting flow rate

- whether head is defined by just the water level at the upstream node
  of the link or by the difference in head between its upstream and
  downstream nodes

- whether or not it contains a flap gate that prevents reverse flow.

An outlet link can also have a flow setting between 0 and 1 that can be
modified by control rules. The setting serves as a multiplier applied to
the flow determined from the outlet's rating curve.

The rating curve can be defined either as an analytical power law
function or a tabular listing of points on the curve. The analytical
power function has the form:

$$Q = aH_{e}^{b}$$                                          
(6-33)

where *Q* is flow rate (cfs), *H*<sub>e</sub> is the effective head (ft), and *a*
and *b* are user-supplied constants. The tabular rating curve consists
of pairs of head (*H*<sub>e</sub>) and flow (*Q*) values for points that the user
chooses to represent the shape of the outlet's rating curve.

<figure>
<img src="VolumeII/media/media/image52.png"
style="width:2.68793in;height:3.39631in" alt="hydra-brake4.png" />
<figcaption><p><span id="_Toc484694741"
class="anchor"></span><strong>Figure 6‑7 Rating curve for a vortex
device compared to an orifice</strong></p></figcaption>
</figure>

The following steps are used whenever the flow through an outlet link
must be computed:

1.  Let *H*<sub>1</sub> denote the most recently computed head at the outlet's
    upstream node and *H*<sub>2</sub> be the same at the downstream node. (For
    kinematic wave analysis *H*<sub>2</sub> is the downstream node's invert
    elevation.)

2.  If *H*<sub>1</sub> \< *H*<sub>2</sub> reverse the values so that *H*<sub>1</sub> has the higher
    head and note that reverse flow will occur. If the outlet has a flap
    gate or *H*<sub>1</sub> is below the outlet's offset elevation then set its
    flow to 0.

3.  For dynamic wave modeling, if the outlet's rating curve is based on
    head difference then compute an effective head on the outlet as
    $H_{e} = H_{1} - max\left( H_{2},\ \ \ Z_{O} \right)$ where *Z*<sub>O</sub>*
    is the outlet's offset elevation. Otherwise $H_{e} = H_{1} - Z_{O}$.

4.  For an analytical rating curve use Equation 6-33 to compute the
    outlet's flow rate *Q*. For a tabular rating curve find the adjacent
    head values in the table that bracket *H*<sub>e</sub> and use linear
    interpolation to find a corresponding flow rate *Q*. (If *H*<sub>e</sub> is
    below the first entry in the table then use the first entry's flow
    value. If it is above the last entry then use the last entry's flow
    value.)

5.  Multiply *Q* by whatever outlet setting is currently in effect and
    change its sign if reverse flow occurs.
