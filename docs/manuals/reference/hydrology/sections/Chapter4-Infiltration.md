#  Chapter 4: Infiltration

## 4.1 Introduction

Infiltration is the process by which rainfall penetrates the ground
surface and fills the pores of the underlying soil (Akan and Houghtalen,
2003). It often accounts for the largest portion of rainfall losses over
pervious areas. Theoretically, infiltration is governed by the Richards
equation (Richards, 1931) which requires that the relationship between
soil permeability and pore water tension as a function of soil moisture
content be known. The difficulty in solving this highly nonlinear
partial differential equation makes it unsuitable for use in a general
purpose model like SWMM, especially for long-term continuous
simulations. Engineers have developed several simpler algebraic
infiltration models that capture the general dependence of infiltration
capacity on soil characteristics and the volume of previously
infiltrated water during the course of a storm event. Because there is
no universal agreement as to which model is best, SWMM allows the user
to choose from among four of the most widely used methods: Horton's
method, a modified Horton method, the Green-Ampt method, and the Curve
Number method.

No matter which infiltration method is used, the parameters that define
the method are highly dependent on the type and condition of the soil
being infiltrated. The NRCS (Natural Resources Conservation Service,
formerly the Soil Conservation Service or SCS) has classified most soils
into Hydrologic Soil Groups, A, B, C, and D, depending on their limiting
infiltration capacities. Well drained, sandy soils are "A"; poorly
drained, clayey soils are "D," as described in Table 4-1. Every soil in
the United States has an A-D classification, or sometimes a dual
classification, such as B/D, meaning drained (artificially) and
undrained (natural) condition.

The group assigned to specific types of soils and locations can be found
by consulting:

- the Natural Resources Conservation Service's (NRCS) Field Office
  Technical Guide

- the NRCS Soil Data Access Web site:
  <http://sdmdataaccess.nrcs.usda.gov/>

- the Web Soil Survey Web site: <http://websoilsurvey.nrcs.usda.gov/>.

Additional soil characterization (physics and chemical) data are
available at the aforementioned web sites.

**Table 4-1 Hydrologic soil group meanings (NRCS, 2009, Chapter 7)**

| Group | Meaning |
|-------|---------|
| A | Low runoff potential. Soils having high infiltration rates even when thoroughly wetted and consisting chiefly of deep, well to excessively drained sands or gravels. |
| B | Soils having moderate infiltration rates when thoroughly wetted and consisting chiefly of moderately deep to deep, moderately well to well-drained soils with moderately fine to moderately coarse textures. E.g., shallow loess, sandy loam. |
| C | Soils having slow infiltration rates when thoroughly wetted and consisting chiefly of soils with a layer that impedes downward movement of water, or soils with moderately fine to fine textures. E.g., clay loams, shallow sandy loam. |
| D | High runoff potential. Soils having very slow infiltration rates when thoroughly wetted and consisting chiefly of clay soils with a high swelling potential, soils with a permanent high water table, soils with a clay-pan or clay layer at or near the surface, and shallow soils over nearly impervious material. |

