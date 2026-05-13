#  Chapter 7: Rainfall Dependent Inflow and Infiltration

## 7.1 Introduction

Rainfall dependent (or rainfall-derived) inflow and infiltration (RDII)
are stormwater flows that enter sanitary or combined sewers due to
\"inflow\" from direct connections of downspouts, sump pumps, foundation
drains, etc. as well as \"infiltration\" of subsurface water through
cracked pipes, leaky joints, poor manhole connections, etc. RDII can be
a significant cause of sanitary sewer overflows (SSOs) of untreated
wastewater into basements, streets and other properties, as well as
receiving streams. It can also cause significant flow increases to
wastewater treatment plants resulting in hydraulic overloading and
disruption of plant processes.

SWMM treats RDII as a separate category of external inflows that enters
the conveyance system at specific user-designated nodes. It is computed
independently of the surface runoff, infiltration, snowmelt and
groundwater processes described in previous chapters of this manual.
RDII flow is added onto the other inflow categories (such as dry weather
sanitary flow, overland runoff, and groundwater interflow) during each
time step of a simulation. RDII calculations were added to version 4 of
SWMM by C. Moore of CDM in 1993. This chapter describes how these RDII
flows are computed from the precipitation records supplied to a SWMM
data set.

## 7.2 Governing Equations

Figure 7-1 depicts the three major components of wet-weather wastewater
flow within a sanitary sewer system (Vallabhaneni et al., 2007). These
are base sanitary flow (BSF), groundwater infiltration (GWI), and RDII.
BSF is the flow discharged to sanitary sewers by homes, businesses,
institutions, and industrial water users throughout the normal course of
a day. It exhibits a typical diurnal pattern, with higher flows during
the morning and early evening hours and lower flows overnight. The
average daily BSF remains more or less constant during the week, but can
vary by both month and season.

GWI consists of groundwater that enters the collection system through
cracked pipes, pipe joints and manhole walls during extended periods of
time when water table levels are high, even in the absence of any
rainfall. It is different from RDII because it does not occur as a
direct response to a rainfall event. GWI varies throughout the year,
with the highest rates in late winter and spring as groundwater levels
rise, and the lowest rates (or no GWI at all) during late summer or
after an extended dry period.

RDII is the flow that can be directly attributed to a rainfall event.
This flow is zero before the start of the event, increases during the
event, and declines back to zero sometime after the event is over. The
start of the RDII response may be delayed during the time it takes for
surfaces to capture a portion of the initial rainfall and for soils to
become saturated. If the event is small enough, then no RDII at all may
be generated. The maximum volume of rainfall that does not produce any
RDII response is referred to as "initial abstraction" (Vallabhaneni et
al., 2007).

<figure>
<img src="VolumeI/media/media/Figure7-1.png"
style="width:6.42708in;height:4.4375in" alt="Flows" />
<figcaption><p><span id="_Toc426447708"
class="anchor"></span><strong>Figure 7-1 Components of wet-weather
wastewater flow.</strong></p></figcaption>
</figure>

Quantitative estimates of RDII are almost always derived from actual
wastewater flow records as opposed to attempting to model the
distributed set of small scale physical processes directly responsible
for RDII. Methods for modeling RDII are reviewed by Bennet et al. (1999)
and Lai (2008). SWMM uses the RTK unit hydrograph approach, which is
among the most flexible and widely used RDII methods (Vallabhaneni et
al., 2007). (The initials RTK stand for the three parameters that
characterize the unit hydrographs used by the method.)

The RTK unit hydrograph method was first developed by CDM-Smith
consultants in an RDII study for the East Bay Municipal Utility District
in Oakland, CA (Giguere and Riek, 1983). It represents the response of a
sewershed to a rainfall event through a series of up to three triangular
unit hydrographs. These unit hydrographs can be applied to any
particular storm event to produce a resulting time history of RDII flow
rates.

Figure 7-2 shows a single triangular unit hydrograph assumed to
represent the RDII flow induced by one unit of rainfall over a unit of
time. This unit hydrograph is characterized by the following parameters:

*R*: the fraction of rainfall volume that enters the sewer system and
equals the volume under the hydrograph

*T*: the time from the onset of rainfall to the peak of the unit
hydrograph

*K*: the ratio of time to recession of the unit hydrograph to the time
to peak

*Q<sub>peak</sub>*: peak flow (per unit area) on the unit hydrograph.

<figure>
<img src="VolumeI/media/media/Figure7-2.png"
style="width:6.5in;height:4.5in" alt="Example of an RDII triangular unit hydrograph" />
<figcaption><p><span id="_Toc426447709"
class="anchor"></span><strong>Figure 7-2 Example of an RDII triangular
unit hydrograph.</strong></p></figcaption>
</figure>