The best source of information about a particular soil type is the Soil
Survey Interpretation, available from a local NRCS office in the U.S.
Data for soils in each county are often summarized in a county soil
survey document; the latter is often available in a local Soil and Water
Conservation District. Because printed versions of these documents are
increasingly difficult to obtain, on-line access is more likely
(<http://soils.usda.gov/survey/>). Of particular interest is the
"Physical Properties" report that includes parameters of interest
regarding infiltration. This report may be downloaded for any soil, as
illustrated in Figure 4-1. These data include saturated hydraulic
conductivity, for instance. Other potentially useful reports include:

- Water Features, including information such as hydrologic soil group (B
  for the Woodburn Silt Loam), water table depth, and ponding frequency.

- RUSLE2 Related Attributes, with data for application of the Universal
  Soil Loss Equation.

- Engineering Properties, including soil horizon depths, soil
  classifications (USDA, Unified, AASHTO), sieve analysis, liquid limit,
  and plasticity index.

In short, the NRCS provides an invaluable resource for information on
soils and drainage of soils. The agency's data are ever more valuable as
they increasingly reside on-line on the Web.

![NRCS Table](VolumeI/media/media/image15.png)

**Figure 4-1 Physical properties for Woodburn silt loam, Benton County, Oregon.**

## 4.2 Horton's Method

Horton's method is empirical in nature and is perhaps the best known of
the infiltration equations. Many hydrologists have a "feel" for the best
values of its three parameters despite the fact that little published
information is available. In its usual form it is applicable only to
events for which the rainfall intensity always exceeds the infiltration
capacity; however, the modified form used in SWMM is intended to
overcome this limitation. The Horton method has been a part of SWMM
since the program was first released (Metcalf and Eddy et al., 1971a).

### 4.2.1 Governing Equations

Horton (1933, 1940) proposed the following exponential equation to
predict the reduction in infiltration capacity over time as observed
from field measurements:

$$f_{p} = f_{\infty} + \left( f_{0} - f_{\infty} \right)e^{- k_{d}t}$$ (4-1) 

where:

  *f*<sub>p</sub>   =   infiltration capacity into soil (ft/sec)
  
  *f*<sub>∞</sub>   =   minimum or equilibrium value of *f*<sub>p</sub> (at t = âˆž) (ft/sec)

  *f*<sub>0</sub>   =   maximum or initial value of *f*<sub>p</sub> (at t = 0) (ft/sec)

  *t*      =   time from beginning of storm (sec)

  *k*<sub>d</sub>   =   decay coefficient (sec<sup>-1</sup>).
  

Equation 4-1 is sketched in Figure 4-2 and can be derived theoretically
from the Richards equation under the proper set of assumptions
(Eagleson, 1970). Note that actual infiltration will be the lesser of
actual rainfall and infiltration capacity:

$$f(t) = min\left\lbrack f_{p}(t),\ i(t) \right\rbrack$$ (4-2) 

where:

  --------------------------------------------------------------------------
  *f*   =   actual infiltration into the soil (ft/sec)
  ----- --- ----------------------------------------------------------------
  *i*   =   rainfall intensity (ft/sec).

  --------------------------------------------------------------------------

  : Thus for the case illustrated in Figure 4-2 runoff would be
  intermittent.

![Horton infiltration curve](VolumeI/media/media/figure4-2.png)

**Figure 4-2 The Horton infiltration curve**

Typical values for parameters *f*<sub>o</sub> and *f*<sub>∞</sub> are usually greater than
typical rainfall intensities. Thus, when Equation 4-1 is used such that
*f*<sub>p</sub> is a function of time only, the exponential term will cause
*f*<sub>p</sub> to decrease even if rainfall intensities are very light, as
sketched in Figure 4-2. This results in a reduction in infiltration
capacity regardless of the actual amount of entry of water into the
soil.

To correct this problem, the integrated form of Horton's equation 4-1 is
used in SWMM:

$$F\left( t_{p} \right) = \int_{0}^{t_{p}}{f_{p}dt = f_{\infty}t_{p} + \frac{\left( f_{0} - f_{\infty} \right)}{k_{d}}\left( 1 - e^{- k_{d}t_{p}} \right)}$$ (4-3)

where *F* is the cumulative infiltration capacity at time *t*<sub>p</sub> in
feet. This function is plotted in Figure 4-3 where it is assumed that
actual infiltration has been equal to *f*<sub>p</sub> over all time *t*. As noted
before, there will in fact be times when infiltration *f* is less than
*f*<sub>p</sub>, so that the true cumulative infiltration will be:

$$F(t) = \int_{0}^{t}{\min\left\lbrack f_{p},\ i \right\rbrack d\tau}$$ (4-4) 

![Cumulative infiltration F as the area under the Horton curve](VolumeI/media/media/figure4-3.png)

**Figure 4-3 Cumulative infiltration F as the area under the Horton curve**

Equations 4-3 and 4-4 can thus be used to define the time *t*<sub>p</sub> along
the Horton curve at which the next value of *f*<sub>p</sub> can be found. That
is, *F* is updated with the actual infiltration *f* over the current
time step and then the following equation, with *t*<sub>p</sub> as the only
unknown, is solved:

$$F = f_{\infty}t_{p} + \frac{\left( f_{0} - f_{\infty} \right)}{k_{d}}\left( 1 - e^{- k_{d}t_{p}} \right)$$ (4-5) 

Once the new *t*<sub>p</sub> is known, the infiltration capacity *f*<sub>p</sub> for the
next time step can be found from Equation 4-1.

An additional optional parameter *F*<sub>max</sub> can be specified that limits
the total volume of water that can infiltrate the soil. When cumulative
infiltration exceeds this value, saturation conditions exist, and no
more infiltration occurs; the land surface behaves as if it were
impermeable. Thus *F(t)* in Equation 4-4 is not allowed to exceed
*F*<sub>max</sub>.

### 4.2.2 Recovery of Infiltration Capacity

For simulations that consist of multiple storm events over a set period
of time, infiltration capacity will be regenerated (recovered) during
dry weather periods. With Horton's method, SWMM performs this function
whenever a subcatchment is dry -- meaning it receives no precipitation
and has no ponded surface water -- according to the hypothetical drying
curve sketched in Figure 4-4:

$$f_{p} = f_{0} - \left( f_{0} - f_{\infty} \right)e^{- k_{r}\left( t - t_{w} \right)}$$ (4-6) 

where:

  ----------------------------------------------------------------------------
  *k*<sub>r</sub>   =   decay coefficient for the recovery curve (sec<sup>-1</sup>)
  -------- --- ---------------------------------------------------------------
  *t*<sub>w</sub>   =   hypothetical projected time at which *f*<sub>p</sub> = *f*<sub>∞</sub> on the
               recovery curve (sec).

  ----------------------------------------------------------------------------

New values of *t*<sub>p</sub> are then generated as indicated in Figure 4-4 as
recovery proceeds. For example, let *t*<sub>pr</sub> be the *t*<sub>p</sub> value at which
recovery begins with *f*<sub>r</sub> as the corresponding infiltration capacity.
According to the recovery curve,

$$f_{r} = f_{0} - \left( f_{0} - f_{\infty} \right)e^{- k_{r}\left( t_{pr} - t_{w} \right)}$$ (4-7) 

one can compute *t*<sub>w</sub> as:

$$t_{w} = t_{pr} - \frac{1}{k_{r}}\ln\left( \frac{f_{0} - f_{\infty}}{f_{0} - f_{r}} \right)$$ (4-8) 

![Regeneration (recovery) of infiltration capacity during dry time steps](VolumeI/media/media/figure4-4.png)

**Figure 4-4 Regeneration (recovery) of infiltration capacity during dry time steps**

Then after a recovery time to *t*<sub>w1</sub> = *t*<sub>pr</sub> + Δ*t*, the new
infiltration capacity *f*<sub>1</sub> is found from:

$$f_{1} = f_{0} - \left( f_{0} - f_{\infty} \right)e^{- k_{r}\left( t_{w1} - t_{w} \right)}$$ (4-9) 

Finally, the new equivalent time *t*<sub>p1</sub> on the infiltration curve from
which the infiltration process would re-start under a wet condition is:

$$t_{p1} = \frac{1}{k_{d}}\ln\left( \frac{f_{0} - f_{\infty}}{f_{1} - f_{\infty}} \right)$$ (4-10) 

These steps can be combined into the following equation:

$$t_{p1} = \frac{1}{k_{d}}\ln\left\lbrack 1 - e^{- k_{r}\mathrm{\Delta}t}\left( 1 - e^{- k_{d}t_{pr}} \right) \right\rbrack$$ (4-11) 

On succeeding time steps, *t*<sub>p1</sub> may be substituted for *t*<sub>pr</sub>, and
*t*<sub>p2</sub> substituted for *t*<sub>p1</sub>, etc. Note that *f*<sub>p</sub> has reached its
maximum value of *f*<sub>0</sub> when *t*<sub>p</sub> = *0*.

Although this recovery method gives sensible results, it is somewhat
unsatisfactory inasmuch as there is no dependence of infiltration
recovery on evapotranspiration (ET). Drying of the soil through ET and
deep infiltration should influence the recovery of infiltration
capacity, but these mechanisms are replaced in SWMM by the more
empirical approach just discussed.

### 4.2.3 Computational Scheme

The detailed computational scheme for computing Horton infiltration for
each subcatchment within a study area over a single time step of a
simulation is presented in the sidebar below.

<div style="background-color: #f8f9fa; border: 1px solid #dee2e6; border-radius: 5px; padding: 15px; margin: 10px 0;">

**Computational Scheme for Horton Infiltration**

The following variables are assumed known at the start of each time step Δt (sec) for the pervious subarea of each subcatchment:

- **i** = rainfall rate (ft/sec)
- **d** = depth of ponded surface water (ft)
- **t<sub>p</sub>** = equivalent time on the Horton curve (sec)

as are the following constants:

- **f<sub>0</sub>** = maximum (or initial) infiltration capacity (ft/sec)
- **f<sub>∞</sub>** = minimum (or ultimate) infiltration capacity (ft/sec)
- **k<sub>d</sub>** = infiltration capacity decay coefficient (sec<sup>-1</sup>)
- **k<sub>r</sub>** = infiltration capacity recovery coefficient (sec<sup>-1</sup>)
- **F<sub>max</sub>** = maximum infiltration volume possible (ft)

Initially at time 0, t<sub>p</sub> = 0.

The computational steps for computing the Horton infiltration rate f for a given subcatchment over a single time step of a simulation proceed as follows:

1. **Compute the available rainfall rate:** i<sub>a</sub> = i + d/Δt.

2. **If i<sub>a</sub> = 0**, meaning the surface is dry, then update the current time on the Horton infiltration curve t<sub>p</sub> as follows:
   ```
   t_p ← 1/k_d ln[1-e^(-k_r Δt) (1-e^(-k_d t_p))]
   ```
   and set the infiltration rate f to 0.

3. **Otherwise** compute the cumulative infiltration volume from the integrated form of the Horton curve at times t<sub>p</sub> and t<sub>1</sub> = t<sub>p</sub> + Δt (F<sub>p</sub> and F<sub>1</sub>, respectively) as follows:
   - **If t<sub>p</sub> ≥ 16/k<sub>d</sub>** then t<sub>p</sub> is on the flat portion of the Horton curve so
     ```
     F_p = f_∞ t_p + (f_0-f_∞)/k_d  and  F_1 = F_p + f_0 Δt
     ```
   - **Otherwise,**
     ```
     F_p = f_∞ t_p + (f_0-f_∞)/k_d (1-e^(-k_d t_p))
     F_1 = f_∞ t_1 + (f_0-f_∞)/k_d (1-e^(-k_d t_1))
     ```
   Limit both F<sub>p</sub> and F<sub>1</sub> to not exceed F<sub>max</sub> if a value for the latter was supplied.

4. **Compute the average infiltration rate f<sub>p</sub>** over the time step: f<sub>p</sub> = (F<sub>1</sub> - F<sub>p</sub>)/Δt.

5. **If t<sub>1</sub> > 16/k<sub>d</sub> or f<sub>p</sub> < i<sub>a</sub>** then update t<sub>p</sub> to t<sub>p</sub> + Δt.

6. **Otherwise** solve the nonlinear equation
   ```
   F_p + f_p Δt = f_∞ t_p + (f_0-f_∞)/k_d (1-e^(-k_d t_p))
   ```
   for the updated value of t<sub>p</sub> using a Newton-Raphson algorithm (Press et al., 1992).

7. **Compute the actual infiltration rate f** as the lesser of f<sub>p</sub> and the available rainfall rate: f = min[f<sub>p</sub>, i<sub>a</sub>].

The Newton-Raphson algorithm used to solve the nonlinear equation at Step 6 is included as a callable subroutine in the SWMM computer code.

</div>

### 4.2.4 Parameter Estimates

The parameters that a user must supply for each subcatchment for the
Horton infiltration method are:

> *f*<sub>0</sub> - the maximum or initial infiltration capacity (in/hr or
> mm/hr),
>
> *f*<sub>∞</sub> - the minimum or equilibrium infiltration capacity (in/hr or
> mm/hr),
>
> *k*<sub>d</sub> - the decay coefficient (hr<sup>-1</sup>),
>
> *k*<sub>r</sub> - the regeneration coefficient (days<sup>-1</sup>), and, optionally,
>
> *F*<sub>max</sub> - the maximum infiltration volume (in or mm).

Conversions between the user-supplied units of these parameters (such as
in, mm or hr) and those used internally (ft and sec) are handled
automatically by the program.

Although the Horton equation is probably the best-known of the several
infiltration equations available, there is little to help the user
select values of parameters *f*<sub>0</sub> and *k*<sub>d</sub> for a particular
application. (Fortunately, some guidance can be found for the value of
*f*<sub>∞</sub>.). Since the ac­tual values of *f*<sub>0</sub> and *k*<sub>d</sub> (and often
*f*<sub>∞</sub>.) depend on the soil, vege­tation, and initial moisture content,
ideally these parameters should be estimated using re­sults from field
infiltrometer tests for a number of sites of the watershed and for a
number of ante­cedent wetness conditions. An example of Horton parameters
for Georgia soils is given in Table 4-2 (Rawls et al., 1976). Horton's
(1940) estimates are shown in Table 4-3. Skaggs and Khaleel (1982)
provide Horton-type decay curves on the basis of theoretical estimates.

**Table 4-2 Horton parameters for selected Georgia soils (Rawls et al., 1976)**

| Soil Type | f∞ (in/hr) | f₀ (in/hr) | kd (hr⁻¹) |
|-----------|------------|------------|-----------|
| Alpha loamy sand | 1.40 | 19.0 | 38.29 |
| Carnegie sandy loam | 1.77 | 14.77 | 19.64 |
| Cowarts loamy sand | 1.95 | 15.28 | 10.65 |
| Dothan loamy sand | 2.63 | 3.47 | 1.40 |
| Fuquay pebbly loamy sand | 2.42 | 6.24 | 4.70 |
| Leefield loamy sand | 1.73 | 11.34 | 7.70 |
| Robersdale loamy sand | 1.18 | 12.41 | 21.75 |
| Stilson loamy sand | 1.55 | 8.11 | 6.55 |
| Tooup sand | 1.80 | 23.01 | 32.71 |

**Table 4-3 Horton parameters provided by Horton (1940)**

| Soil and Cover | f∞ (in/hr) | f₀ (in/hr) | kd (hr⁻¹) |
|----------------|------------|------------|-----------|
| Standard agricultural (bare) | 0.24 -- 8.9 | 11.4 | 96 |
| Standard agricultural (turfed) | 8.2 -- 11.8 | 36.7 | 48 |
| Peat | 0.82 -- 11.8 | 13.3 | 108 |
| Fine sandy clay (bare) | 0.82 -- 1.0 | 8.6 | 120 |
| Fine sandy clay (turfed) | 4.1 -- 1.2 | 27.4 | 84 |

If it is not possible to use field data to find estimates of *f*<sub>0</sub>,
*f*<sub>∞</sub>, and *k*<sub>d</sub> for each subcatchment, the following guidelines should
be helpful. Often, NRCS data may be used directly. For instance, for the
two upper horizons (soil layers) of Woodburn silt loam (Figure 4-1),
saturated hydraulic conductivity is listed as 4 - 14 micrometers per
second, or 0.6 - 2.0 in/hr (14 - 50 mm/hr). Unfortunately, this wide
range in values is commonly encountered among soil survey data.
Fortunately, the range also serves as a reminder that infiltration rates
are notoriously variable in space as well as in time and should not be
considered "exact." Note that saturated hydraulic conductivity is the
more appropriate word for parameter *K*<sub>S</sub>, also termed "permeability"
on older soil survey interpretation tables.

**Minimum Infiltration Capacity (*f*<sub>∞</sub>)**

The Horton parameter *f*<sub>∞</sub> is essentially equal to saturated hydraulic
conductivity, *K*<sub>S</sub>, that is, *f*<sub>∞</sub> ≈ *K*<sub>S</sub>. The *f*<sub>∞</sub> value is also
the limiting infiltration rate when water is ponded on the surface, at
low depths. Generalized estimates for *K*<sub>S</sub> will also be discussed in
conjunction with the Green-Ampt infiltration method later in this
chapter and are the best source of values for *f*<sub>∞</sub> in the absence of
site-specific data.

Alternatively, values for *f*<sub>∞</sub> according to Musgrave (1955) are given
in Table 4-4. To help select a value within the range given for each
soil group, the user should consider the texture of the layer of least
hydraulic conductivity in the profile. Depending on whether that layer
is sand, loam, or clay, the *f*<sub>∞</sub> value should be chosen near the top,
middle, and bottom of the range respectively. For example, the data
sheet for Woodburn silt loam identifies it as being in Hydrologic Soil
Group B, which puts the estimate of *f*<sub>∞</sub> into the range of 0.15 -
0.30 in/hr (3.8 -7.6 mm/hr), much lower than the *K*<sub>S</sub> value dis­cussed
above. Examina­tion of the texture of the layers in the soil profile
indicates that they are silty in nature, sug­gesting that the estimate of
the *f*<sub>∞</sub> value should be in the low end of the range, say 0.15 - 0.20
in/hr (3.8 - 5.1 mm/hr). A sensitivity test on the *f*<sub>∞</sub> value will
indicate the importance of this parameter to the overall result; in
fact, *f*<sub>∞</sub> is usually the most sensitive of the three Horton curve
parameters.

**Table 4-4 Values of f∞ for Hydrologic Soil Groups (Musgrave, 1955)**

| Hydrologic Soil Group | f∞ (in/hr) |
|----------------------|------------|
| A | 0.45 - 0.30 |
| B | 0.30 - 0.15 |
| C | 0.15 - 0.05 |
| D | 0.05 - 0 |

Caution should be used in applying values from Table 4-4 to sandy soils
(group A) since reported *K*<sub>S</sub> values are often much higher. For
instance, sandy soils in Florida can have *K*<sub>S</sub> values from 7 to 18
in/hr (180 - 450 mm/hr) (Carlisle et al., 1981). Unless the water table
rises to the surface, minimum infil­tration capacity will be very high,
and rainfall rates will almost always be less than *f*<sub>∞</sub>, leading to
little or no overland flow from such soils.

**Decay Coefficient (*k*<sub>d</sub>)**

For any field infiltration test the rate of decrease (or "decay") of
infiltration capacity from the initial value depends on the initial
moisture content. Thus the *k*<sub>d</sub>-value determined for the same soil
will vary from test to test. It is postulated here that, if *f*<sub>0</sub> is
always specified in relation to a particular soil moisture condition
(e.g., dry), and for moisture contents other than this the time scale is
changed accordingly (i.e., time "zero" is adjusted to correspond with
the constant *f*<sub>0</sub>), then *k*<sub>d</sub> can be considered a constant for the soil
independent of initial moisture content. Put another way, this means
that infiltration curves for the same soil, but different antecedent
conditions, can be made coincident if they are moved along the time
axis. Butler (1957) makes a similar assumption.

Values of *k*<sub>d</sub> found in the literature (Overton and Meadows, 1976;
Wanielista, 1978; Maidment, 1993; ASCE, 1996) range from 0.67 to 120
hr<sup>-1</sup>. Nevertheless most of the values cited appear to be in the range
3 - 6 hr<sup>-1</sup>. The evidence is not clear as to whether there is any
relationship between soil texture and the *k*<sub>d</sub> value although several
published curves seem to indicate a lower value for sandy soils. If no
field data are available, an estimate of 4 hr<sup>-1</sup> could be used. Use of
such an estimate implies that, under ponded conditions, the infiltra­tion
capacity will fall 98 percent of the way towards its minimum value in
the first hour, a not uncommon observation. Rates of decay of
infiltration for several values of *k*<sub>d</sub> are shown in Table 4-5.

**Table 4-5 Rate of decay of infiltration capacity for different values of kd**

| kd (hr⁻¹) | Percent of decline of infiltration capacity towards limiting value f∞ after 1 hour |
|-----------|-----------------------------------------------------------------------------------|
| 2 | 76 |
| 3 | 95 |
| 4 | 98 |
| 5 | 99 |

**Initial Infiltration Capacity (*f*<sub>0</sub>)**

The initial infiltration capacity, *f*<sub>0</sub> depends primarily on soil
type, initial moisture content, and surface vegetation conditions. For
example, Linsley et al. (1982) present data that show, for a sandy loam
soil, a 60 to 70 percent reduction in the *f*<sub>0</sub> value due to wet
initial conditions. They also show that lower *f*<sub>0</sub> values apply for a
loam soil than for a sandy loam soil. As to the effect of vegetation,
Jens and McPherson (1964, pp. 20.20-20.38) list data that show that
dense grass vegetation nearly doubles the infiltra­tion capacities over
those measured for bare soil surfaces.

For the assumption to hold that the decay coefficient *k*<sub>d</sub> is
independent of initial moisture content, *f*<sub>0</sub> must be specified for
the dry soil condition. For long-term continuous simulations SWMM
automatically adjusts the effective *f*<sub>0</sub> value as part of the
infiltration capacity regeneration routine. However, for a single-event
simulation, the user must specify the *f*<sub>0</sub> value for the storm in
question, which may be less than the value for dry soil condi­tions.

Published values of *f*<sub>0</sub> vary depending on the soil, moisture, and
vegetation conditions for the particular test measurement. The *f*<sub>0</sub>
values listed in Table 4-6 can be used as a rough guide. Interpolation
between the values may be required.

**Table 4-6 Representative values for f₀**

**A. DRY soils (with little or no vegetation):**
- Sandy soils: 5 in/hr
- Loam soils: 3 in/hr
- Clay soils: 1 in/hr

**B. DRY soils (with dense vegetation):**
- Multiply values given in A by 2 (after Jens and McPherson, 1964)

**C. MOIST soils (change from dry f₀ value required for single event simulation only):**
- Soils which have drained but not dried out (i.e., field capacity): divide values from A and B by 3
- Soils close to saturation: Choose value close to f∞ value
- Soils which have partially dried out: divide values from A and B by 1.5-2.5

**Regeneration Coefficient (*k*<sub>r</sub>)**

For continuous simulation, infiltration capacity will be regenerated
(recovered) during dry weather according to Equation 4-6. Instead of
asking the user to supply a value for *k*<sub>r</sub>, SWMM instead asks for an
estimate of drying time *T*<sub>dry</sub> in days. This is the time it takes for
a saturated soil to fully recover to a dry state. Drying times are
typically longer than wetting times, implying *k~r~ \< k~d~*. On
well-drained porous soils (e.g., medium to coarse sands), recovery of
infiltration capacity is quite rapid and could well be complete in a
couple of days. For heavier soils, the recovery rate is likely to be
slower, say 7 to 14 days. The choice of the value can also be related to
the interval between a heavy storm and wilting of vegetation.

The Green-Ampt method (discussed below in Section 4.4), bases its
recovery time solely on the soil's saturated hydraulic conductivity
*K*<sub>S</sub>. Adopting its approach produces the following estimate for
*T*<sub>dry</sub> in days:

$$T_{dry} = \frac{3.125}{\sqrt{K_{s}}}$$ (4-12) 

where *K*<sub>S</sub> is expressed in in/hr. Thus this equation predicts a drying
time of 2 days for a sandy soil with *K*<sub>S</sub> = 2.0 in/hr versus 10 days
for a clay soil with *K*<sub>S</sub> of 0.1 in/hr.

Since mathematically, the exponential term in Equation 4-6 would require
an infinite amount of time to allow infiltration capacity to return to
its initial value *f*<sub>0</sub>, SWMM considers "full recovery" to occur when
98 percent of the difference between the initial and minimum capacities
has been achieved. Thus from Equation 4-6 (for *k*<sub>r</sub> in days<sup>-1</sup>),

$$0.02\left( f_{0} - f_{\infty} \right) = \left( f_{0} - f_{\infty} \right)e^{- k_{r}T_{dry}}$$ (4-13) 

which leads to the following estimate of *k*<sub>r</sub> expressed in days<sup>-1</sup>:

$$k_{r} = \frac{- ln(0.02)}{T_{dry}} = \frac{3.912}{T_{dry}}$$ (4-14) 

This computation of *k*<sub>r</sub> from a user-supplied value of *T*<sub>dry</sub> and
its subsequent conversion from days<sup>-1</sup> to sec<sup>-1</sup> is done internally by
SWMM.

## 4.3 Modified Horton Method

A. O. Akan developed a modified version of the Horton infiltration
method (Akan, 1992; Akan and Houghtalen, 2003) that has been added as a
separate infiltration option in SWMM 5. The method uses the same
parameters as the original Horton method but instead of tracking the
time along the Horton decay curve it uses the cumulative infiltration
volume in excess of the minimum infiltration rate as its state variable.
It assumes that part of the infiltrating water will percolate deeper
into the soil at the minimum infiltration rate (commonly taken as the
soil's saturated hydraulic conductivity). As a result, it is the
difference between the actual and minimum infiltration rates that
accumulates just below the surface that causes infiltration capacity to
decrease with time. This method is purported to give more accurate
infiltration estimates when low rainfall intensities occur.

### 4.3.1 Governing Equations

The modified method starts with the same exponential decay equation as
the original Horton method:

$$f_{p} = f_{\infty} + \left( f_{0} - f_{\infty} \right)e^{- k_{d}t}$$ (4-15) 

where all symbols have been previously defined.

As with the original Horton method, the actual infiltration rate *f* is
the smaller of *f*<sub>p</sub> and the rainfall rate *i*. Integrating Equation
4-15 from 0 to time t produces the following equation for the cumulative
infiltration through time t:

$$F = f_{\infty}t + \frac{\left( f_{0} - f_{\infty} \right)}{k_{d}}\left( 1 - e^{- k_{d}t} \right)$$ (4-16) 

Solving for $e^{- k_{d}t}$ from (4-15) and substituting into (4-16)
gives:

$$F = f_{\infty}t + \frac{f_{0} - f_{p}}{k_{d}}$$ (4-17) 

and solving for *f*<sub>p</sub> gives:

$$f_{p} = f_{0} - k_{d}(F - f_{\infty}t)$$ (4-18) 

The last term in parenthesis is equivalent
to$\int_{0}^{t}{\left( f - f_{\infty} \right)dt}$. So one can
approximate Eq. (4-18) by

$$f_{p} = f_{0} - {k_{d}F}_{e}$$ (4-19) 

where $F_{e} = \sum_{i}^{}{(f_{i} - f_{\infty})\mathrm{\Delta}t_{i}}$
and $f_{i}$ is the actual infiltration over a previous time interval
$\mathrm{\Delta}t_{i}$.

### 4.3.2 Recovery of Infiltration Capacity

Regarding recovery of infiltration capacity during dry periods, one can
assume that the instantaneous recovery rate is proportional to the
difference between the current capacity and the maximum capacity:

$$\frac{df_{r}}{dt} = k_{r}(f_{0} - f_{r})$$ (4-20) 

where $f_{r}$ represents the infiltration capacity during recovery and
$k_{r}$ is the same regeneration coefficient (1/sec) used in the
conventional Horton method. Integrating this equation starting at some
time where the infiltration capacity is $f_{r0}$ produces the following
result for the capacity after a recovery time of *t:*

$$f_{r} = f_{0} - (f_{0} - f_{r0})e^{- k_{r}t}$$ (4-21) 

From Eq. 4-19, the cumulative excess infiltration volume corresponding
to this capacity,$F_{er}$, would be:

$$F_{er} = (f_{0} - f_{r})/k_{d}$$ (4-22) 

and substituting 4-21 for $f_{r}$ gives:

$$F_{er} = \frac{\left( f_{0} - f_{r0} \right)}{k_{d}}e^{- k_{r}t}$$ (4-23) 

But again from 4-19,

$$(f_{0} - f_{r0})/k_{d} = F_{e}$$ (4-24) 

so the new cumulative volume after recovery is simply:

$$F_{er} = F_{e}e^{- k_{r}t}$$ (4-25) 

### 4.3.3 Computational Scheme

The detailed computational scheme for computing the Modified Horton
infiltration rate for each subcatchment within a study area over a
single time step of a simulation is presented in the sidebar titled
**Computational Scheme for Modified Horton Infiltration**.

<div style="background-color: #f8f9fa; border: 1px solid #dee2e6; border-radius: 5px; padding: 15px; margin: 10px 0;">

**Computational Scheme for Modified Horton Infiltration**

The following variables are assumed known at the start of each time step Δt (sec) for the pervious sub-area of each SWMM subcatchment:

- **i** = rainfall rate (ft/sec)
- **d** = depth of ponded surface water (ft)
- **F<sub>e</sub>** = excess infiltrated volume (ft)

as are the following constants:

- **f<sub>0</sub>** = maximum (or initial) infiltration capacity (ft/sec)
- **f<sub>∞</sub>** = minimum (or ultimate) infiltration capacity (ft/sec)
- **k<sub>d</sub>** = infiltration capacity decay coefficient (sec<sup>-1</sup>)
- **k<sub>r</sub>** = infiltration capacity recovery coefficient (sec<sup>-1</sup>)
- **F<sub>max</sub>** = maximum infiltration volume possible (optional) (ft)

Initially at time 0, F<sub>e</sub> = 0.

The following steps are used to compute the modified Horton infiltration rate f over a single time step of a simulation:

1. **Compute the available rainfall rate:** i<sub>a</sub> = i + d / Δt.

2. **If i<sub>a</sub> = 0**, meaning the surface is dry, then update the current excess infiltrated volume as follows:
   ```
   F_e = F_e e^(-k_r Δt)
   ```
   and set the infiltration rate f to 0.

3. **Else if F<sub>e</sub> ≥ F<sub>max</sub>**, set f<sub>p</sub> to 0. Otherwise compute a potential infiltration rate f<sub>p</sub> from
   ```
   f_p = max(f_0 - k_d F_e, f_∞)
   ```

4. **Compute the actual infiltration rate f** as the lesser of f<sub>p</sub> and the available rainfall rate:
   ```
   f = min(f_p, i_a)
   ```

5. **If f > f<sub>∞</sub>** then update the cumulative excess infiltration volume:
   ```
   F_e ← min(F_e + (f - f_∞)Δt, F_max)
   ```

</div>

### 4.3.4 Parameter Estimates

Because the modified Horton method utilizes the same parameters as the
original Horton method, the description in section 4.2.4 of how to
estimate their values also applies to the modified method.

## 4.4 Green-Ampt Method

The Green-Ampt equation (Green and Ampt, 1911) has received considerable
attention in recent years. The original equation was for infiltration
with excess water at the surface at all times. Mein and Larson (1973)
showed how it could be adapted to a steady rainfall input and proposed a
way in which the capillary suction parameter could be determined. Chu
(1978) has shown the applicability of the equation to the unsteady
rainfall situation, using data for a field catchment. The Green-Ampt
method was added into SWMM III in 1981 by R.G. Mein and W. Huber (Huber
et al., 1981).

### 4.4.1 Governing Equations

The Green-Ampt conceptualization of the infiltration process is one in
which infiltrated water moves vertically downward in a saturated layer,
beginning at the surface (Figure 4-5). In the wetted zone the moisture
content *θ* is at saturation *θ*<sub>s</sub> while the moisture content in the
un-wetted zone is at some known initial level *θ*<sub>i</sub>.

![Two-zone representation of the Green-Ampt infiltration model](VolumeI/media/media/figure4-5.png)

**Figure 4-5 Two-zone representation of the Green-Ampt infiltration model (after Nicklow et al., 2006)**

The water velocity within the wetted zone is given by Darcy's Law as a
function of the saturated hydraulic conductivity *K*<sub>S</sub>, the capillary
suction head along the wetting front *ψ*<sub>S</sub>, the depth of ponded water
at the surface *d*, and the depth of the saturated layer below the
surface *L*<sub>s</sub>:

$$f_{p} = K_{s}\left\lbrack \frac{d + L_{s} + \psi_{s}}{L_{s}} \right\rbrack$$ (4-26) 

The depth of the saturated layer *L*<sub>s</sub> can be expressed in terms of the
cumulative infiltration, *F*, and the initial moisture deficit to be
filled below the wetting front, *θ*<sub>d</sub> = *θ*<sub>s</sub> - *θ*<sub>i</sub> as
![](VolumeI/media/media/image20.wmf). Substituting this into Equation 4-26 and
assuming that *d* is small compared to the other depths gives the
Green-Ampt equation for saturated conditions:

$$f_{p} = K_{s}\left\lbrack 1 + \frac{\psi_{s}\theta_{d}}{F} \right\rbrack$$ (4-27) 

Equation 4-27 applies only after a saturated layer develops at the
ground surface. Prior to this point in time the infiltration capacity
will equal the rainfall intensity:

$$f_{p} = i$$ (4-28) 

As time increases, one can test whether saturation has been reached by
solving 4-27 for *F* (which will be denoted as *F*<sub>s</sub>) with *f*<sub>p</sub> set
equal to *i* and check if this value equals or exceeds the actual
cumulative infiltration *F*:

$$F_{s} = \frac{K_{s}\psi_{s}\theta_{d}}{i - K_{s}}$$ (4-29) 

Note that there is no calculation of *F*<sub>s</sub> when *i \<= K~s~*, although
*F* still gets updated during such periods. Finally, in this scheme the
actual infiltration *f* is the same as the potential value *f*<sub>p</sub>:

$$f = f_{p}$$ (4-30) 

The two equations are illustrated in Figure 4-6 for the situation *K*<sub>S</sub>
= 0.25 in/hr, *ψ*<sub>S</sub> = 6.5 in, and *θ*<sub>d</sub> = 0.20. The initial, flat
portion of the curve corresponds to *f = i*, up to the point where *F =
F~s~* (Equation 4-29). The remainder of the curve corresponds to the
potential rate computed with Equation 4-27. Note that the infiltration
rate approaches *K*<sub>S</sub> (0.25 in/hr) asymptotically.

![Illustration of infiltration capacity as function of cumulative infiltration for the Green-Ampt method](VolumeI/media/media/figure4-6.png)

**Figure 4-6 Illustration of infiltration capacity as function of cumulative infiltration for the Green-Ampt method**

Equation 4-27 shows that the infiltration capacity after surface
saturation depends on the infiltrated volume, which in turn depends on
the infiltration rates in previous time steps. To avoid numerical errors
over long time steps, the integrated form of the Green-Ampt equation is
more suitable. That is, *f*<sub>p</sub> is replaced by *dF/dt* and integrated to
obtain:

$$F = K_{s} + \psi_{s}\theta_{d}\ln\left( 1 + \frac{F}{\psi_{s}\theta_{d}} \right)$$ (4-31) 

If *F*<sub>1</sub> is the known cumulative infiltration at the start of the time
step and *F*<sub>2</sub> the unknown cumulative infiltration at the end of the
time step then one can write:

$$F_{2} = C + \psi_{s}\theta_{d}\ln\left( F_{2} + \psi_{s}\theta_{d} \right)$$ (4-32) 

where
$C = K_{s}\Delta t + F_{1} - \psi_{s}\theta_{d}\ln\left( F_{1} + \psi_{s}\theta_{d} \right)$
is a known constant. Equation 4-32 can be solved numerically for *F*<sub>2</sub>.
The average infiltration capacity *f*<sub>p</sub> over the time step can then be
computed as $\left( F_{2} - F_{1} \right)/\Delta t$.

### 4.4.2 Recovery of Infiltration Capacity

Evaporation, subsurface drainage, and moisture redistribution between
rainfall events decrease the soil moisture content in the upper soil
zone and increase the infiltration capacity of the soil. The processes
involved are complex and depend on many factors. In SWMM a simple
empirical routine (Huber et al., 1981) is used as outlined below;
commonly used units are given in the equations to make the description
easier to understand. Note that this procedure suffers from the same
lack of relationship to ET as does the Horton recovery, discussed
earlier.

Infiltration is usually dominated by conditions in the uppermost layer
of the soil. The thickness of this layer depends on the soil type; for a
sandy soil it could be several inches, for heavy clay it would be less.
The equation used to determine the thickness of the layer *L*<sub>u</sub> is:

$$L_{u} = 4\sqrt{K_{s}}$$ (4-33) 

where *L*<sub>u</sub> has units of inches and *K*<sub>S</sub> is expressed in in/hr. Thus
for a high *K*<sub>S</sub> of 0.5 in/hr (12.7 mm/hr) the thickness computed by
Equation 4-33 is 2.83 inches (71.8 mm). For a soil with a low hydraulic
conductivity, say *K*<sub>S</sub> = 0.1 in/hr (2.5 mm/hr), the computed thickness
is 1.26 inches (32.1 mm). This constant thickness is different from the
saturated zone thickness *L*<sub>s</sub> shown in Figure 4-5 which grows over
time as infiltration proceeds.

In the Green-Ampt model, the initial soil moisture deficit at the start
of a rainfall event determines how much infiltration capacity is
available during the event itself. Recall that the moisture deficit
*θ*<sub>d</sub> is the difference between the saturated moisture content *θ*<sub>s</sub>
and the initial moisture content *θ*<sub>i</sub>. During a dry period the
moisture deficit in the upper soil zone, *θ*<sub>du</sub>, is regenerated, i.e.,
its value is increased. Thus SWMM keeps continuous track of this
quantity. At the start of a simulation, *θ*<sub>du</sub> is set equal to the
user-supplied initial value of *θ*<sub>dmax</sub>. During a wet period when
infiltration occurs at a rate *f* over a time step of *Δt*, *θ*<sub>du</sub> is
decreased according to:

$$\theta_{du} \leftarrow \theta_{du} - \frac{f\Delta t}{L_{u}}$$ (4-34) 

down to a possible limiting value of 0. During a dry period it increases
as follows:

$$\theta_{du} \leftarrow \theta_{du} + k_{r}\theta_{dmax}\Delta t$$ (4-35) 

up to a maximum possible value of *θ*<sub>dmax</sub> , where *k*<sub>r</sub> is a recovery
constant (hr<sup>-1</sup>).

One can assume that the recovery constant is also dependent on *K*<sub>S</sub>,
such that tight, clay soils with low *K*<sub>S</sub> take longer to recover than
do loose, sandy soils with high *K*<sub>S</sub>. The following relationship is
used for *k*<sub>r</sub>:

$$k_{r} = \frac{\sqrt{K_{s}}}{75}$$ (4-36) 

where the constant 75 has units of (in-hr)<sup>1/2</sup>. Note that the time it
would take a fully saturated soil to recovery to its maximum capacity is
simply:

$$\frac{1}{k_{r}} = \frac{75}{\sqrt{K_{s}}}\ $$ 

hours (or $3.125/\sqrt{K_{s}}$ days).

To complete the recovery process it is necessary to define the minimum
amount of time that a soil must remain in recovery before any further
rainfall would be considered as an independent event. This time *T*<sub>r</sub>
(hr) is computed as:

$$T_{r} = \frac{0.06}{k_{r}} = \frac{4.5}{\sqrt{K_{s}}}$$ (4-37) 

Thus when a new period of rainfall occurs after a recovery interval of
at least *T*<sub>r</sub> hours, the two-stage Green-Ampt infiltration process is
re-started with *θ*<sub>d</sub> = *θ*<sub>du</sub> and *F* = 0. Figure 4-7 summarizes the
functional dependence of the three internally computed recovery
parameters *L*<sub>u</sub>, *k*<sub>r</sub>, and *T*<sub>r</sub> on the saturated hydraulic
conductivity *K*<sub>S</sub>.

![Green-Ampt recovery parameters as functions of hydraulic conductivity](VolumeI/media/media/figure4-7.png)

**Figure 4-7 Green-Ampt recovery parameters as functions of hydraulic conductivity**

### 4.4.3 Computational Scheme

The detailed computational scheme for computing the Green-Ampt
infiltration rate for each subcatchment within a study area over a
single time step of a simulation is presented in the sidebar below.

### 4.4.4 Parameter Estimates

The soil parameters that a user must supply for each subcatchment for
the Green-Ampt infiltration method are:

- *K*<sub>S</sub> - the saturated hydraulic conductivity (in/hr or mm/hr),

- *ψ*<sub>S</sub> - the suction head at the wetting front (in or mm),

- *θ*<sub>dmax</sub> - the maximum moisture deficit available (volume of dry
  voids per volume of soil).

Conversions between the user-supplied units of these parameters (in (or
mm) and hr) and those used internally (ft and sec) are handled
automatically by the program.

**Saturated Hydraulic Conductivity (*K*<sub>S</sub>)**

Probably the best single source for estimates of saturated hydraulic
conductivity (*K*<sub>S</sub>) and suction head (*ψ*<sub>S</sub>) for a wide range of
soils -- and one that makes use of the Green-Ampt method relatively
attractive -- is the data by Rawls et al. (1983), shown in Table 4-7.
These data were derived from measurements made on roughly 5000 soils
across the United States and while they will never be truly site
specific, they are certainly consistent and defensible. Although there
is considerable variation in the parameter estimates, a good first
approximation may be made using the table. Values of hydraulic
conductivity may also be used for estimates of the Horton parameter
*f*<sub>∞</sub>. But the range of values shown for porosity and suction head (the
authors do not provide ranges for *K*<sub>S</sub>) should be a warning about
placing too much faith in such generalized estimates.

The NRCS Soil Survey Physical Data (see Figure 4-1) values for hydraulic
conductivity could also be used as a preliminary estimate. A better
guide for the *K*<sub>S</sub> values is as given for parameter *f*<sub>∞</sub> for the
Horton equation; theoretically these parameters (i.e., *f*<sub>∞</sub> and
*K*<sub>S</sub>) should be equal for the same soil. Note that, in general, the
range of *K*<sub>S</sub> values encountered will be of the order of tenths of an
inch per hour.

Another source of conductivity estimates is the regression equation
developed by Saxton and Rawls (2006) that predicts *K*<sub>S</sub> from the sand,
clay and organic matter content of a soil. See Section 5.5.2 of the
Groundwater chapter for more details.

**Table 4-7 Green-Ampt parameters for different soil classes (Rawls et al., 1983)**

(Numbers in parentheses are ± one standard deviation from the parameter
value shown.)

| Soil Class | Porosity, φ | Effective Porosity, φe* | Wetting Front Suction Head, ψs (in) | Saturated Hydraulic Conductivity, Ks (in/hr) |
|------------|-------------|-------------------------|-------------------------------------|-------------------------------------|
| Sand | 0.437 (0.374--0.500) | 0.417 (0.354--0.480) | 1.95 (0.38--9.98) | 4.74 |
| Loamy sand | 0.437 (0.363--0.506) | 0.401 (0.329--0.473) | 2.41 (0.53--11.00) | 1.18 |
| Sandy loam | 0.453 (0.351--0.555) | 0.412 (0.283--0.541) | 4.33 (1.05--17.90) | 0.43 |
| Loam | 0.463 (0.375--0.551) | 0.434 (0.334--0.534) | 3.50 (0.52--23.38) | 0.13 |
| Silt loam | 0.501 (0.420--0.582) | 0.486 (0.394--0.578) | 6.57 (1.15--37.56) | 0.26 |
| Sandy clay loam | 0.398 (0.332--0.464) | 0.330 (0.235--0.425) | 8.60 (1.74--42.52) | 0.06 |
| Clay loam | 0.464 (0.409--0.519) | 0.309 (0.279--0.501) | 8.22 (1.89--35.87) | 0.04 |
| Silty clay loam | 0.471 (0.418--0.524) | 0.432 (0.347--0.517) | 10.75 (2.23--51.77) | 0.04 |
| Sandy clay | 0.430 (0.370--0.490) | 0.321 (0.207--0.435) | 9.41 (1.61--55.20) | 0.02 |
| Silty clay | 0.479 (0.425--0.533) | 0.423 (0.334--0.512) | 11.50 (2.41--54.88) | 0.02 |
| Clay | 0.475 (0.427--0.523) | 0.385 (0.269--0.501) | 12.45 (2.52--61.61) | 0.01 |

\*Effective porosity is the difference between the porosity *φ* and the
residual moisture content *φ*<sub>r</sub> that remains after a saturated soil is
allowed to drain thoroughly.

Urban soils are usually highly disturbed (Pitt et al., 1999, 2001; Pitt
and Voorhees, 2000). Construction has often occurred on or nearby the
locations in question, and soils may be compacted from their natural
state. Alternatively, soils are sometimes imported for horticultural
purposes. Such imported soils (e.g., for lawns) may exhibit relatively
high infiltration rates. The parameter estimates discussed previously
are based on data for *undisturbed* soils, e.g., using Natural Resources
Conservation Service (NRCS) data. Parameters for natural, undisturbed
soils are likely to *overestimate* the infiltration characteristics for
urban soils. Modelers should bear in mind that only site-specific
infiltrometer and/or soil physics tests can determine local infiltration
properties, and that high spatial variability is the rule, rather than
the exception.

**Suction Head (*ψ*<sub>S</sub>)**

The suction head, *ψ*<sub>S</sub> (also referred to as capillary tension), is
perhaps the most difficult parameter to measure. It can be derived from
soil moisture - conductivity data (Mein and Larsen, 1973) of the type
shown in Figures 5-5 in Chapter 5 for groundwater. Unfortunately, such
detailed data are rare for most soils. Fortunately the results obtained
for Green-Ampt infiltration are not highly sensitive to the estimate of
*ψ*<sub>S</sub> (Brakensiek and Onstad, 1977).

An excellent local data source can often be found in Soil Science
departments at state universities. Tests are run on a variety of soils
found within the state, including soil moisture versus soil tension
data, from which *ψ*<sub>S</sub> can be derived. For example, Carlisle et al.
(1981) provide such data for Florida soils along with information on
*K*<sub>S</sub>, bulk density, and other physical and chemical properties.

Approximate values may also be found from several authors: Mein and
Larsen (1973), Brakensiek and Onstad (1977), Clapp and Hornberger
(1978), Chu (1978), Rawls et al. (1983). Published values vary
considerably and conflict; however, a range of 2 to 15 inches (50 to 380
mm) covers virtually all soil textures. But as with *K*<sub>S</sub>, probably the
best single source for estimates for capillary suction (*ψ*<sub>S</sub>) is the
data by Rawls et al. (1983) listed in Table 4-7. Brakensiek et al.
(1981) noted that *ψ*<sub>S</sub> was highly correlated with hydraulic
conductivity over all soil classes. Using nonlinear regression on the
average values for these two variables listed in Table 4-7 produces the
following relationship for *K*<sub>S</sub> in in/hr and *ψ*<sub>S</sub> in inches:

$\psi_{s} = 3.237K_{S}^{- 0.328}$ (*R*<sup>2</sup>> = 0.9) (4-38)                

**Maximum Moisture Deficit (*θ*<sub>dmax</sub>)**

The maximum moisture deficit, *θ*<sub>dmax</sub> is defined as the difference
between the moisture content at saturation and at the start of the
simulation. Because this parameter is the most sensitive of the three
parameters for estimates of runoff from pervious areas (Brakensiek and
Onstad, 1977), some care should be taken in determining the best
*θ*<sub>dmax</sub> value to use. The saturated moisture content is approximately
equal to the soil's porosity *φ* (i.e., the fraction of voids), assuming
one ignores the 5 - 10% of trapped air that typically exists at
saturation. After a saturated soil is allowed to drain thoroughly, the
residual moisture content that remains is *φ*<sub>r</sub>. The effective porosity
*φ*<sub>e</sub> is defined as *φ*<sub>e</sub> = *φ* - *φ*<sub>r</sub> and can be used to
represent *θ*<sub>dmax</sub> for dry antecedent conditions. Typical values of
*φ*<sub>e</sub> are included in the Rawls et al. (1983) data set listed in Table
4-7.

Sandy soils tend to have lower porosities than clay soils, but drain to
lower moisture contents between storms because the water is not held so
strongly in the soil pores. Consequently, values of *θ*<sub>dmax</sub> for dry
antecedent conditions tend to be higher for sandy soils than for clay
soils. Table 4-8, derived from Clapp and Hornberger (1973), is another
source of *θ*<sub>dmax</sub> values for various soil types.

**Table 4-8 Typical values of θdmax for various soil types.**

| Soil Texture | Typical θdmax at Soil Wilting Point |
|--------------|-------------------------------------|
| Sand | 0.34 |
| Sandy Loam | 0.33 |
| Silt Loam | 0.32 |
| Loam | 0.31 |
| Sandy Clay Loam | 0.26 |
| Clay Loam | 0.24 |
| Clay | 0.21 |

These *θ*<sub>dmax</sub> values would be suitable for input for long term
continuous simulation; the soil type selected should correspond to the
surface layer for the particular subcatchment. For single event
simulation the values of Table 4-8 would apply only to very dry
antecedent conditions. For moist or wet antecedent conditions lower
values of *θ*<sub>dmax</sub> should be used. When estimating the particular value
it should be borne in mind that sandy soils drain more quickly than
clayey soils, i.e., for the same time since the previous event, the
*θ*<sub>dmax</sub> value for a sandy soil will be closer in value to that of
Table 4-8 than it would be for a clayey soil.

Another estimate for *θ*<sub>dmax</sub> may be based on the NRCS Soil Survey
Physical Data as "Available Moisture Capacity" in/in of soil
(dimensionless fraction), which is defined as the difference between
field capacity and the wilting point. Thus, it is an underestimate of
the maximum *θ*<sub>d</sub> value. Furthermore, Available Moisture Capacity
values listed may exhibit similar variability (or lack thereof) as for
hydraulic conductivity estimates discussed earlier, but these values are
at least specific to the soil in question. For instance, for the
Woodburn silt loam illustrated in Figure 4-1, *θ*<sub>dmax</sub> might be at the
high end of the range of 0.19 -- 0.24 for the surface layer
(considerably less than the generic value of 0.32 for silt loam in Table
4-8 or the range of 0.394 to 0.578 given in Table 4-7).

Finally, the initial moisture deficit can be related to another very
general measure of a soil: its storage capacity, S, which can be
expressed as:

$$S = d_{wt}\theta_{dmax}$$ (4-39) 

where *d*<sub>wt</sub> is the depth to the sub-surface water table. Estimates of
soil storage capacity, *S*, are available using the Curve Number method,
discussed below. That is, *S* is a function of the curve number (Section
4.5.4), for which a vast literature is available. If depth to water
table is known, or if typical depths are given for a soil on its Soil
Survey Interpretation data, then Equation 4-39 may be solved for
*θ*<sub>dmax</sub>.

## 4.5 Curve Number Method

The Curve Number infiltration method is new to SWMM 5. It is based on
the widely used SCS (Soil Conservation Service, now known as the NRCS --
Natural Resource Conservation Service) curve number method for
evaluating rainfall excess. First developed in 1954, the method is
embodied in the widely used TR-20 and TR-55 computer models (NRCS, 1986)
as well as most hydrology handbooks and textbooks (e.g., Bedient et al.,
2013). It was added into SWMM to take advantage of its familiarity to
most practicing engineers and the availability of tabulated curve
numbers for a wide range of land use and soil groups. The original curve
number method is a combined loss method that lumps together all losses
due to interception, depression storage, and infiltration to predict the
total rainfall excess from a rainfall event. The SWMM uses a modified,
incremental form of the method that accounts only for infiltration
losses, since the other abstractions are modeled separately. Other
incremental applications of the curve number method have been proposed
by Chen (1975), Aron et al. (1977) and Akan and Houghtalen (2003).

### 4.5.1 Governing Equations

In its classic form, the Curve Number model uses the following equation
to relate total event runoff *Q* (in) to total event precipitation *P*
(in) (Haan et al., 1994; McCuen, 1998; Bedient et al., 2013; NRCS,
2004b):

$$Q = \frac{P^{2}}{P + S_{\max}}$$ (4-40)

where *S*<sub>max</sub> = the soil's maximum moisture storage capacity (inches).
*S*<sub>max</sub> can also be thought of as the difference in water volume
contained in a fully saturated soil versus a fully drained soil. In this
sense it is similar to the maximum moisture deficit parameter *θ*<sub>dmax</sub>
used in the Green-Ampt model, except it is expressed on a volumetric
basis rather than as a fraction (see Equation 4-39). *S*<sub>max</sub> is derived
from a tabulated "curve number" *CN* that varies with soil type and
antecedent conditions:

$$S_{\max} = \frac{1000}{CN} - 10$$ (4-41)

It should be emphasized that Equation 4-40 and subsequent equations use
units of **inches**. Curve numbers for various soil types and
land covers are tabulated in the NRCS's National Engineering Handbook
(NRCS, 2004a) and in many text books.

In the formal SCS method, Equation 4-40 is written with *P* replaced by
*P* - *I*<sub>a</sub> where *I*<sub>a</sub> is an initial abstraction (in) that accounts
for the volume of rainfall captured by vegetative interception, filling
of depression storage, and initial soil wetting. Because SWMM already
accounts for these phenomena through its depression storage parameter,
*d*<sub>p</sub>, this refinement is not included here.

Assuming that all rainfall that does not run off is lost to infiltration
(i.e., *P* - *Q* = *F*), Equation 4-40 can be extended to predict total
(cumulative) infiltration *F* (in) as:

$$F = P - \frac{P^{2}}{P + S_{\max}}$$ (4-42)

For a continuous model like SWMM, Equation 4-42 can be applied in an
incremental fashion to compute an infiltration rate *f* at each time
step. Let *P*<sub>1</sub> and *F*<sub>1</sub> be the cumulative precipitation and
infiltration, respectively, at the start of the time step. At the end of
the time step:

$$P_{2} = P_{1} + i\Delta t$$ (4-43) 

and

$$F_{2} = P_{2} - \frac{P_{2}^{2}}{P_{2} + S_{e}}$$ (4-44) 

where *P*<sub>2</sub> and *F*<sub>2</sub> are the cumulative precipitation and
infiltration values, respectively, at the end of a time step *Δt* (hr),
*i* (in/hr) is the rainfall rate over the time step, and *S*<sub>e</sub> is the
moisture storage capacity at the start of the rainfall event to which
the time step belongs. For a single event simulation, *S*<sub>e</sub> equals
*S*<sub>max</sub> but may be lower when moisture storage capacity depletion and
recovery occur over a longer simulation period as discussed in the next
section.

The infiltration rate *f* (ft/sec) can then be computed as:

$$f = \left( F_{2} - F_{1} \right)/\Delta t$$ (4-45) 

and the cumulative values get updated to *P*<sub>1</sub> = *P*<sub>2</sub> and *F*<sub>1</sub> = *F*<sub>2</sub>
to prepare for the next time step. Note that as it stands, this model
would not allow for any infiltration of ponded water when there is a
period of no rainfall within an event. To overcome this limitation it is
assumed that the infiltration rate for such periods remains the same as
in the immediately preceding period. Also, when overland flow re-routing
occurs (see Section 3.6), the rainfall rate *i* in Equation 4-43 does
not include the additional re-routed flow.

### 4.5.2 Recovery of Storage Capacity

As with the other infiltration methods discussed, a soil's moisture
storage capacity is depleted during wet periods and replenished during
dry periods. To model this behavior with the Curve Number method, the
variable *S* is introduced to track the remaining storage capacity
(i.e., moisture deficit) over time. It is analogous to the state
variable *θ*<sub>du</sub> used in the Green-Ampt method. Initially, *S* = *S*<sub>max</sub>.
Whenever infiltration at rate *f* occurs over a time step *Δt*, *S* is
reduced by *fΔt*. During a period with no infiltration *S* is assumed to
be replenished at a rate proportional to *S*<sub>max</sub>:

$$S \leftarrow S + k_{r}S_{\max}\mathrm{\Delta}t$$ (4-46) 

where *k*<sub>r</sub> is a storage capacity recovery constant (hr<sup>-1</sup>). This
recovery expression has the same form as used in the Green-Ampt model
and the coefficient *k*<sub>r</sub> has a similar meaning in both models.

Because the Curve Number method was originally meant to be applied to
single, discrete rainfall events, a mechanism is needed to define when
separate events occur. At the start of a new event, the cumulative
variables *P* and *F* are reset to 0 and *S*<sub>e</sub> is set equal to the
current remaining storage capacity *S*. Once again borrowing from the
Green-Ampt method, a period of *T*<sub>r</sub> hours without rainfall must occur
before the next rainfall period is deemed to begin a new event. *T*<sub>r</sub>
is assumed to be related to the recovery constant *k*<sub>r</sub> through
Equation 4-25 which is repeated here:

$$T_{r} = \frac{0.06}{k_{r}}$$ (4-47) 

### 4.5.3 Computational Scheme

The detailed computational scheme for computing Curve Number
infiltration for each subcatchment within a study area over a single
time step of a simulation is presented in the sidebar below.

<div style="background-color: #f8f9fa; border: 1px solid #dee2e6; border-radius: 5px; padding: 15px; margin: 10px 0;">

**Computational Scheme for Curve Number Infiltration**

**Note:** For ease of presentation the following description uses length units of inches and time units of hours rather than feet and seconds which are used internally in SWMM.

The following variables are assumed known at the start of each time step Δ*t* (hr) for the pervious subarea of each subcatchment:

- **i** = rainfall rate over the current time step (in/hr)
- **d** = depth of ponded surface water (in)
- **P<sub>1</sub>** = cumulative rainfall for the current rainfall event (in)
- **S<sub>e</sub>** = soil moisture storage capacity at the start of the current rainfall event (in)
- **S** = soil moisture storage capacity remaining (in)
- **F<sub>1</sub>** = cumulative infiltration volume (in)
- **T** = time since the last period with rainfall (hr)

as are the following constants:

- **S<sub>max</sub>** = maximum moisture storage capacity as computed from the curve number (in)
- **k<sub>r</sub>** = storage capacity recovery constant (hr<sup>-1</sup>)
- **T<sub>r</sub>** = minimum recovery time before a new rainfall event can occur (hr)

Initially at time 0, P<sub>1</sub> = 0, S<sub>e</sub> = S = S<sub>max</sub>, F<sub>1</sub> = 0, and T = T<sub>r</sub>.

The computational steps for computing the Curve Number infiltration rate *f* for a given subcatchment over a single time step of a simulation proceed as follows:

1. **If there is rainfall (i > 0) then:**
   - **If a new event has begun (T ≥ T<sub>r</sub>) then reset the following variables:** P<sub>1</sub> = 0, F<sub>1</sub> = 0, and S<sub>e</sub> = S.
   - **Reset the time since the last rainfall:** T = 0.
   - **Compute cumulative rainfall (P<sub>2</sub>) and infiltration (F<sub>2</sub>) at the end of the time step:**
     ```
     P₂ = P₁ + iΔt
     F₂ = P₂ - (P₂²)/(P₂ + Sₑ)
     ```
   - **Compute a potential infiltration rate:**
     ```
     fₚ = (F₂ - F₁)/Δt
     ```
   - **Update cumulative rainfall and infiltration:**
     ```
     P₁ = P₂
     F₁ = F₂
     ```

2. **If there is no rainfall then increase the inter-event time (T ← T + Δt) and set the potential infiltration rate to the rate from the previous time period (f<sub>p</sub> = f).**

3. **If there is some potential infiltration (f<sub>p</sub> > 0) then:**
   - **Limit the actual infiltration rate to the maximum available rate:**
     ```
     f = min[fₚ, i + d/Δt]
     ```
   - **Reduce the soil moisture storage capacity:**
     ```
     S ← max[S - fΔt, 0]
     ```

4. **Otherwise regenerate soil moisture storage capacity:**
   ```
   S ← min[S + kᵣSₘₐₓΔt, Sₘₐₓ]
   ```

</div>


### 4.5.4 Parameter Estimates

There are only two parameters required for each subcatchment using the
Curve Number infiltration method:

- the curve number

- the drying time (i.e., the time it takes a fully saturated soil to
  recover to a dry state).

The curve number is used to compute the maximum soil moisture storage
capacity (*S*<sub>max</sub>) using Equation 4-41. The drying time *T*<sub>dry</sub> in
days is used to compute the regeneration constant *k*<sub>r</sub> in hours<sup>-1</sup>
as:

$$k_{r} = \frac{1}{24T_{dry}}$$ (4-48) 

The minimum inter-event recovery time *T*<sub>r</sub> is then computed from
*k*<sub>r</sub> using Equation 4-47.

A highly structured method for estimating curve numbers is provided by
the NRCS (NRCS, 2004a; McCuen, 1998, Bedient et al., 2013 and virtually
every hydrology text). Such estimates are embedded in engineering
practice through Table 4-9 in which curve number values are given as
function of land use and soil Hydrologic Soil Group (A through D).
Hydrologic Soil Group is provided on the NRCS Soil Survey data discussed
in Section 4.1. For instance, the Woodburn silt loam of Figure 4-1 is in
Hydrologic Soil Group B.

There are several things to keep in mind when using curve numbers from
Table 4-9. First, these curve numbers apply only to normal antecedent
moisture conditions (AMC II). For AMC I (low moisture) or AMC III (high
moisture) the following adjustments can be made to the tabulated values
(NRCS, 2004a):

$$CN_{I} = \frac{4.2CN_{II}}{10 - 0.058{CN}_{II}}$$ (4-49) 

$${CN}_{III} = \frac{23{CN}_{II}}{10 - 0.13{CN}_{II}}$$ (4-50) 

where *CN*<sub>i</sub> refers to the curve number for antecedent moisture
condition *i*. For long-term simulations the AMC I curve number should
be used to allow the soil to reach its maximum possible moisture
retention capacity during extended dry periods.

Second, the urban land use descriptions included in Table 4-9 lump
together the pervious and impervious portions of the subcatchment area
to which a curve number is assigned. This means that the subcatchment in
question must be modeled as being completely pervious, with no
partitioning into separate pervious and impervious areas as is normally
done in SWMM (refer to Section 3.3). Otherwise too much runoff will be
generated. If one wants to continue to partition their subcatchments
into pervious and impervious areas, they will have to either adjust the
curve numbers taken from Table 4-9 to remove the effects of
imperviousness or find another source of curve numbers, such as from
calibration against field measurements (see Shuster and Pappas, 2011).

**Table 4-9 Runoff curve numbers for selected land uses (NRCS, 2004a)**

(For antecedent moisture condition II)

| Land Use Description | A | B | C | D |
|---------------------|---|---|---|---|
| **Cultivated land¹** | | | | |
| Without conservation treatment | 72 | 81 | 88 | 91 |
| With conservation treatment | 62 | 71 | 78 | 81 |
| **Pasture or range land** | | | | |
| Poor condition | 68 | 79 | 86 | 89 |
| Good condition | 39 | 61 | 74 | 80 |
| **Meadow** | | | | |
| Good condition | 30 | 58 | 71 | 78 |
| **Wood or forest land** | | | | |
| Thin stand, poor cover, no mulch | 45 | 66 | 77 | 83 |
| Good cover² | 25 | 55 | 70 | 77 |
| **Open spaces, lawns, parks, golf courses, cemeteries, etc.** | | | | |
| Good condition: grass cover on 75% or more of the area | 39 | 61 | 74 | 80 |
| Fair condition: grass cover on 50 -- 75% of the area | 49 | 69 | 79 | 84 |
| **Commercial and business areas (85% impervious)** | 89 | 92 | 94 | 95 |
| **Industrial districts (72% impervious)** | 81 | 88 | 91 | 93 |
| **Residential³** | | | | |
| Average lot size | Average % impervious⁴ | | | |
| 1/8 ac or less | 65 | 77 | 85 | 90 | 92 |
| 1/4 ac | 38 | 61 | 75 | 83 | 87 |
| 1/3 ac | 30 | 57 | 72 | 81 | 86 |
| 1/2 ac | 25 | 54 | 70 | 80 | 85 |
| 1 ac | 20 | 51 | 68 | 79 | 84 |
| **Paved parking lots, roofs, driveways, etc.⁵** | 98 | 98 | 98 | 98 |
| **Streets and roads** | | | | |
| Paved with curbs and storm sewers⁵ | 98 | 98 | 98 | 98 |
| Gravel | 76 | 85 | 89 | 91 |
| Dirt | 72 | 82 | 87 | 89 |

**Footnotes:**

1. For a more detailed description of agricultural land use curve numbers, refer to the NRCS (2004a) National Engineering Handbook, Chapter 9, "Hydrologic Soil-Cover Complexes".

2. Good cover is protected from grazing and litter and brush cover soil.

3. Curve numbers are computed assuming that the runoff from the house and driveway is directed toward the street with a minimum of roof water directed to lawns where additional infiltration could occur.

4. The remaining pervious areas (lawn) are considered to be in good pasture condition for these curve numbers.

5. In some warmer climates of the country a curve number of 95 may be used.


Estimates of a soil's drying time have been discussed previously in
conjunction with both the Horton regeneration constant in Section 4.2.4
and the Green-Ampt recovery process in Section 4.3.2. It was suggested
that the drying time *T*<sub>dry</sub> in days could be related to a soil's
saturated hydraulic conductivity *K*<sub>S</sub> in in/hr as follows:

$$T_{dry} = \frac{3.125}{\sqrt{K_{s}}}$$ (4-51) 

where estimates of *K*<sub>S</sub> based on soil type can be found from Table
4-7.

## 4.6 Numerical Example

Because the four infiltration methods discussed in this chapter have
very different formulations, it is interesting to compare the results
they produce for a specific set of modeling conditions. Each method was
used to simulate infiltration over a relatively flat, completely
pervious subcatchment containing a well-drained Group B soil. The
subcatchment properties, rainfall event, and infiltration parameters for
each method are listed in Table 4-10. The infiltration parameters were
chosen to have each method produce about the same amount of runoff for
the design storm yet be within the normal ranges discussed in previous
sections of this chapter.

**Table 4-10 Parameters used in example comparison of infiltration methods**

| Item | Parameter | Value |
|------|-----------|-------|
| **Subcatchment** | Percent Impervious | 0 |
| | Percent Slope | 0.5 |
| | Width (ft) | 140 |
| | Roughness | 0.1 |
| | Depression Storage (in) | 0.05 |
| **Rainfall Event** | Duration (hr) | 6.0 |
| | Total Depth (in) | 2.0 |
| | Time-to-Peak / Duration | 0.375 |
| | Evaporation (in/hr) | 0 |
| **Horton Infiltration** | Initial Capacity (in/hr) | 1.2 |
| | Ultimate Capacity (in/hr) | 0.1 |
| | Decay Coefficient (hr⁻¹) | 2.0 |
| | Drying Time (days) | 7.0 |
| **Green-Ampt Infiltration** | Saturated Hydraulic Conductivity (in/hr) | 0.1 |
| | Suction Head (in) | 2.0 |
| | Initial Moisture Deficit | 0.2 |
| **Curve Number Infiltration** | Curve Number | 80 |
| | Drying Time (days) | 7.0 |

Figure 4-8 shows the infiltration rates obtained with each infiltration
method under these conditions. The numbers in the chart's legend are the
fraction of rainfall that becomes runoff for each method. Even though
similar amounts of runoff are produced, the methods display distinctly
different infiltration patterns over time. These patterns are influenced
not only by the parameters that were chosen for each method, but also by
the temporal pattern of rainfall intensity that occurs during an event.

<figure>
<img src="VolumeI/media/media/image24.png"
style="width:5.59453in;height:4.16725in" alt="InfilExample.png" />
<figcaption><p><span id="_Toc426447688"
class="anchor"></span><strong>Figure 4-8 Infiltration rates produced by
different methods for a 2-inch rainfall event.</strong></p></figcaption>
</figure>

(Numbers in parentheses are the fraction of rainfall that becomes
runoff.)