Figure 7-3 shows how this single unit hydrograph would be applied to a
storm that consists of three time periods of varying rainfall volume.
The original unit hydrograph is replicated for each rainfall time
period, with its origin offset by the time period and its ordinates
multiplied by the rainfall volume for that period. The overall response
to the storm is the hydrograph obtained by summing the ordinates of the
volume-adjusted hydrographs at each time point. The volumetric RDII
inflow into the conveyance system is the ordinate of the composite
hydrograph multiplied by the contributing area of the affected
sewershed. This process of adding together the rainfall-adjusted,
time-shifted hydrographs is known as convolution (Chow et al, 1988) and
is expressed mathematically as:

$$Q_{t} = \sum_{j = 1}^{t}{U_{t - j + 1}P_{j}}$$  (7-1)

where:

  ---------------------------------------------------------------------------
  *Q<sub>t</sub>*   =   RDII flow per unit area during time period *t*,
  -------- --- --------------------------------------------------------------
  *U<sub>t</sub>*   =   ordinate of the unit hydrograph for time period *t*,

  *P<sub>j</sub>*   =   depth of rainfall for time period *j*.
  ---------------------------------------------------------------------------

<figure>
<img src="VolumeI/media/media/Figure7-3.png"
style="width:6.5in;height:4.5in" alt="Application of a unit hydrograph to a storm event" />
<figcaption><p><span id="_Toc426447710"
class="anchor"></span><strong>Figure 7-3 Application of a unit hydrograph
to a storm event.</strong></p></figcaption>
</figure>

The ordinate value *U<sub>j</sub>* for time period *j* is determined from the
shape parameters *R, T*, and *K* of the unit hydrograph as follows. One
can write:

$$U_{j} = f_{j}Q_{peak}$$  (7-2)

where *f<sub>j</sub>* is the fraction of the rising limb (or falling limb) that
corresponds to time period *j*. Because the area under the unit
hydrograph is *R*, the value of *Q<sub>peak</sub>* is:

$$Q_{peak} = \frac{2R}{T + KT}$$  (7-3)

Thus *U<sub>j</sub>*can be expressed as:

$$U_{j} = \frac{2Rf_{j}}{T + KT}$$  (7-4)

By convention, the time *τ_j* on the unit hydrograph base corresponding
to time period *j* is taken as the midpoint between either ends of the
time interval:

$$\tau_{j} = (j - 0.5)\Delta\tau$$  (7-5)

where *Δτ* is the time interval over which precipitation is recorded.
The fraction *f<sub>j</sub>* is then determined as:

$$f_{j} = \frac{\tau_{j}}{T}$$  for *τ_j ≤ T*  (7-6)

$$f_{j} = 1 - \frac{\tau_{j - T}}{KT}$$  for *T* < *τ_j ≤ T + KT*  (7-7)

$$f_{j} = 0$$  for *τ_j > T + KT*  (7-8)

Because actual RDII hydrographs have complex shapes, three different
hydrographs of increasing durations are typically used to represent the
overall RDII unit response (Vallabhaneni et al., 2007). The first
hydrograph models the most rapidly responding inflow component usually
caused by direct sources of inflow, and has a time to peak *T* of one to
three hours. The second includes both rainfall-derived inflow and
infiltration, and has a longer *T* value. The third represents
infiltration that may continue long after the storm event has ended and
has the longest *T* value. Figure 7-4 depicts how the three unit
hydrographs are summed together to produce a total RDII hydrograph in
response to a unit of rainfall over one unit of time. Equation 7-1 is
still used to compute the overall RDII hydrograph to any given storm
event, with a separate *Q<sub>t</sub>* computed for each of the three unit
hydrographs. These are then added together to produce the total flow per
unit area for time period *t*.

![RDII%20Hydrographs](VolumeI/media/media/image49.png)

**Figure 7-4 Use of three unit hydrographs to represent RDII (Vallabhaneni et al., 2007).**

Not all storms will result in measurable inflow/infiltration. Just as
with ordinary runoff, a certain initial volume of rainfall will be
captured by surface ponding, interception by flat roofs and vegetation,
and surface wetting and will not contribute to RDII. This phenomenon is
represented in SWMM by three user-supplied "initial abstraction" (*IA*)
parameters that accompany each RDII unit hydrograph. *IA<sub>max</sub>* (in or
mm) is the maximum depth of initial abstraction capacity available for
the sewershed. *IA<sub>0</sub>* (in or mm) is the amount of that capacity already
used up at the start of the simulation. *IA<sub>r</sub>* (in/day or mm/day) is
the rate at which capacity becomes available again during periods of no
rainfall. During storm events, the volume of rainfall applied to the
unit hydrograph convolution formula, Equation 7-1, is reduced by the
amount of initial abstraction capacity remaining. During dry periods,
this capacity is regenerated based on the user-supplied recovery rate.

## 7.3 Computational Scheme

SWMM generates RDII inflows for specific nodes of a sewer system. Recall
from Section 1.2 that SWMM uses a network of links and nodes to
represent the conveyance portion of a drainage area. For RDII
applications this network would be the sewer system (either sanitary or
combined), the links are the sewer pipes and the nodes are points where
pipes connect to one another (e.g., manholes or pipe fittings).

It should be noted once again that RDII is computed independently from
any surface runoff or groundwater flow generated from the subcatchments
contained in a SWMM model. The sewershed that produces RDII flow for a
specific sewer system node is not represented explicitly in SWMM and
need not correspond to any of the runoff subcatchments defined for the
study area. In fact it is perfectly acceptable (and quite common for
sanitary sewer systems) to conduct an RDII analysis without including
any subcatchments in the model. In this case the model would consist of
a set of Rain Gage objects (and their data sources), the node and link
objects that make up the sewer network and sets of user-supplied time
series that describe groundwater (GWI) and sanitary (BSF) flows.

SWMM computes all RDII inflow time series prior to the start of a
simulation and saves these inflow values to an interface file. Each line
of the file contains, in chronological order, a node ID name, a date, a
time of day, and the RDII inflow value for that node. Dates with no RDII
inflows are not recorded. To compute the entries of this file the
following quantities are assumed known for each node of the conveyance
system node that receives RDII inflows:

- the area (*A*) of the sewershed that contributes RDII to the node,

- the *R-T-K* parameters for each of three RDII unit hydrographs,

- the initial abstraction parameters (*IA<sub>max</sub>, IA<sub>0</sub>*, and *IA<sub>r</sub>*)
  associated with each RDII unit hydrograph,

- the time series of rain volumes that fall on the sewershed and their
  recording interval *Δτ* (sec) as provided by a SWMM Rain Gage object.

> The steps used to process a precipitation record against a set of unit
> hydrographs to produce a record of RDII inflows for a specific
> conveyance node are described in the sidebar shown below.

## 7.4 Parameter Estimates

To use SWMM's RDII option a user must supply estimates of the three
parameters (R, T, and K) that define each of three unit hydrographs for
each node where RDII enters the sewer system. Each unit hydrograph can
also have a set of initial abstraction parameters (Ia_0, Ia_max, and
Ia_r). SWMM also allows one to specify different sets of unit
hydrographs and initial abstraction parameters for different months of
the year. In addition, the area of the RDII contributing sewershed must
also be specified.

R-T-K parameters are derived from site-specific flow monitoring data.
There are no general values that can be applied in the absence of actual
field data. All of these parameters require that a continuous flow
monitoring program be implemented at strategic points in the sewer
system. As described in Vallabhaneni et al., 2007, estimating the RDII
unit hydrograph parameters for a sewershed involves the following
activities:

1.  Identify the sewershed areas that are tributary to the flow monitor
    (see Figure 7-5).

2.  Extract the RDII portion of the recorded flow at the monitoring
    station during a wet weather event (see Figure 7-6).

3.  Estimate the R-T-K values for each of three unit hydrographs whose
    resultant hydrograph best matches the RDII flow extracted from the
    flow record (see Figure 7-7).

<figure>
<img src="VolumeI/media/media/image50.png"
style="width:6.31428in;height:6.09655in" alt="RDII_Sewersheds" />
<figcaption><p><span id="_Toc426447712"
class="anchor"></span><strong>Figure 7-5 Sewershed delineation
(Vallabhaneni et al., 2007).</strong></p></figcaption>
</figure>

<figure>
<img src="VolumeI/media/media/image51.png"
style="width:6.20833in;height:3.60741in" alt="RDII_Flow_History" />
<figcaption><p><span id="_Toc426447713"
class="anchor"></span><strong>Figure 7-6 Extracting RDII flow from a
continuous flow monitor (Vallabhaneni et al.,
2007).</strong></p></figcaption>
</figure>

![RDII_UH_Estimate](VolumeI/media/media/image52.png)

**Figure 7-7 Fitting unit hydrographs to an RDII flow record (Vallabhaneni et al., 2007).**

## 7.5 Numerical Example

A simple example illustrates how SWMM constructs an RDII interface file
for use within a hydraulic simulation. Assume there is a single rain
gage whose rainfall time series is shown in Table 7-1. Note that the
recording interval is 1 hour, and that there are two events separated by
22 hours. SWMM will use data from this gage to construct a time series
of RDII flows for a node named N1 in the conveyance system that services
an area of 10 acres. There is a single group of 3 unit hydrographs used
to derive RDII from the rain gage data. The shapes and parameters of the
unit hydrographs (UH1, UH2, and UH3) are shown in Figure 7-8. Note that
the R-values of this set of unit hydrographs sum to 0.36, implying that
36 percent of total rainfall volume winds up as RDII. To keep things
simple, initial abstraction is not considered in this example.

**Table 7-1 Rainfall time series for the illustrative RDII example**

| Hour | Rainfall (inches) |
|------|-------------------|
| 0:00 | 0.0 |
| 1:00 | 0.25 |
| 2:00 | 0.5 |
| 3:00 | 0.8 |
| 4:00 | 0.4 |
| 5:00 | 0.1 |
| 6:00 | 0.0 |
| 27:00 | 0.0 |
| 28:00 | 0.4 |
| 29:00 | 0.2 |
| 30:00 | 0.0 |


<figure>
<img src="VolumeI/media/media/Figure7-8.png"
style="width:6.510416666666667in;height:3.5in" alt="Unit hydrographs used for the illustrative RDII example" />
<figcaption><p><span id="_Toc426447715"
class="anchor"></span><strong>Figure 7-8 Unit hydrographs used for the
illustrative RDII example.</strong></p></figcaption>
</figure>

The resulting RDII flows are depicted in Figure 7-9. SWMM places these
flows into an RDII interface file, a portion of which is displayed in
Figure 7-10. This file is accessed during the flow routing portion of a
SWMM run to add RDII inflow into node N1 at each time step of the
routing process.

<figure>
<img src="VolumeI/media/media/Figure7-9.png"
style="width:5.822916666666667in;height:3.5in" alt="Time series of RDII flows for the illustrative RDII example" />
<figcaption><p><span id="_Toc426447716"
class="anchor"></span><strong>Figure 7-9 Time series of RDII flows for
the illustrative RDII example.</strong></p></figcaption>
</figure>

**SWMM5 Interface File**

```
900 - reporting time step in sec

1 - number of constituents as listed below:

FLOW CFS

1 - number of nodes as listed below:

N1

Node Year Mon Day Hr Min Sec FLOW
```

| Node | Year | Mon | Day | Hr | Min | Sec | FLOW |
|------|------|-----|-----|----|----|-----|------|
| N1 | 2002 | 02 | 02 | 01 | 15 | 00 | 0.204195 |
| N1 | 2002 | 02 | 02 | 01 | 30 | 00 | 0.204195 |
| N1 | 2002 | 02 | 02 | 01 | 45 | 00 | 0.204195 |
| N1 | 2002 | 02 | 02 | 02 | 00 | 00 | 0.204195 |
| N1 | 2002 | 02 | 02 | 02 | 15 | 00 | 0.554604 |
| N1 | 2002 | 02 | 02 | 02 | 30 | 00 | 0.554604 |
| N1 | 2002 | 02 | 02 | 02 | 45 | 00 | 0.554604 |
| N1 | 2002 | 02 | 02 | 03 | 00 | 00 | 0.554604 |
| N1 | 2002 | 02 | 02 | 03 | 15 | 00 | 1.021479 |
| N1 | 2002 | 02 | 02 | 03 | 30 | 00 | 1.021479 |
| N1 | 2002 | 02 | 02 | 03 | 45 | 00 | 1.021479 |
| N1 | 2002 | 02 | 02 | 04 | 00 | 00 | 1.021479 |
| N1 | 2002 | 02 | 02 | 04 | 15 | 00 | 1.001312 |
| N1 | 2002 | 02 | 02 | 04 | 30 | 00 | 1.001312 |
| N1 | 2002 | 02 | 02 | 04 | 45 | 00 | 1.001312 |
| N1 | 2002 | 02 | 02 | 05 | 00 | 00 | 1.001312 |
| N1 | 2002 | 02 | 02 | 05 | 15 | 00 | 0.703842 |
| N1 | 2002 | 02 | 02 | 05 | 30 | 00 | 0.703842 |
| N1 | 2002 | 02 | 02 | 05 | 45 | 00 | 0.703842 |
| N1 | 2002 | 02 | 02 | 06 | 00 | 00 | 0.703842 |

**Figure 7-10 Excerpt from the RDII interface file for the illustrative RDII example.**

