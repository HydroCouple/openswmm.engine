# Appendix C SPECIALIZED PROPERTY EDITORS

## C.1 Aquifer Editor

The Aquifer Editor is invoked whenever a new aquifer object is created or an existing aquifer object is selected for editing. It contains the following data fields:

![Aquifer Editor dialog box with various properties and their values.](../../Manual/images/aquifer-editor.png)

*   **Aquifer Name**
    User-assigned aquifer name.

*   **Porosity**
    Volume of voids / total soil volume (volumetric fraction).

*   **Wilting Point**
    Volume of pore water relative to total volume for a well dried soil where only bound water remains. The moisture content of the soil cannot fall below this limit.

*   **Field Capacity**
    Volume of pore water relative to total volume after the soil has been allowed to drain fully. Below this level, vertical drainage of water through the soil layer does not occur.

*   **Conductivity**
    Soil's saturated hydraulic conductivity (in/hr or mm/hr).

*   **Conductivity Slope**
    Average slope of log(conductivity) versus soil moisture deficit (porosity minus moisture content) curve (unitless).

*   **Tension Slope**
    Average slope of soil tension versus soil moisture content curve (inches or mm).

*   **Upper Evaporation Fraction**
    Fraction of total evaporation available for evapotranspiration in the upper unsaturated zone.

*   **Lower Evaporation Depth**
    Maximum depth below the surface at which evapotranspiration from the lower saturated zone can still occur (ft or m).

*   **Lower Groundwater Loss Rate**
    Rate of percolation to deep groundwater when the water table reaches the ground surface (in/hr or mm/hr).

*   **Bottom Elevation**
    Elevation of the bottom of the aquifer (ft or m).

*   **Water Table Elevation**
    Elevation of the water table in the aquifer at the start of the simulation (ft or m).

*   **Unsaturated Zone Moisture**
    Moisture content of the unsaturated upper zone of the aquifer at the start of the simulation (volumetric fraction) (cannot exceed soil porosity).

*   **Upper Evaporation Pattern**
    Name of the monthly time pattern of adjustments applied to the upper evaporation fraction (optional – leave blank if not applicable).

## C.2 Climatology Editor

The Climatology Editor is used to enter values for various climate-related variables required by certain SWMM simulations. The dialog is divided into six tabbed pages, where each page provides a separate editor for a specific category of climate data.

### Temperature Page

![Climatology Editor - Temperature page.](../../Manual/images/climatology-temp.png)

The Temperature page of the Climatology Editor dialog is used to specify the source of temperature data used for snowmelt computations. It is also used to select a climate file as a possible source for evaporation rates. There are three choices available:

*   **No Data**
    Select this choice if snowmelt is not being simulated and evaporation rates are not based on data in a climate file.

*   **Time Series**
    Select this choice if the variation in temperature over the simulation period will be described by one of the project's time series. Also, enter (or select) the name of the time series. Click the edit button to make the Time Series Editor appear for the selected time series.

*   **External Climate File**
    Select this choice if min/max daily temperatures will be read from an external climate file (see Section 11.4). Also choose this option if you want daily evaporation rates to be estimated from daily temperatures or be read directly from the file. Then do the following:
    *   Click the folder button to search for a climate file or click the X button to clear the file name.
    *   To start reading the climate file at a particular date in time that is different than the start date of the simulation (as specified in the Simulation Options), check off the “Start Reading File at" box and enter a starting date (month/day/year) in the date entry field next to it.
    *   If using a NOAA-GHCN file, specify the temperature units used by the file.

### Evaporation Page

![Climatology Editor - Evaporation page.](../../Manual/images/climatology-evap.png)

The Evaporation page of the Climatology Editor dialog is used to supply evaporation rates, in inches/day (or mm/day), for a study area. There are five choices for specifying these rates that are selected from the **Source of Evaporation Rates** combo box:

*   **Constant Value**
    Use this choice if evaporation remains constant over time. Enter the value in the edit box provided.

*   **Time Series**
    Select this choice if evaporation rates will be specified in a time series. Enter or select the name of the time series in the dropdown combo box provided. Click the edit button to bring up the Time Series editor for the selected series. Note that for each date specified in the time series, the evaporation rate remains constant at the value supplied for that date until the next date in the series is reached (i.e., interpolation is not used on the series).

*   **Climate File**
    This choice indicates that daily evaporation rates will be read from the same climate file that was specified for temperature. Enter values for monthly pan coefficients in the data grid provided (these are used to convert pan evaporation to actual evaporation and are typically on the order of 0.7).

*   **Monthly Averages**
    Use this choice to supply an average rate for each month of the year. Enter the value for each month in the data grid provided. Note that rates remain constant within each month.

*   **Temperatures**
    The Hargreaves method will be used to compute daily evaporation rates from the daily air temperature record contained in the external climate file specified on the Temperature page of the dialog. This method also uses the site's latitude, which can be entered on the Snowmelt page of the dialog even if snow melt is not being simulated.

*   **Evaporate Only During Dry Periods:**
    Select this option if evaporation can only occur during periods with no precipitation.

In addition this page allows one to specify an optional **Monthly Soil Recovery Pattern**. This is a time pattern whose factors adjust the rate at which infiltration capacity is recovered during periods with no precipitation. It applies to all subcatchments for any choice of infiltration method. For example, if the normal infiltration recovery rate was 1% during a specific time period and a pattern factor of 0.8 applied to this period, then the actual recovery rate would be 0.8%. The Soil Recovery Pattern allows one to account for seasonal soil drying rates. In principle, the variation in pattern factors should mirror the variation in evaporation rates but might be influenced by other factors such as seasonal groundwater levels. The edit button is used to launch the Time Pattern Editor for the selected pattern.

### Wind Speed Page

![Climatology Editor - Wind Speed page.](../../Manual/images/climatology-wind.png)

The Wind Speed page of the Climatology Editor dialog is used to provide average monthly wind speeds. These are used when computing snowmelt rates under rainfall conditions. Melt rates increase with increasing wind speed. Units of wind speed are miles/hour for US units and km/hour for metric units. There are two choices for specifying wind speeds:

*   **Climate File Data**
    Wind speeds will be read from the same climate file that was specified for temperature.

*   **Monthly Averages**
    Wind speed is specified as an average value that remains constant in each month of the year. Enter a value for each month in the data grid provided. The default values are all zero.

### Snowmelt Page

![Climatology Editor - Snowmelt page.](../../Manual/images/climatology-snowmelt.png)

The Snowmelt page of the Climatology Editor dialog is used to supply values for the following parameters related to snow melt calculations:

*   **Dividing Temperature Between Snow and Rain**
    Enter the temperature below which precipitation falls as snow instead of rain. Use degrees F for US units or degrees C for metric units.

*   **ATI (Antecedent Temperature Index) Weight**
    This parameter reflects the degree to which heat transfer within a snow pack during non-melt periods is affected by prior air temperatures. Smaller values reflect a thicker surface layer of snow which results in reduced rates of heat transfer. Values must be between 0 and 1, and the default is 0.5.

*   **Negative Melt Ratio**
    This is the ratio of the heat transfer coefficient of a snow pack during non-melt conditions to the coefficient during melt conditions. It must be a number between 0 and 1. The default value is 0.6.

*   **Elevation Above MSL**
    Enter the average elevation above mean sea level for the study area, in feet or meters. This value is used to provide a more accurate estimate of atmospheric pressure. The default is 0.0, which results in a pressure of 29.9 inches Hg. The effect of wind on snow melt rates during rainfall periods is greater at higher pressures, which occur at lower elevations.

*   **Latitude**
    Enter the latitude of the study area in degrees North. This number is used when computing the hours of sunrise and sunset, which in turn are used to extend min/max daily temperatures into continuous values. It is also used to compute daily evaporation rates from daily temperatures. The default is 50 degrees North.

*   **Longitude Correction**
    This is a correction, in minutes of time, between true solar time and the standard clock time. It depends on a location's longitude (θ) and the standard meridian of its time zone (SM) through the expression 4(θ-SM). This correction is used to adjust the hours of sunrise and sunset when extending daily min/max temperatures into continuous values. The default value is 0.

### Areal Depletion Page

![Climatology Editor - Areal Depletion page.](../../Manual/images/climatology-areal.png)

The Areal Depletion page of the Climatology Editor Dialog is used to specify points on the Areal Depletion Curves for both impervious and pervious surfaces within a project's study area. These curves define the relation between the area that remains snow covered and snow pack depth. Each curve is defined by 10 equal increments of relative depth ratio between 0 and 0.9. (Relative depth ratio is the ratio of an area's current snow depth to the depth at which there is 100% areal coverage). Enter values in the data grid provided for the fraction of each area that remains snow covered at each specified relative depth ratio. Valid numbers must be between 0 and 1, and be increasing with increasing depth ratio.

Clicking the **Natural Area** button fills the grid with values that are typical of natural areas. Clicking the **No Depletion** button will fill the grid with all 1's, indicating that no areal depletion occurs. This is the default for new projects.

### Adjustments Page

![Climatology Editor - Adjustments page.](../../Manual/images/climatology-adjustments.png)

The Adjustments page of the Climatology Editor Dialog is used to supply a set of monthly adjustments applied to the temperature, evaporation rate, rainfall, and soil hydraulic conductivity that SWMM uses at each time step of a simulation:

*   The monthly **Temperature** adjustment (plus or minus in either degrees F or C) is added to the temperature value that SWMM would otherwise use in a specific month of the year.
*   The monthly **Evaporation** adjustment (plus or minus in either in/day or mm/day) is added to the evaporation rate value that SWMM would otherwise use in a specific month of the year.
*   The monthly **Rainfall** adjustment is a multiplier applied to the precipitation value that SWMM would otherwise use in a specific month of the year.
*   The monthly **Conductivity** adjustment is a multiplier applied to the soil hydraulic conductivity used compute rainfall infiltration, groundwater percolation, and exfiltration from channels and storage units.

The same adjustment is applied for each time period within a given month and is repeated for that month in each subsequent year being simulated. Leaving a monthly adjustment blank means that there is no adjustment made in that month.

## C.3 Control Rules Editor

The Control Rules Editor is invoked whenever a new control rule is created or an existing rule is selected for editing. The editor contains a memo field where the entire collection of control rules is displayed and can be edited.

![Control Rules Editor.](../../Manual/images/control-rules-editor.png)

### Control Rule Format

Each control rule is a series of statements of the form:

**RULE** ruleID
**IF** condition_1
**AND** condition_2
**OR** condition_3
**AND** condition_4
Etc.
**THEN** action_1
**AND** action_2
Etc.
**ELSE** action_3
**AND** action_4
Etc.
**PRIORITY** value

where keywords are shown in boldface and `ruleID` is an ID label assigned to the rule, `condition_n` is a Condition Clause, `action_n` is an Action Clause, and `value` is a priority value (e.g., a number from 1 to 5). The formats used for Condition and Action clauses are discussed below.

Only the **RULE**, **IF** and **THEN** portions of a rule are required; the **ELSE** and **PRIORITY** portions are optional.

Blank lines between clauses are permitted and any text to the right of a semicolon is considered a comment.

When mixing **AND** and **OR** clauses, the **OR** operator has higher precedence than **AND**, i.e.,
`IF A or B and C`
is equivalent to
`IF (A or B) and C.`
If the interpretation was meant to be
`IF A or (B and C)`
then this can be expressed using two rules as in
`IF A THEN ...`
`IF B and C THEN ...`

The **PRIORITY** value is used to determine which rule applies when two or more rules require that conflicting actions be taken on a link. A conflicting rule with a higher priority value has precedence over one with a lower value (e.g., PRIORITY 5 outranks PRIORITY 1). A rule without a priority value always has a lower priority than one with a value. For two rules with the same priority value, the rule that appears first is given the higher priority.

### Condition Clauses

A Condition Clause of a control rule has the following formats:
`object id attribute relation value`
`object id attribute relation object id attribute`

where:
*   **object** = a category of object
*   **id** = the object's ID name
*   **attribute** = an attribute or property of the object
*   **relation** = a relational operator (=, <>, <, <=, >, >=)
*   **value** = an attribute value

Some examples of condition clauses are:
```
GAGE G1 6-HR_DEPTH > 0.5
NODE N23 DEPTH > 10
NODE N23 DEPTH > NODE N25 DEPTH
PUMP P45 STATUS = OFF
SIMULATION CLOCKTIME = 22:45:00
```
The objects and attributes that can appear in a condition clause are as follows:

| Object | Attributes | Value |
| :--- | :--- | :--- |
| **GAGE** | INTENSITY<br>n-HR_DEPTH | numerical value |
| **NODE** | DEPTH<br>MAXDEPTH<br>HEAD<br>VOLUME<br>INFLOW | numerical value |
| **LINK or CONDUIT** | FLOW<br>FULLFLOW<br>DEPTH<br>MAXDEPTH<br>VELOCITY<br>LENGTH<br>SLOPE | numerical value |
| | STATUS | OPEN or CLOSED |
| | TIMEOPEN<br>TIMECLOSED | decimal hours or hr:min |
| **PUMP** | STATUS | ON or OFF |
| | SETTING | pump curve multiplier |
| | FLOW | numerical value |
| **ORIFICE** | SETTING | fraction open |
| **WEIR** | SETTING | fraction open |
| **OUTLET** | SETTING | rating curve multiplier |
| **SIMULATION** | TIME | elapsed time in decimal hours or hr:min:sec |
| | DATE | month/day/year |
| | MONTH | month of year (January = 1) |
| | DAY | day of week (Sunday = 1) |
| | CLOCKTIME | time of day in hr:min:sec |

Gage **INTENSITY** is the rainfall intensity for a specific rain gage in the current simulation time period. Gage **n-HR_DEPTH** is a gage's total rainfall depth over the past n hours where n is a number between 1 and 48.

**TIMEOPEN** is the duration a link has been in an OPEN or ON state or have its SETTING be greater than zero; **TIMECLOSED** is the duration it has remained in a CLOSED or OFF state or have its SETTING be zero. Both **TIMEOPEN** and **TIMECLOSED** apply to all link objects, including pumps, orifices, weirs, and outlets.

### Action Clauses

An Action Clause of a control rule can have one of the following formats:
`CONDUIT id STATUS = OPEN/CLOSED`
`PUMP id STATUS = ON/OFF`
`PUMP/ORIFICE/WEIR/OUTLET id SETTING = value`

where the meaning of **SETTING** depends on the object being controlled:
*   for Pumps it is a multiplier applied to the flow computed from the pump curve (for a Type5 pump curve it is a relative speed setting that shifts the curve up or down),
*   for Orifices it is the fractional amount that the orifice is fully open,
*   for Weirs it is the fractional amount of the original freeboard that exists (i.e., weir control is accomplished by moving the crest height up or down),
*   for Outlets it is a multiplier applied to the flow computed from the outlet's rating curve.

Some examples of action clauses are:
```
PUMP P67 STATUS = OFF
ORIFICE O212 SETTING = 0.5
```

### Modulated Controls

Modulated controls are control rules that provide for a continuous degree of control applied to a pump or flow regulator as determined by the value of some controller variable, such as water depth at a node, or by time. The functional relation between the control setting and the controller variable can be specified by using a Control Curve, a Time Series, or a PID Controller. Some examples of modulated control rules are:

```
RULE MC1
IF NODE N2 DEPTH >= 0
THEN WEIR W25 SETTING = CURVE C25

RULE MC2
IF SIMULATION TIME > 0
THEN PUMP P12 SETTING = TIMESERIES TS101

RULE MC3
IF LINK L33 FLOW <> 1.6
THEN ORIFICE O12 SETTING = PID 0.1 0.0 0.0
```

Note how a modified form of the action clause is used to specify the name of the control curve, time series or PID parameter set that defines the degree of control. A PID parameter set contains three values – a proportional gain coefficient, an integral time (in minutes), and a derivative time (in minutes). Also, by convention the controller variable used in a Control Curve or PID Controller will always be the object and attribute named in the last condition clause of the rule. As an example, in rule MC1 above Curve C25 would define how the fractional setting at Weir W25 varied with the water depth at Node N2. In rule MC3, the PID controller adjusts the opening of Orifice O12 to maintain a flow of 1.6 in Link L33.

### PID Controllers

A PID (Proportional-Integral-Derivative) Controller is a generic closed-loop control scheme that tries to maintain a desired set-point on some process variable by computing and applying a corrective action that adjusts the process accordingly. In the context of a hydraulic conveyance system a PID controller might be used to adjust the opening on a gated orifice to maintain a target flow rate in a specific conduit or to adjust a variable speed pump to maintain a desired depth in a storage unit. The classical PID controller has the form:

`m(t) = Kp * [e(t) + (1/Ti) * ∫e(τ)dτ + Td * (de(t)/dt)]`

where `m(t)` = controller output, `Kp` = proportional coefficient (gain), `Ti` = integral time, `Td` = derivative time, `e(t)` = error (difference between setpoint and observed variable value), and `t` = time. The performance of a PID controller is determined by the values assigned to the coefficients `Kp`, `Ti`, and `Td`.

The controller output `m(t)` has the same meaning as a link setting used in a rule's Action Clause while `dt` is the current flow routing time step in minutes. Because link settings are relative values (with respect to either a pump's standard operating curve or to the full opening height of an orifice or weir) the error `e(t)` used by the controller is also a relative value. It is defined as the difference between the control variable setpoint `x*` and its value at time `t`, `x(t)`, normalized to the setpoint value: `e(t) = (x* - x(t)) / x*`.

Note that for direct action control, where an increase in the link setting causes an increase in the controlled variable, the sign of `Kp` must be positive. For reverse action control, where the controlled variable decreases as the link setting increases, the sign of `Kp` must be negative. The user must recognize whether the control is direct or reverse action and use the proper sign on `Kp` accordingly. For example, adjusting an orifice opening to maintain a desired downstream flow is direct action. Adjusting it to maintain an upstream water level is reverse action. Controlling a pump to maintain a fixed wet well water level would be reverse action while using it to maintain a fixed downstream flow is direct action.

### Named Variables

Named Variables are aliases used to represent the triplet of `<object type | object id | object attribute>` (or a doublet for Simulation times) that appear in the condition clauses of control rules. They allow condition clauses to be written as:
`variable relation value`
`variable relation variable`

where `variable` is defined on a separate line before its first use in a rule using the format:
`VARIABLE name = object id attribute`

Here is an example of using this feature:
```
VARIABLE N123_Depth = NODE N123 DEPTH
VARIABLE N456_Depth = NODE N456 DEPTH
VARIABLE P45 = PUMP 45 STATUS

RULE 1
IF N123_Depth > N456_Depth
AND P45 = OFF
THEN PUMP 45 STATUS = ON

RULE 2
IF N123_Depth < 1
THEN PUMP 45 STATUS = OFF
```
A variable is not allowed to have the same name as an object attribute.

Aside from saving some typing, named variables are required when using arithmetic expressions in rule condition clauses.

### Arithmetic Expressions

In addition to a simple condition placed on a single variable, a control condition clause can also contain an arithmetic expression formed from several variables whose value is compared against. Thus the format of a condition clause can be extended as follows:
`expression relation value`
`expression relation variable`

where `expression` is defined on a separate line before its first use in a rule using the format:
`EXPRESSION name = f(variable1, variable2, ...)`

The function `f(...)` can be any well-formed mathematical expression containing one or more named variables as well as any of the following math functions (which are case insensitive) and operators:
*   **abs(x)** for absolute value of x
*   **sgn(x)** which is +1 for x >= 0 or -1 otherwise
*   **step(x)** which is 0 for x <= 0 and 1 otherwise
*   **sqrt(x)** for the square root of x
*   **log(x)** for logarithm base e of x
*   **log10(x)** for logarithm base 10 of x
*   **exp(x)** for e raised to the x power
*   the standard trig functions (sin, cos, tan, and cot)
*   the inverse trig functions (asin, acos, atan, and acot)
*   the hyperbolic trig functions (sinh, cosh, tanh, and coth)
*   the standard operators +, -, *, /, ^ (for exponentiation ) and any level of nested parentheses.

Here is an example of using this feature:
```
VARIABLE P1_flow = LINK 1 FLOW
VARIABLE P2_flow = LINK 2 FLOW
VARIABLE O3_flow = Link 3 FLOW
EXPRESSION Net_Inflow = (P1_flow + P2_flow)/2 - O3_flow

RULE 1
IF Net_Inflow > 0.1
THEN ORIFICE 3 SETTING = 1
ELSE ORIFICE 3 SETTING = 0.5
```

## C.4 Cross-Section Editor

The Cross-Section Editor dialog is used to specify the shape and dimensions of a conduit's cross-section.

![Cross-Section Editor dialog showing various conduit shapes.](../../Manual/images/cross-section-editor.png)

When a shape is selected from the image list an appropriate set of edit fields appears for describing the dimensions of that shape. Length dimensions are in units of feet for US units and meters for SI units. Slope values represent ratios of horizontal to vertical distance. The **Barrels** field specifies how many identical parallel conduits exist between its end nodes.

The **Force Main** shape option is a circular conduit that uses either the Hazen-Williams or Darcy-Weisbach formulas to compute friction losses for pressurized flow during Dynamic Wave flow routing. In this case the appropriate C-factor (for Hazen-Williams) or roughness height (for Darcy-Weisbach) is supplied as a cross-section property. The choice of friction loss equation is made on the Dynamic Wave Simulation Options dialog. Note that a conduit does not have to be assigned a Force Main shape for it to pressurize. Any of the other closed cross-section shapes can potentially pressurize and thus function as force mains using the Manning equation to compute friction losses.

If a **Custom** shaped section is chosen, a drop-down edit box will appear where one can enter or select the name of a Shape Curve that will be used to define the geometry of the section. This curve specifies how the width of the cross-section varies with height, where both width and height are scaled relative to the section's maximum depth. This allows the same shape curve to be used for conduits of differing sizes. Clicking the **Edit** button next to the shape curve box will bring up the Curve Editor where the shape curve's coordinates can be edited.

If a **Street** shaped section is chosen, a drop-down edit box will appear where one can enter or select the name of a Street object that describes the cross-section's geometry. Clicking the **Edit** button next to the edit box will bring up the Street Section Editor where one can edit the street's geometry.

If an **Irregular** shaped section is chosen, a drop-down edit box will appear where one can enter or select the name of a Transect object that describes the cross-section's geometry. Clicking the **Edit** button next to the edit box will bring up the Transect Editor where one can edit the transect data.

## C.5 Curve Editor

The Curve Editor dialog is invoked whenever a new curve object is created or an existing curve object is selected for editing. The editor adapts itself to the type of curve being edited (Control, Diversion, Pump, Rating, Shape, Storage, Tidal or Weir).

![Pump Curve Editor dialog.](../../Manual/images/pump-curve-editor.png)

To use the Curve Editor:

*   Enter values for the following data entry fields:
    *   **Name**: Name of the curve.
    *   **Type**: (Pump Curves Only). Choice of pump curve type as described in Section 3.2.8.
    *   **Description**: Optional comment or description of what the curve represents. Click the edit button to launch a multi-line comment editor if more than one line is needed.
    *   **Data Grid**: The curve's X,Y data.
*   Click the **View** button to see a graphical plot of the curve drawn in a separate window.
*   If additional rows are needed in the Data Grid, simply press the **Enter** key when in the last row.
*   Right-clicking over the Data Grid will make a popup Edit menu appear. It contains commands to cut, copy, insert, and paste selected cells in the grid as well as options to insert or delete a row.
*   Press **OK** to accept the curve entries or **Cancel** to cancel the edits made.

One can also click the **Load** button to load in a curve that was previously saved to file or click the **Save** button to save the current curve's data to a file.

## C.6 Groundwater Flow Editor

The Groundwater Flow Editor dialog is invoked when the Groundwater property of a subcatchment is being edited. It is used to link a subcatchment to both an aquifer and to a node of the drainage system that exchanges groundwater with the aquifer.

![Groundwater Flow Editor dialog.](../../Manual/images/groundwater-flow-editor.png)

The editor also specifies coefficients that determine the rate of lateral groundwater flow between the aquifer and the node. These coefficients (A1, A2, B1, B2, and A3) appear in the following equation that computes groundwater flow as a function of groundwater and surface water levels:

`QL = A1(Hgw - Hcb)ᴮ¹ – A2(Hsw - Hcb)ᴮ² + A3HgwHsw`

where:
*   **QL** = lateral groundwater flow (cfs per acre or cms per hectare)
*   **Hgw** = height of saturated zone above bottom of aquifer (ft or m)
*   **Hsw** = height of surface water at receiving node above aquifer bottom (ft or m)
*   **Hcb** = height of channel bottom above aquifer bottom (ft or m).

Note that QL can also be expressed in inches/hr for US units.

The rate of percolation to deep groundwater, QD, in in/hr (or mm/hr) is given by the following equation:

`QD = LGLR * (Hgw / HGS)`

where `LGLR` is the lower groundwater loss rate parameter assigned to the subcatchment's aquifer (in/hr or mm/hr) and `HGS` is the distance from the ground surface to the aquifer bottom (ft or m).

In addition to the standard lateral flow equation, the dialog allows one to define a custom equation whose results will be added onto those of the standard equation. One can also define a custom equation for deep groundwater flow that will replace the standard one. Finally, the dialog offers the option to override certain parameters that were specified for the aquifer to which the subcatchment belongs. The properties listed in the editor are as follows:

*   **Aquifer Name**: Name of the aquifer object that describes the subsurface soil properties, thickness, and initial conditions. Leave this field blank if you want the subcatchment not to generate any groundwater flow.
*   **Receiving Node**: Name of node that receives groundwater from the aquifer.
*   **Surface Elevation**: Elevation of ground surface for the subcatchment that lies above the aquifer in feet or meters.
*   **Groundwater Flow Coefficient**: Value of A1 in the groundwater flow formula.
*   **Groundwater Flow Exponent**: Value of B1 in the groundwater flow formula.
*   **Surface Water Flow Coefficient**: Value of A2 in the groundwater flow formula.
*   **Surface Water Flow Exponent**: Value of B2 in the groundwater flow formula.
*   **Surface-GW Interaction Coefficient**: Value of A3 in the groundwater flow formula.
*   **Surface Water Depth**: Fixed depth of surface water above receiving node's invert (feet or meters). Set to zero if surface water depth will vary as computed by flow routing.
*   **Threshold Water Table Elevation**: Minimum water table elevation that must be reached before any flow occurs (feet or meters). Leave blank to use the receiving node's invert elevation.
*   **Aquifer Bottom Elevation**: Elevation of the bottom of the aquifer below this particular subcatchment (feet or meters). Leave blank to use the value from the parent aquifer.
*   **Initial Water Table Elevation**: Initial water table elevation at the start of the simulation for this particular subcatchment (feet or meters). Leave blank to use the value from the parent aquifer.
*   **Unsaturated Zone Moisture**: Moisture content of the unsaturated upper zone above the water table for this particular subcatchment at the start of the simulation (volumetric fraction). Leave blank to use the value from the parent aquifer.
*   **Custom Lateral Flow Equation**: Click the ellipsis button (or press Enter) to launch the Custom Groundwater Flow Equation editor for lateral groundwater flow QL (see section C.7). The equation supplied by this editor will be used in addition to the standard equation to compute groundwater outflow from the subcatchment.
*   **Custom Deep Flow Equation**: Click the ellipsis button (or press Enter) to launch the Custom Groundwater Flow Equation editor for deep groundwater flow QD. The equation supplied by this editor will be used to replace the standard equation for deep groundwater flow.

The coefficients supplied to the groundwater flow equations must be in units that are consistent with the groundwater flow units, which can either be cfs/acre (equivalent to inches/hr) for US units or cms/ha for SI units.

> Note that elevations are used to specify the ground surface, water table height, and aquifer bottom in the dialog's data entry fields but that the groundwater flow equation uses depths above the aquifer bottom.

> If groundwater flow is simply proportional to the difference in groundwater and surface water heads, then set the Groundwater and Surface Water Flow Exponents (B1 and B2) to 1.0, set the Groundwater Flow Coefficient (A1) to the proportionality factor, set the Surface Water Flow Coefficient (A2) to the same value as A1, and set the Interaction Coefficient (A3) to zero.

> When conditions warrant, the groundwater flux can be negative, simulating flow into the aquifer from the channel, in the manner of bank storage. An exception occurs when A3 ≠ 0, since the surface water - groundwater interaction term is usually derived from groundwater flow models that assume unidirectional flow. Otherwise, to ensure that negative fluxes will not occur, one can make A1 greater than or equal to A2, B1 greater than or equal to B2, and A3 equal to zero.

> To completely replace the standard groundwater flow equation with the custom equation, set all of the standard equation coefficients to 0.

## C.7 Groundwater Equation Editor

The Groundwater Equation Editor is used to supply a custom equation for computing groundwater flow between the saturated sub-surface zone of a subcatchment and either a node in the conveyance network (lateral flow) or to a deeper groundwater aquifer (deep flow). It is invoked from the Groundwater Flow Editor form.

![Custom Groundwater Flow Equation Editor.](../../Manual/images/gw-equation-editor.png)

For lateral groundwater flow the result of evaluating the custom equation will be added onto the result of the standard equation. To replace the standard equation completely set all of its coefficients to 0. Remember that lateral groundwater flow units are cfs/acre (equivalent to inches/hr) for US units and cms/ha for metric units.

The following symbols can be used in the equation:
*   **Hgw** (for height of the groundwater table)
*   **Hsw** (for height of the surface water)
*   **Hcb** (for height of the channel bottom)
*   **Hgs** (for height of the ground surface)
*   **Phi** (for porosity of the subsurface soil)
*   **Theta** (for moisture content of the upper unsaturated zone)
*   **Ks** (for saturated hydraulic conductivity in inches/hr or mm/hr)
*   **K** (for hydraulic conductivity at the current moisture content in inches/hr or mm/hr)
*   **Fi** (for infiltration rate from the ground surface in inches/hr or mm/hr)
*   **Fu** (for percolation rate from the upper unsaturated zone in inches/hr or mm/hr)
*   **A** (for subcatchment area in acres or hectares)
where all heights are relative to the aquifer's bottom elevation in feet (or meters).

The **STEP** function can be used to have flow only when the groundwater level is above a certain threshold. For example, the expression:
`0.001 * (Hgw - 5) * STEP(Hgw - 5)`
would generate flow only when Hgw was above 5. See Section C.22 (Treatment Editor) for a list of additional math functions that can be used in a groundwater flow expression.

## C.8 Infiltration Editor

The Infiltration Editor dialog is used to specify the method and its parameters that model the rate at which rainfall infiltrates into the upper soil zone of a subcatchment's pervious area. It is invoked when editing the Infiltration property of a Subcatchment. The infiltration parameters depend on which infiltration method is selected for the subcatchment: Horton and Modified Horton, Green-Ampt and Modified Green-Ampt, or Curve Number. The infiltration method is normally the default one set by project's Simulation Options (see Section 8.1.1) or its Default Properties (see Section 5.4.2). The dialog allows one to override the default method for the subcatchment being edited.

![Infiltration Editor dialog.](../../Manual/images/infiltration-editor.png)

### Horton Infiltration Parameters

The following data fields appear in the Infiltration Editor for Horton infiltration:

*   **Max. Infil. Rate**
    Maximum infiltration rate on the Horton curve (in/hr or mm/hr). Representative values are as follows:
    *   A. DRY soils (with little or no vegetation):
        *   Sandy soils: 5 in/hr
        *   Loam soils: 3 in/hr
        *   Clay soils: 1 in/hr
    *   B. DRY soils (with dense vegetation):
        *   Multiply values in A. by 2
    *   C. MOIST soils:
        *   Soils which have drained but not dried out (i.e., field capacity):
            *   Divide values from A and B by 3.
        *   Soils close to saturation:
            *   Choose value close to minimum infiltration rate.
        *   Soils which have partially dried out:
            *   Divide values from A and B by 1.5 - 2.5.
*   **Min. Infil. Rate**
    Minimum infiltration rate on the Horton curve (in/hr or mm/hr). Equivalent to the soil's saturated hydraulic conductivity. See the Soil Characteristics Table in Section A.2 for typical values.
*   **Decay Constant**
    Infiltration rate decay constant for the Horton curve (1/hours). Typical values range between 2 and 7.
*   **Drying Time**
    Time in days for a fully saturated soil to dry completely. Typical values range from 2 to 14 days.
*   **Max. Infil. Vol.**
    Maximum infiltration volume possible (inches or mm, 0 if not applicable). It can be estimated as the difference between a soil's porosity and its wilting point times the depth of the infiltration zone.

### Green-Ampt Infiltration Parameters

The following data fields appear in the Infiltration Editor for Green-Ampt infiltration:

*   **Suction Head**
    Average value of soil capillary suction along the wetting front (inches or mm).
*   **Conductivity**
    Soil saturated hydraulic conductivity (in/hr or mm/hr).
*   **Initial Deficit**
    Fraction of soil volume that is initially dry (i.e., difference between soil porosity and initial moisture content). For a completely drained soil, it is the difference between the soil's porosity and its field capacity.

Typical values for all of these parameters can be found in the Soil Characteristics Table in Section A.2.

### Curve Number Infiltration Parameters

The following data fields appear in the Infiltration Editor for Curve Number infiltration:

*   **Curve Number**
    This is the SCS curve number which is tabulated in the publication *SCS Urban Hydrology for Small Watersheds, 2nd Ed.*, (TR-55), June 1986. Consult the Curve Number Table (Section A.4) for a listing of values by soil group, and the accompanying Soil Group Table (Section A.3) for the definitions of the various groups.
*   **Conductivity**
    This property has been deprecated and is no longer used.
*   **Drying Time**
    The number of days it takes a fully saturated soil to dry. Typical values range between 2 and 14 days.

## C.9 Inflows Editor

The Inflows Editor dialog is used to assign Direct, Dry Weather, and RDII inflow into a node of the drainage system. It is invoked whenever the Inflows property of a Node object is selected in the Property Editor. The dialog consists of three tabbed pages that provide a special editor for each type of inflow.

### Direct Inflows Page

![Inflows Editor, Direct tab.](../../Manual/images/inflows-direct.png)

The Direct page on the Inflows Editor dialog is used to specify the time history of direct external flow and water quality entering a node of the drainage system. These inflows are represented by both a constant and time varying component as follows:

`Inflow at time t = (baseline value) * (baseline pattern factor) + (scale factor) * (time series value at time t)`

The page contains the following input fields that define the properties of this relation:

*   **Constituent**
    Selects the constituent (FLOW or one of the project's specified pollutants) whose direct inflow will be described.
*   **Baseline**
    Specifies the value of the constant baseline component of the constituent's inflow. For FLOW, the units are the project's flow units. For pollutants, the units are the pollutant's concentration units if inflow is a concentration, or can be any mass flow units if the inflow is a mass flow (see Conversion Factor below). If left blank then no baseline inflow is assumed.
*   **Baseline Pattern**
    An optional Time Pattern whose factors adjust the baseline inflow on either an hourly, daily, or monthly basis (depending on the type of time pattern specified). Clicking the edit button will bring up the Time Pattern Editor dialog for the selected time pattern. If left blank, then no adjustment is made to the baseline inflow.
*   **Time Series**
    Specifies the name of the time series that contains inflow data for the selected constituent. If left blank then no direct inflow will occur for the selected constituent at the node in question. You can click the edit button to bring up the Time Series Editor dialog for the selected time series.
*   **Scale Factor**
    A multiplier used to adjust the values of the constituent's inflow time series. The baseline value is not adjusted by this factor. The scale factor can have several uses, such as allowing one to easily change the magnitude of an inflow hydrograph while keeping its shape the same, without having to re-edit the entries in the hydrograph time series. Or it can allow a group of nodes sharing the same time series to have their inflows behave in a time-synchronized fashion while letting their individual magnitudes be different. If left blank the scale factor defaults to 1.0.
*   **Inflow Type**
    For pollutants, selects the type of inflow data contained in the time series as being either a concentration (mass/volume) or mass flow rate (mass/time). This field does not appear for FLOW inflow.
*   **Units Factor**
    A numerical factor used to convert the units of pollutant mass flow rate in the time series data into concentration mass units per second. For example, if the time series data were in pounds per day and the pollutant concentration defined in the project was mg/L, then the conversion factor value would be (453,590 mg/lb) / (86400 sec/day) = 5.25 (mg/sec) per (lb/day).

More than one constituent can be edited while the dialog is active by simply selecting another choice for the Constituent property. However, if the Cancel button is clicked then any changes made to all constituents will be ignored.

> If a pollutant is assigned a direct inflow in terms of concentration, then one must also assign a direct inflow to flow, otherwise no pollutant inflow will occur. An exception is at submerged outfalls where pollutant intrusion can occur during periods of reverse flow. If pollutant inflow is defined in terms of mass, then a flow inflow time series is not required.

### Dry Weather Inflows Page

![Inflows Editor, Dry Weather tab.](../../Manual/images/inflows-dry.png)

The Dry Weather page of the Inflows Editor dialog is used to specify a continuous source of dry weather flow entering a node of the drainage system. The page contains the following input fields:

*   **Constituent**
    Selects the constituent (FLOW or one of the project's specified pollutants) whose dry weather inflow will be specified.
*   **Average Value**
    Specifies the average (or baseline) value of the dry weather inflow of the constituent in the relevant units (flow units for flow, concentration units for pollutants). Leave blank if there is no dry weather flow for the selected constituent.
*   **Time Patterns**
    Specifies the names of the time patterns to be used to allow the dry weather flow to vary in a periodic fashion by month of the year, by day of the week, and by time of day (for both weekdays and weekends). One can either type in a name or select a previously defined pattern from the dropdown list of each combo box. Up to four different types of patterns can be assigned. You can click the edit button next to each Time Pattern field to edit the respective pattern.

More than one constituent can be edited while the dialog is active by simply selecting another choice for the Constituent property. However, if the Cancel button is clicked then any changes made to all constituents will be ignored.

### RDII Inflow Page

The RDII Inflow page of the Inflows Editor dialog form is used to specify RDII (Rainfall-Dependent Infiltration and Inflow) for the node in question. The page contains the following two input fields:

*   **Unit Hydrograph Group**
    Enter (or select from the dropdown list) the name of the Unit Hydrograph group that applies to the node in question. The unit hydrographs in the group are used in combination with the group's assigned rain gage to develop a time series of RDII inflows per unit area over the period of the simulation. Leave this field blank to indicate that the node receives no RDII inflow. Clicking the edit button will launch the Unit Hydrograph Editor for the UH group specified.
*   **Sewershed Area**
    Enter the area (in acres or hectares) of the sewershed that contributes RDII to the node in question. Note this area will typically be only a small, localized portion of the subcatchment area that contributes surface runoff to the node.

## C.10 Initial Buildup Editor

The Initial Buildup Editor is invoked from the Property Editor when editing the Initial Buildup property of a subcatchment. It specifies the amount of pollutant buildup existing over the subcatchment at the start of the simulation.

![Initial Buildup Editor.](../../Manual/images/initial-buildup-editor.png)
![Initial Buildup Editor.](../../Manual/images/initial-buildup-editor2.png)

The editor consists of a data entry grid with two columns. The first column lists the name of each pollutant in the project and the second column contains edit boxes for entering the initial buildup values. If no buildup value is supplied for a pollutant, it is assumed to be 0. The units for buildup are either pounds per acre when US customary units are in use or kilograms per hectare when SI metric units are in use.

If a non-zero value is specified for the initial buildup of a pollutant, it will override any initial buildup computed from the Antecedent Dry Days parameter specified on the Dates page of the Simulation Options dialog.

## C.11 Inlet Structure Editor

The Inlet Structure Editor is invoked when a new Inlet object is created or is selected for editing. As shown below it contains an Inlet Name field used to uniquely identify the inlet structure and an Inlet Type field to select the type of structure.

![Inlet Structure Editor.](../../Manual/images/inlet-structure-editor.png)

The design parameters shown in the data entry panel depend on the choice of inlet type.

### Grate Inlet

The design parameters for a grated inlet include:

*   **Grate Type**
    Select from the choices shown in Table C-1 below.

**Table C-1 Types of grate inlets**

| Grate Type | Sketch | Description |
| :--- | :--- | :--- |
| P_BAR-50 | ![P_BAR-50 grate sketch](../../Manual/images/grate-pbar50.png) | Parallel bar grate with bar spacing 1⅞" on center |
| P_BAR-50x100 | ![P_BAR-50x100 grate sketch](../../Manual/images/grate-pbar50x100.png) | Parallel bar grate with bar spacing 1⅞" on center and ½" diameter lateral rods spaced at 4" on center |
| P_BAR-30 | ![P_BAR-30 grate sketch](../../Manual/images/grate-pbar30.png) | Parallel bar grate with 1⅛" on center bar spacing |
| CURVED_VANE | ![CURVED_VANE grate sketch](../../Manual/images/grate-curved-vane.jpg) | Curved vane grate with 3¼" longitudinal bar and 4¼" transverse bar spacing on center |
| TILT_BAR-45 | ![TILT_BAR-45 grate sketch](../../Manual/images/grate-tilt45.png) | 45 degree tilt bar grate with 2¼" longitudinal bar and 4" transverse bar spacing on center |
| TILT_BAR-30 | ![TILT_BAR-30 grate sketch](../../Manual/images/grate-tilt30.png) | 30 degree tilt bar grate with 3¼" and 4" on center longitudinal and lateral bar spacing respectively |
| RETICULINE | ![RETICULINE grate sketch](../../Manual/images/grate-reticuline.png) | "Honeycomb" pattern of lateral bars and longitudinal bearing bars |
| GENERIC | | A generic grate design. |

*   **Length**
    The grate's length parallel to the street curb (feet or meters).
*   **Width**
    The grate's width (feet or meters).
*   **Open Fraction** (for GENERIC grates only)
    The fraction of the grate's area that is open. Values are predetermined for non-Generic grates.
*   **Splash Velocity** (for GENERIC grates only)
    The minimum velocity that causes some water to shoot over the inlet thus reducing its capture efficiency (ft/sec or m/sec). Values are predetermined for non-Generic grates.

### Curb Opening Inlet

The design parameters for a curb opening inlet are:

*   **Length**
    The length of the opening (feet or meters).
*   **Height**
    The height of the opening (feet or meters).
*   **Throat Angle**
    The orientation of the curb opening's throat relative to the street surface. Choices are:
    *   **Vertical** ![Vertical throat curb opening](../../Manual/images/curb-vertical.png)
    *   **Inclined** ![Inclined throat curb opening](../../Manual/images/curb-inclined.png)
    *   **Horizontal** ![Horizontal throat curb opening](../../Manual/images/curb-horizontal.png)

### Combination Inlet

Combination inlets use the parameters for both a grate and curb opening inlet. For the curb opening, only the portion that extends beyond the length of the grate contributes to the overall capture efficiency.

### Slotted Drain Inlet

The design parameters for a slotted drain inlet are:

*   **Length**
    The drain's length parallel to the street curb (feet or meters).
*   **Width**
    The drain's width (feet or meters).

### Drop Grate Inlet

Drop grate inlets use the same parameters as a grated inlet.

### Drop Curb Inlet

Drop curb inlets use the same length and height parameters as a curb opening inlet.

### Custom Inlet

The only design parameter for a custom inlet is the name of a user-defined flow capture curve. Two options for this curve are available:

1.  a **Diversion Curve** (normally used for Divider nodes) that has captured flow be a function of the inlet's approach flow
2.  a **Rating Curve** (normally used for Outlet links) that makes the captured flow be a function of water depth.

Diversion curves are best suited for on-grade inlets and Rating curves for on-sag inlets.

![Graph of a custom inlet curve.](../../Manual/images/custom-inlet-curve.png)

Clicking the edit button next to the curve's name field will open a Curve Editor dialog.

## C.12 Inlet Usage Editor

The Inlet Usage Editor is used to place an Inlet Structure into a Street or open channel conduit. It is accessed by selecting a conduit into the Property Editor and then clicking the ellipsis button in its **Inlets** property. The following information is requested by the editor:

![Inlet Usage Editor.](../../Manual/images/inlet-usage-editor.png)

*   **Inlet Structure**
    Select the name of an inlet structure that was created with the Inlet Structure Editor (Section C.11) from the drop-down list. The list will contain only those inlets that are compatible with the conduit's cross-section (i.e., curb and gutter inlets for street sections or drop inlets for trapezoidal or rectangular channel sections). Selecting the blank first item will remove the inlet from the conduit.
*   **Capture Node**
    Enter the name of the node that receives flow captured by the inlet. You can select the node by clicking it on the Study Area Map or by selecting it from the Project Browser.
*   **Number of Inlets**
    The number of identical inlets placed in the conduit. For two-sided street conduits this number refers to pairs of inlets placed on each side of the street.
*   **Percent Clogged**
    The degree to which each inlet is clogged. For example, if a value of 40% is entered then the normal flow capture computed for the inlet is reduced by 40%.
*   **Flow Restriction**
    The maximum flow (in the project's flow units) that can be captured by a single inlet. A value of 0 indicates that flow capture is unrestricted.
*   **Depression Height**
    The height of any local gutter depression that exists over the length of the inlet (in feet or meters). A value of 0 indicates no local depression. This parameter is ignored for drop inlets.
*   **Depression Width**
    The width of any local gutter depression in feet or meters. It should be at least as large as the width that the inlet extends out into the gutter. This value is ignored if the depression height is 0 or if a drop inlet is used.
*   **Inlet Placement**
    Specifies whether the inlet is placed in an on-grade or on-sag location. Selecting **AUTOMATIC** has the program determine the placement based on the topography of the street layout.

> Grated, curb opening and slotted drain inlets can only be used by Street conduits. Drop grates and drop curb inlets can only be used by open rectangular or trapezoidal channels. Custom inlets can be used in any conduit.

## C.13 Land Use Assignment Editor

The Land Use Assignment editor is invoked from the Property Editor when editing the **Land Uses** property of a subcatchment. Its purpose is to assign land uses to the subcatchment for water quality simulations. The percent of land area in the subcatchment covered by each land use is entered next to its respective land use category. If the land use is not present its field can be left blank. The percentages entered do not necessarily have to add up to 100.

![Land Use Assignment Editor.](../../Manual/images/land-use-assignment.png)
![Land Use Assignment Editor.](../../Manual/images/land-use-assignment2.png)

## C.14 Land Use Editor

The Land Use Editor dialog is used to define a category of land use for the study area and to define its pollutant buildup and washoff characteristics.

The dialog contains three tabbed pages of land use properties:
*   **General Page** (provides land use name and street sweeping parameters)
*   **Buildup Page** (defines rate at which pollutant buildup occurs)
*   **Washoff Page** (defines rate at which pollutant washoff occurs)

### General Page

![Land Use Editor, General tab.](../../Manual/images/land-use-editor-general.png)

The General page of the Land Use Editor dialog describes the following properties of a particular land use category:

*   **Land Use Name**
    The name assigned to the land use.
*   **Description**
    An optional comment or description of the land use (click the ellipsis button or press Enter to edit).
*   **Street Sweeping Interval**
    Days between street sweeping within the land use (0 for no sweeping).
*   **Street Sweeping Availability**
    Fraction of the buildup of all pollutants that is available for removal by sweeping.
*   **Last Swept**
    Number of days since last swept at the start of the simulation.

If street sweeping does not apply to the land use, then the last three properties can be left blank.

### Buildup Page

![Land Use Editor, Buildup tab.](../../Manual/images/land-use-editor-buildup.png)

The Buildup page of the Land Use Editor dialog describes the properties associated with pollutant buildup over the land during dry weather periods. These consist of:

*   **Pollutant**
    Select the pollutant whose buildup properties are being edited.
*   **Function**
    The type of buildup function to use for the pollutant. The choices are **NONE** for no buildup, **POW** for power function buildup, **EXP** for exponential function buildup **SAT** for saturation function buildup, and **EXT** for buildup supplied by an external time series. See the discussion of Pollutant Buildup in Section 3.3.11 for explanations of these different functions. Select **NONE** if no buildup occurs.
*   **Max. Buildup**
    The maximum buildup that can occur, expressed as lbs (or kg) of the pollutant per unit of the normalizer variable (see below). This is the same as the C1 coefficient used in the buildup formulas discussed in Section 3.3.11.

The following two properties apply to the **POW**, **EXP**, and **SAT** buildup functions:

*   **Rate Constant**
    The time constant that governs the rate of pollutant buildup. This is the C2 coefficient in the Power and Exponential buildup formulas discussed in Section 3.3.11. For Power buildup its units are mass/days raised to a power, while for Exponential buildup its units are 1/days.
*   **Power/Sat. Constant**
    The exponent C3 used in the Power buildup formula, or the half-saturation constant C2 used in the Saturation buildup formula discussed in Section 3.3.11. For the latter case, its units are days.

The following two properties apply to the **EXT** (External Time Series) option:

*   **Scaling Factor**
    A multiplier used to adjust the buildup rates listed in the time series.
*   **Time Series**
    The name of the Time Series that contains buildup rates (as mass per normalizer per day).
*   **Normalizer**
    The variable to which buildup is normalized on a per unit basis. The choices are either land area (in acres or hectares) or curb length. Any units of measure can be used for curb length, as long as they remain the same for all subcatchments in the project.

When there are multiple pollutants, each pollutant must be selected separately from the Pollutant dropdown list and have its pertinent buildup properties specified.

### Washoff Page

![Land Use Editor, Washoff tab.](../../Manual/images/land-use-editor-washoff.png)

The Washoff page of the Land Use Editor dialog describes the properties associated with pollutant washoff over the land use during wet weather events. These consist of:

*   **Pollutant**
    The name of the pollutant whose washoff properties are being edited.
*   **Function**
    The choice of washoff function to use for the pollutant. The choices are:
    *   **NONE**: no washoff
    *   **EXP**: exponential washoff
    *   **RC**: rating curve washoff
    *   **EMC**: event-mean concentration washoff.
    The formula for each of these functions is discussed in Section 3.3.11 (Land Uses) under the Pollutant Washoff topic.
*   **Coefficient**
    This is the value of C1 in the exponential and rating curve formulas, or the event-mean concentration.
*   **Exponent**
    The exponent used in the exponential and rating curve washoff formulas.
*   **Cleaning Efficiency**
    The street cleaning removal efficiency (percent) for the pollutant. It represents the fraction of the amount that is available for removal on the land use as a whole (set on the General page of the editor) which is actually removed.
*   **BMP Efficiency**
    Removal efficiency (percent) associated with any Best Management Practice that might have been implemented (but is not explicitly represented in the model). The washoff load computed at each time step is simply reduced by this amount.

As with the Buildup page, each pollutant must be selected in turn from the Pollutant dropdown list and have its pertinent washoff properties defined.

## C.15 LID Control Editor

The LID Control Editor is used to define a low impact development control that can be deployed throughout a study area to store, infiltrate, and evaporate subcatchment runoff. The design of the control is made on a per-unit-area basis so that it can be placed in any number of subcatchments at different sizes or number of replicates.

![LID Control Editor.](../../Manual/images/lid-control-editor.png)

The editor contains the following data entry fields:

*   **Control Name**
    A name used to identify the particular LID control.
*   **LID Type**
    The generic type of LID being defined (bio-retention cell, rain garden, green roof, infiltration trench, permeable pavement, rain barrel, or vegetative swale).
*   **Process Layers**
    These are a tabbed set of pages containing data entry fields for the vertical layers and drain system that comprise an LID control. They include some combination of the following, depending on the type of LID selected: Surface Layer, Pavement Layer, Soil Layer, Storage Layer, and Drain System or Drainage Mat.

### Surface Layer Properties

The Surface Layer page of the LID Control Editor is used to describe the surface properties of all types of LID controls except rain barrels. Surface layer properties include:

*   **Berm Height (or Storage Depth)**
    When confining walls or berms are present this is the maximum depth to which water can pond above the surface of the unit before overflow occurs (in inches or mm). For Rooftop Disconnection it is the roof's depression storage depth, and for Vegetative Swales it is the height of the trapezoidal cross-section.
*   **Vegetative Volume Fraction**
    The fraction of the volume within the storage depth filled with vegetation. This is the volume occupied by stems and leaves, not their surface area coverage. Normally this volume can be ignored, but may be as high as 0.1 to 0.2 for very dense vegetative growth.
*   **Surface Roughness**
    Manning's roughness coefficient (n) for overland flow over surface soil cover, pavement, roof surface or vegetative swale. Use 0 for other types of LIDs.
*   **Surface Slope**
    Slope of a roof surface, pavement surface or vegetative swale (percent). Use 0 for other types of LIDs.
*   **Swale Side Slope**
    Slope (run over rise) of the side walls of a vegetative swale's cross-section. This value is ignored for other types of LIDs.

> If either Surface Roughness or Surface Slope values are 0 then any ponded water that exceeds the surface storage depth is assumed to completely overflow the LID control within a single time step.

### Pavement Layer Properties

The Pavement Layer page of the LID Control Editor supplies values for the following properties of a permeable pavement LID:

*   **Thickness**
    The thickness of the pavement layer (inches or mm). Typical values are 4 to 6 inches (100 to 150 mm).
*   **Void Ratio**
    The volume of void space relative to the volume of solids in the pavement for continuous systems or for the fill material used in modular systems. Typical values for pavements are 0.12 to 0.21. Note that porosity = void ratio / (1 + void ratio).
*   **Impervious Surface Fraction**
    Ratio of impervious paver material to total area for modular systems; 0 for continuous porous pavement systems.
*   **Permeability**
    Permeability of the concrete or asphalt used in continuous systems or hydraulic conductivity of the fill material (gravel or sand) used in modular systems (in/hr or mm/hr). In the latter case the fill's nominal conductivity should be multiplied by the fraction of the total area it covers. The permeability of new porous concrete or asphalt is very high (e.g., hundreds of in/hr) but can drop off over time due to clogging by fine particulates in the runoff (see below).
*   **Clogging Factor**
    Number of pavement layer void volumes of runoff treated it takes to completely clog the pavement. Use a value of 0 to ignore clogging. Clogging progressively reduces the pavement's permeability in direct proportion to the cumulative volume of runoff treated.
    If one has an estimate of the number of years it takes to fully clog the system (Yclog), the Clogging Factor can be computed as: `Yclog * Pa * CR * (1 + VR) * (1 - ISF) / (T * VR)` where Pa is the annual rainfall amount over the site, CR is the pavement's capture ratio (area that contributes runoff to the pavement divided by area of the pavement itself), VR is the system's Void Ratio, ISF is the Impervious Surface Fraction, and T is the pavement layer Thickness.
    As an example, suppose it takes 5 years to clog a continuous porous pavement system that serves an area where the annual rainfall is 36 inches/year. If the pavement is 6 inches thick, has a void ratio of 0.2 and captures runoff only from its own surface, then the Clogging Factor is 5 x 36 x (1 + 0.2) / 6 / 0.2 = 180.
*   **Regeneration Interval**
    The number of days that the pavement layer is allowed to clog before its permeability is restored, typically by vacuuming its surface. A value of 0 (the default) indicates that no permeability regeneration occurs.
*   **Regeneration Fraction**
    The fractional degree to which the pavement's permeability is restored when a regeneration interval is reached. The default is 0 (no restoration) while a value of 1 indicates complete restoration to the original permeability value. Once a regeneration occurs the pavement begins to clog once again at a rate determined by the Clogging Factor.

### Soil Layer properties

The Soil Layer page of the LID Control Editor describes the properties of the engineered soil mixture used in bio-retention types of LIDs and the optional sand layer beneath permeable pavement. These properties are:

*   **Thickness**
    The thickness of the soil layer (inches or mm). Typical values range from 18 to 36 inches (450 to 900 mm) for rain gardens, street planters and other types of land-based bio-retention units, but only 3 to 6 inches (75 to 150 mm) for green roofs.
*   **Porosity**
    The volume of pore space relative to total volume of soil (as a fraction).
*   **Field Capacity**
    Volume of pore water relative to total volume after the soil has been allowed to drain fully. Below this level, vertical drainage of water through the soil layer does not occur.
*   **Wilting Point**
    Volume of pore water relative to total volume for a well dried soil where only bound water remains. The moisture content of the soil cannot fall below this limit.
*   **Conductivity**
    Hydraulic conductivity for the fully saturated soil (in/hr or mm/hr).
*   **Conductivity Slope**
    Average slope of the curve of log(conductivity) versus soil moisture deficit (porosity minus moisture content) (unitless). Typical values range from 30 to 60. It can be estimated from a standard soil grain size analysis as `0.48(%Sand) + 0.85(%Clay)`.
*   **Suction Head**
    The average value of soil capillary suction along the wetting front (inches or mm). This is the same parameter as used in the Green-Ampt infiltration model.

> Porosity, field capacity, conductivity and conductivity slope are the same soil properties used for Aquifer objects when modeling groundwater, while suction head is the same parameter used for Green-Ampt infiltration. Except here they apply to the special soil mixture used in a LID unit rather than the site's naturally occurring soil. See Appendix A.2 for typical values of these properties.

### Storage Layer Properties

The Storage Layer page of the LID Control Editor describes the properties of the crushed stone or gravel layer used in bio-retention cells, permeable pavement systems, and infiltration trenches as a bottom storage/drainage layer. It is also used to specify the height of a rain barrel (or cistern). The following data fields are displayed:

*   **Thickness (or Barrel Height)**
    This is the thickness of a gravel layer or the height of a rain barrel (inches or mm). Crushed stone and gravel layers are typically 6 to 18 inches (150 to 450 mm) thick while single family home rain barrels range in height from 24 to 36 inches (600 to 900 mm).

*The following data fields do not apply to Rain Barrels.*

*   **Void Ratio**
    The volume of void space relative to the volume of solids in the layer. Typical values range from 0.5 to 0.75 for gravel beds. Note that porosity = void ratio / (1 + void ratio).
*   **Seepage Rate**
    The rate at which water seeps into the native soil below the layer (in inches/hour or mm/hour). This would typically be the Saturated Hydraulic Conductivity of the surrounding subcatchment if Green-Ampt infiltration is used or the Minimum Infiltration Rate for Horton infiltration. If there is an impermeable floor or liner below the layer then use a value of 0.
*   **Clogging Factor**
    Total volume of treated runoff it takes to completely clog the bottom of the layer divided by the void volume of the layer. Use a value of 0 to ignore clogging. Clogging progressively reduces the Infiltration Rate in direct proportion to the cumulative volume of runoff treated and may only be of concern for infiltration trenches with permeable bottoms and no under drains.

*The following data field applies only to Rain Barrels.*

*   **Covered**
    Specifies if the rain barrel is covered or not. A covered rain barrel receives no direct rainfall.

### Storage Drain Properties

LID storage layers can contain an optional drainage system that collects water entering the layer and conveys it to a conventional storm drain or other location (which can be different than the outlet of the LID's subcatchment). Drain flow can also be returned to the pervious area of the LID's subcatchment. The drain can be offset some distance above the bottom of the storage layer, to allow some volume of runoff to be stored (and eventually infiltrated) before any excess is captured by the drain. For Rooftop Disconnection, the drain system consists of the roof's gutters and downspouts that have some maximum conveyance capacity.

The Drain page of the LID Control Editor describes the properties of this system. It contains the following data entry fields:

*   **Drain Flow Coefficient and Drain Flow Exponent**
    The drain coefficient C and exponent n determines the rate of flow through a drain as a function of the height of stored water above the drain's offset. The following equation is used to compute this flow rate (per unit area of the LID unit):
    `q = C * hⁿ`
    where `q` is outflow (in/hr or mm/hr) and `h` is the height of saturated media above the drain (inches or mm). A typical value for `n` would be 0.5 (making the drain act like an orifice). Note that the units of `C` depend on the unit system being used as well as the value assigned to `n`. If the layer has no drain then set `C` to 0.
*   **Drain Offset Height**
    This is the height of the drain line above the bottom of a storage layer or rain barrel (inches or mm).
*   **Drain Delay (for Rain Barrels only)**
    The number of dry weather hours that must elapse before the drain line in a rain barrel is opened (the line is assumed to be closed once rainfall begins). A value of 0 signifies that the barrel's drain line is always open and drains continuously. This parameter is ignored for other types of LIDs.
*   **Flow Capacity (for Rooftop Disconnection only)**
    This is the maximum flow rate that the roof's gutters and downspouts can handle (in inches/hour or mm/hour) before overflowing. This is the only drain parameter used for Rooftop Disconnection.
*   **Open Level**
    The height (in inches or mm) in the drain's Storage Layer that causes the drain to automatically open when the water level rises above it. The default is 0 which means that this feature is disabled.
*   **Closed Level**
    The height (in inches or mm) in the drain's Storage Layer that causes the drain to automatically close when the water level falls below it. The default is 0.
*   **Control Curve**
    The name of an optional Control Curve that adjusts the computed drain flow as a function of the head of water above the drain. Leave blank if not applicable.

There are several things to keep in mind when specifying the parameters of an LID's underdrain:
*   If the storage layer that contains the drain has an impermeable bottom then it's best to place the drain at the bottom with a zero offset. Otherwise, to allow the full storage volume to fill before draining occurs, one would place the drain at the top of the storage layer.
*   If the storage layer has no drain then set the drain coefficient to 0.
*   If the drain can carry whatever flow enters the storage layer up to some specific limit then set the drain coefficient to the limit and the drain exponent to 0.
*   If the underdrain consists of slotted pipes where the slots act as orifices, then the drain exponent would be 0.5 and the drain coefficient would be 60,000 times the ratio of total slot area to LID area. For example, drain pipe with five 1/4" diameter holes per foot spaced 50 feet apart would have an area ratio of 0.000035 and a drain coefficient of 2.
*   If the goal is to drain a fully saturated unit in a specific amount of time then set the drain exponent to 0.5 (to represent orifice flow) and the drain coefficient to 2D¹/²/T where D is the distance from the drain to the surface plus any berm height (in inches or mm) and T is the time in hours to drain. For example, to drain a depth of 36 inches in 12 hours requires a drain coefficient of 1. If this drain consisted of the slotted pipes described in the previous bullet, whose coefficient was 2, then a flow regulator, such as a cap orifice, would have to be placed on the drain outlet to achieve the reduced flow rate.

### Drainage Mat Properties

Green Roofs usually contain a drainage mat or plate that lies below the soil media and above the roof structure. Its purpose is to convey any water that drains through the soil layer off of the roof. The Drainage Mat page of the LID Control Editor for Green Roofs lists the properties of this layer which include:

*   **Thickness**
    The thickness of the mat or plate (inches or mm). It typically ranges between 1 to 2 inches.
*   **Void Fraction**
    The ratio of void volume to total volume in the mat. It typically ranges from 0.5 to 0.6.
*   **Roughness**
    This is the Manning's roughness coefficient (n) used to compute the horizontal flow rate of drained water through the mat. It is not a standard product specification provided by manufacturers and therefore must be estimated. Previous modeling studies have suggested using a relatively high value such as from 0.1 to 0.4.

### LID Pollutant Removal

The Pollutant Removal page of the LID Control Editor allows one to specify the degree to which pollutants are removed by an LID control as seen by the flow leaving the unit through its underdrain system. Thus it only applies to LID practices that contain an underdrain (bio-retention cells, permeable pavement, infiltration trenches, and rain barrels).

The page contains a data entry grid with the project's pollutant names listed in one column and the percent removal that each receives by the LID unit in the second editable column. If a percent removal value is left blank it is assumed to be 0.

The removals specified on this page are applied to the unit's underdrain when it sends flow onto either a subcatchment or into a conveyance system node. They do not apply to any surface flow that leaves the LID unit. As an example, if the runoff treated by the LID unit had a TSS concentration of 100 mg/L and a removal percentage of 90, then if 5 cfs flowed from its drain into a conveyance system node the mass loading contribution to the node would be `100 x (1.0 – 0.9) x 5 x 28.3 L/ft3 = 1,415 mg/sec`. If in addition the unit had a surface outflow of 1 cfs into the same node, the mass loading from this flow stream would be `100 x 1 x 28.3 = 2,830 mg/sec`.

## C.16 LID Group Editor

The LID Group Editor is invoked when the LID Controls property of a Subcatchment is selected for editing. It is used to identify a group of previously defined LID controls that will be placed within the subcatchment, the sizing of each control, and what percent of runoff from the non-LID portion of the subcatchment each should treat.

![LID Controls for Subcatchment S1 dialog box.](../../Manual/images/lid-group-editor.jpg)

The editor displays the current group of LIDs placed in the subcatchment along with buttons for adding an LID unit, editing a selected unit, and deleting a selected unit. These actions can also be chosen by hitting the **Insert** key, the **Enter** key, and the **Delete** key, respectively. Selecting **Add** or **Edit** will bring up an LID Usage Editor where one can enter values for the data fields shown in the Group Editor.

Note that the total **% of Area** for all of the LID units within a subcatchment must not exceed 100%. The same applies to **% From Impervious** and **% From Pervious**. Refer to the LID Usage Editor for the meaning of these parameters.

## C.17 LID Usage Editor

The LID Usage Editor is invoked from a subcatchment's LID Group Editor to specify how a particular LID control will be deployed within the subcatchment. It contains the following data entry fields:

![LID Usage Editor.](../../Manual/images/lid-usage-editor.png)

*   **Control Name**: The name of a previously defined LID control to be used in the subcatchment.
*   **LID Occupies Full Subcatchment**: Select this checkbox option if the LID control occupies the full subcatchment (i.e., the LID is placed in its own separate subcatchment and accepts runoff from upstream subcatchments).
*   **Area of Each Unit**: The surface area devoted to each replicate LID unit (sq. ft or sq. m). If the **LID Occupies Full Subcatchment** box is checked, then this field becomes disabled and will display the total subcatchment area divided by the number of replicate units. (See Section 3.3.16 for options on placing LIDs within subcatchments.) The label below this field indicates how much of the total subcatchment area is devoted to the particular LID being deployed and gets updated as changes are made to the number of units and area of each unit.
*   **Number of Replicate Units**: The number of equal size units of the LID practice (e.g., the number of rain barrels) deployed within the subcatchment.
*   **Surface Width Per Unit**: The width of the outflow face of each identical LID unit (in ft or m). This parameter applies to roofs, pavement, trenches, and swales that use overland flow to convey surface runoff off of the unit. It can be set to 0 for other LID processes, such as bio-retention cells, rain gardens, and rain barrels that simply spill any excess captured runoff over their berms.
*   **% Initially Saturated**: For LID units with a soil layer this is the degree to which the layer is initially filled with water (0% saturation corresponds to the wilting point moisture content, 100 % saturation has the moisture content equal to the porosity). For units with a storage layer it corresponds to the initial depth of water in the layer.
*   **% of Impervious Area Treated**: The percent of the impervious portion of the subcatchment's non-LID area whose runoff is treated by the LID practice. (E.g., if rain barrels are used to capture roof runoff and roofs represent 60% of the impervious area, then the impervious area treated is 60%). If the LID unit treats only direct rainfall, such as with a green roof or roof disconnection, then this value should be 0. If the LID unit takes up the entire subcatchment then this field is ignored.
*   **% of Pervious Area Treated**: The percent of the pervious portion of the subcatchment's non-LID area whose runoff is treated by the LID practice. If the LID unit treats only direct rainfall, such as with a green roof or roof disconnection, then this value should be 0. If the LID unit takes up the entire subcatchment then this field is ignored.
*   **Send Drain Flow To**: Provide the name of the Node or Subcatchment that receives any drain flow produced by the LID unit. This field can be left blank if this flow goes to the same outlet as the LID unit's subcatchment.
*   **Return All Outflow to Pervious Area**: Select this option if outflow from the LID unit should be routed back onto the pervious area of the subcatchment that contains it. If drain outflow was selected to be routed to a different location than the subcatchment outlet then only surface outflow will be returned. Otherwise both surface and drain flow will be returned. Selecting this option would be a common choice to make for Rain Barrels, Rooftop Disconnection and possibly Green Roofs.
*   **Detailed Report File**: The name of an optional file where detailed time series results for the LID will be written. Click the browse button to select a file using the standard Windows File Save dialog or click the delete button to remove any detailed reporting. The detailed report file will be a tab delimited text file that can be easily opened and viewed with any text editor or spreadsheet program (such as Microsoft Excel) outside of SWMM.

> If the subcatchment containing the LID internally routes some portion of the impervious area runoff onto the pervious area then the percent of impervious area treated by the LID unit refers to the remaining impervious area that is not internally routed. For example, if the subcatchment has 2 acres of impervious area with runoff from 50% of this area routed onto its pervious area then an LID unit which treats 20% of the impervious area would receive runoff from 0.2 acres of impervious area. This same convention applies to the percent of pervious area treated when there is internal routing from pervious to impervious areas.

## C.18 Pollutant Editor

The Pollutant Editor is invoked when a new pollutant object is created or an existing pollutant is selected for editing. It contains the following fields:

![Pollutant Editor.](../../Manual/images/pollutant-editor.png)

*   **Name**: The name assigned to the pollutant.
*   **Units**: The concentration units (mg/L, ug/L, or #/L (counts/L)) in which the pollutant concentration is expressed.
*   **Rain Concentration**: Concentration of the pollutant in rain water (concentration units).
*   **GW Concentration**: Concentration of the pollutant in ground water (concentration units).
*   **I&I Concentration**: Concentration of the pollutant in any rainfall-dependent infiltration and inflow (concentration units).
*   **DWF Concentration**: Concentration of the pollutant in any dry weather sanitary flow (concentration units). This value can be overridden for any specific node of the conveyance system by editing the node's **Inflows** property.
*   **Initial Concentration**: Concentration of the pollutant throughout the conveyance system at the start of the simulation.
*   **Decay Coefficient**: First-order decay coefficient of the pollutant (1/days).
*   **Snow Only**: **YES** if pollutant buildup occurs only when there is snow cover, **NO** otherwise (default is **NO**).
*   **Co-Pollutant**: Name of another pollutant whose runoff concentration contributes to the runoff concentration of the current pollutant.
*   **Co-Fraction**: Fraction of the co-pollutant's runoff concentration that contributes to the runoff concentration of the current pollutant.

An example of a co-pollutant relationship would be where the runoff concentration of a particular heavy metal is some fixed fraction of the runoff concentration of suspended solids. In this case suspended solids would be declared as the co-pollutant for the heavy metal.

## C.19 Snow Pack Editor

The Snow Pack Editor is invoked when a new snow pack object is created or an existing snow pack is selected for editing. The editor contains a data entry field for the snow pack's name and two tabbed pages, one for snow pack parameters and one for snow removal parameters.

### Snow Pack Parameters Page

![Snow Pack Editor, Parameters page.](../../Manual/images/snow-pack-editor-params.png)

The Parameters page of the Snow Pack Editor dialog provides snow melt parameters and initial conditions for snow that accumulates over three different types of areas: the impervious area that is plowable (i.e., subject to snow removal), the remaining impervious area, and the entire pervious area. The page contains a data entry grid which has a column for each type of area and a row for each of the following parameters:

*   **Minimum Melt Coefficient**: The degree-day snow melt coefficient that occurs on December 21. Units are either in/hr-deg F or mm/hr-deg C.
*   **Maximum Melt Coefficient**: The degree-day snow melt coefficient that occurs on June 21. Units are either in/hr-deg F or mm/hr-deg C. For a short term simulation of less than a week or so it is acceptable to use a single value for both the minimum and maximum melt coefficients.
The minimum and maximum snow melt coefficients are used to estimate a melt coefficient that varies by day of the year. The latter is used in the following degree-day equation to compute the melt rate for any particular day:
`Melt Rate = (Melt Coefficient) * (Air Temperature – Base Temperature).`
*   **Base Temperature**: Temperature at which snow begins to melt (degrees F or C).
*   **Fraction Free Water Capacity**: The volume of a snow pack's pore space which must fill with melted snow before liquid runoff from the pack begins, expressed as a fraction of snow pack depth.
*   **Initial Snow Depth**: Depth of snow at the start of the simulation (water equivalent depth in inches or millimeters).
*   **Initial Free Water**: Depth of melted water held within the pack at the start of the simulation (inches or mm). This number should be at or below the product of the initial snow depth and the fraction free water capacity.
*   **Depth at 100% Cover**: The depth of snow beyond which the entire area remains completely covered and is not subject to any areal depletion effect (inches or mm).
*   **Fraction of Impervious Area That is Plowable**: The fraction of impervious area that is plowable and therefore is not subject to areal depletion.

### Snow Removal Parameters Page

![Snow Pack Editor, Snow Removal Parameters page.](../../Manual/images/snow-pack-editor-removal.png)

The Snow Removal page of the Snow Pack Editor describes how snow removal occurs within the Plowable area of a snow pack. The following parameters govern this process:

*   **Depth at which snow removal begins (in or mm)**: Depth which must be reached before any snow removal begins.
*   **Fraction transferred out of the watershed**: The fraction of snow depth that is removed from the system (and does not become runoff).
*   **Fraction transferred to the impervious area**: The fraction of snow depth that is added to snow accumulation on the pack's impervious area.
*   **Fraction transferred to the pervious area**: The fraction of snow depth that is added to snow accumulation on the pack's pervious area.
*   **Fraction converted to immediate melt**: The fraction of snow depth that becomes liquid water which runs onto any subcatchment associated with the snow pack.
*   **Fraction moved to another subcatchment**: The fraction of snow depth which is added to the snow accumulation on some other subcatchment. The name of the subcatchment must also be provided.

The various removal fractions must add up to 1.0 or less. If less than 1.0, then some remaining fraction of snow depth will be left on the surface after all of the redistribution options are satisfied.

## C.20 Storage Shape Editor

The Storage Shape Editor is used to describe how a storage unit's surface area varies with depth above the bottom of the unit. It is invoked when the Storage Shape property of a storage node is selected for editing (see Section B.6). There are six types of shapes one can choose from:

![Storage Shape Editor.](../../Manual/images/storage-shape-editor.png)

*   **Cylindrical**
    The storage unit has vertical sides and an elliptical base. The equation for surface area A is:
    `A = (π/4)LW`
    where L = base major axis length and W = base minor axis width. If only the surface area is known then one can use the Functional storage option instead.

*   **Conical**
    The storage unit is shaped as a truncated elliptical cone. The equation for surface area A as a function of water depth D is:
    `A = π[L(W/4) + WZD + (W/L)(ZD)²]`
    where L = base major axis length, W = base minor axis width and Z = side slope (run / rise) of a vertical slice through the major axis.

*   **Parabolic**
    The storage unit has the shape of an elliptical paraboloid. The equation for surface area A as a function of water depth D is:
    `A = (π/4)L(W/H)D`
    where L = major axis length at height H and W = minor axis width at height H. This shape can also be described using the Functional storage option.

*   **Pyramidal**
    This is for storage units shaped as a truncated rectangular pyramid or a rectangular box. The equation for surface area A as a function of water depth D is:
    `A = LW + 2(L + W)ZD + (2ZD)²`
    where L = base length, W = base width and Z = side slope (run / rise) (which would be 0 for a box).

*   **Functional**
    The following general function is used to relate surface area A to water depth D:
    `A = a0 + a1Dᵃ²`
    Where a0, a1, and a2 are user supplied coefficients. The coefficient values for some particular types of shapes are as follows:
    *   Shapes with vertical sides (such as a cylinder or rectangular prism):
        *   a0 = area of the base
        *   a1 = a2 = 0
    *   Open channel with a trapezoidal cross-section and vertical ends (i.e., a trapezoidal prism):
        *   a0 = WL
        *   a1 = 2ZL
        *   a2 = 1
        where W = bottom width of cross-section, L = channel length, and Z = side slope.
    *   Open channel with a parabolic cross-section and vertical ends:
        *   a0 = 0
        *   a1 = WLH⁰.⁵
        *   a2 = 1
        where W = top width, L = channel length and H = full height.
    *   Elliptical paraboloid:
        *   a0 = 0
        *   a1 = πLW/H
        *   a2 = 1
        where L is the length of the major axis and W the length of the minor axis at full height H.
    *   Circular non-truncated cone:
        *   a0 = 0
        *   a1 = (π/4)(W/H)²
        *   a2 = 2
        where W is the cone's diameter at height H.

*   **Tabular**
    This option uses a tabular Storage Curve to relate surface area to depth. It can represent natural depressions with irregular shaped contour intervals, spheroid storage vessels or conventional shapes with different base sizes stacked on top of one another. The first point supplied to the curve should be the surface area of the unit's base at a depth of 0. Otherwise it will be assumed that the unit has zero surface area at its base. The curve will be extrapolated outwards to meet the unit's maximum depth if need be.

For each of these options, depth is measured in feet and surface area in square feet for US units, while meters and square meters, respectively, are used for SI units.

Clicking the **Show Volume Calculator** label will display a panel where one can see what the surface area and stored volume will be for a specified water depth for the currently selected storage shape.

## C.21 Street Section Editor

The Street Section Editor is used to define the dimensions of a street or roadway cross-section. It is invoked when a new Street object is created or an existing one is selected for editing.

![Street Section Editor.](../../Manual/images/street-section-editor.png)

The editor asks that the following dimensions be provided for the portion of the street extending from the high point of the roadway to the curb and beyond to any backing that might exist:

*   **Street Section Name**: The name assigned to the street cross-section. Conduits with a STREET shape cross-section will refer to this name to identify its cross-section dimensions.
*   **Road Width (Tcrown)**: The distance from the curb to the high point of the street roadway (i.e., the street crown) (feet or meters). Traffic lanes are typically 10 to 12 feet (3.3 to 3.7 meters) wide with gutters being 1 to 3 feet (0.3 to 1 meter) wide.
*   **Curb Height (Hcurb)**: The height of the curb with respect to the street's cross slope (feet or meters). Typical heights are 0.33 to 0.67 feet (0.1 to 0.2 meters) with 0.5 feet (0.15 meters) being standard in the U.S.
*   **Cross Slope (Sx)**: The slope of the roadway portion of the cross-section (percent). Cross slopes range between 1 to 4 percent with 2 percent being a common value.
*   **Road Roughness**: Manning's roughness coefficient (n) for the road surface. Typical values range from 0.013 to 0.017.
*   **One or Two Sided**: Select One Sided if the street section extends only to the street crown or Two Sided if the same street section shape exists on the opposite side of the street crown.
*   **Gutter Depression (a)**: The distance that the gutter portion of the street is depressed below where the cross slope of the roadway would intersect the curb (feet or meters). Depressed gutter sections increase the conveyance capacity of a street. A typical value would be 0.17 feet (2 inches or 0.05 meters). Conventional gutters maintain the same slope as the roadway and would therefore have a 0 depression depth.
*   **Gutter Width (W)**: The width between the curb and the roadway for a depressed gutter (feet or meters). A typical value would be 2 feet (0.6 meters). For conventional gutters with no depression depth use a value of 0.
*   **Backing Width (Tback)**: The width of the area that the street backs up against (such as a sidewalk or lawn area) (feet or meters). Enter 0 if there is no backing.
*   **Backing Slope (Sback)**: The slope of the backing area (percent). If the backing width is non-zero then this must be a positive number. Otherwise it is ignored.
*   **Backing Roughness**: Manning's roughness coefficient (n) for the backing's surface. This parameter is ignored if the backing width is 0.

## C.22 Time Pattern Editor

The Time Pattern Editor is invoked when a new time pattern object is created or an existing time pattern is selected for editing.

![Time Pattern Editor.](../../Manual/images/time-pattern-editor.png)

The editor contains that following data entry fields:

*   **Name**: The name assigned to the time pattern.
*   **Type**: The type of time pattern being specified. The choices are Monthly, Daily, Hourly and Weekend Hourly.
*   **Description**: Provide an optional comment or description for the time pattern. If more than one line is needed, click the edit button to launch a multi-line comment editor.
*   **Multipliers**: Enter a value for each multiplier. The number and meaning of the multipliers changes with the type of time pattern selected:
    *   **MONTHLY**: One multiplier for each month of the year.
    *   **DAILY**: One multiplier for each day of the week.
    *   **HOURLY**: One multiplier for each hour from 12 midnight to 11 PM.
    *   **WEEKEND**: Same as for HOURLY except applied to weekend days.

> In order to maintain an average dry weather flow or pollutant concentration at its specified value (as entered on the Inflows Editor), the multipliers for a pattern should average to 1.0.

## C.23 Time Series Editor

The Time Series Editor is invoked whenever a new time series object is created or an existing time series is selected for editing.

![Time Series Editor.](../../Manual/images/time-series-editor.png)

To use the Time Series Editor:

1.  Enter values for the following standard items:
    *   **Name**: Name of the time series.
    *   **Description**: Optional comment or description of what the time series represents. Click the edit button to launch a multi-line comment editor if more than one line is needed.
2.  Select whether to use an external file as the source of the data or to enter the data directly into the form's data entry grid.
3.  If the external file option is selected, click the folder button to locate the file's name. The file's contents must be formatted in the same manner as the direct data entry option discussed below. See the description of Time Series Files in Section 11.6 for details.
4.  For direct data entry, enter values in the data entry grid as follows:
    *   **Date Column**: Optional date (in month/day/year format) of the time series values (only needed at points in time where a new date occurs).
    *   **Time Column**: If dates are used, enter the military time of day for each time series value (as hours:minutes or decimal hours). If dates are not used, enter time as hours since the start of the simulation.
    *   **Value Column**: The time series' numerical values.
A graphical plot of the data in the grid can be viewed in a separate window by clicking the **View** button. Right clicking over the grid will make a popup Edit menu appear. It contains commands to cut, copy, insert, and paste selected cells in the grid as well as options to insert or delete a row.
5.  Press **OK** to accept the time series or **Cancel** to cancel the edits.

> Note that there are two methods for describing the occurrence time of time series data:
> *   as calendar date/time of day (which requires that at least one date, at the start of the series, be entered in the Date column)
> *   as elapsed hours since the start of the simulation (where the Date column remains empty).

> For rainfall time series, it is only necessary to enter periods with non-zero rainfall amounts. SWMM interprets the rainfall value as a constant value lasting over the recording interval specified for the rain gage which utilizes the time series. For all other types of time series, SWMM uses interpolation to estimate values at times that fall in between the recorded values.

## C.24 Title/Notes Editor

The Title/Notes editor is invoked when a project's Title/Notes data category is selected for editing. As shown below, the editor contains a multi-line edit field where a description of a project can be entered. It also contains a check box used to indicate whether or not the first line of notes should be used as a header for printing.

![Title/Notes Editor.](../../Manual/images/title-notes-editor.png)

## C.25 Transect Editor

The Transect Editor is invoked when a new transect object is created or an existing transect is selected for editing. It contains the following data entry fields:

![Transect Editor.](../../Manual/images/transect-editor.png)

*   **Name**: The name assigned to the transect.
*   **Description**: An optional comment or description of the transect.
*   **Station/Elevation Data Grid**: Values of distance from the left side of the channel along with the corresponding elevation of the channel bottom as one moves across the channel from left to right, looking in the downstream direction. Up to 1500 data values can be entered.
*   **Roughness**: Values of Manning's roughness coefficient (n) for the left overbank, right overbank, and main channel portion of the transect. The overbank roughness values can be zero if no overbank exists.
*   **Bank Stations**: The distance values appearing in the Station/Elevation grid that mark the end of the left overbank and the start of the right overbank. Use 0 to denote the absence of an overbank.
*   **Modifiers**:
    *   The **Stations** modifier is a factor by which the distance between each station will be multiplied when the transect data is processed by SWMM. Use a value of 0 if no such factor is needed.
    *   The **Elevations** modifier is a constant value that will be added to each elevation value.
    *   The **Meander** modifier is the ratio of the length of a meandering main channel to the length of the overbank area that surrounds it. This modifier is applied to all conduits that use this particular transect for their cross-section. It assumes that the length supplied for these conduits is that of the longer main channel. SWMM will use the shorter overbank length in its calculations while increasing the main channel roughness to account for its longer length. The modifier is ignored if it is left blank or set to 0.

Right-clicking over the Data Grid will make a popup Edit menu appear. It contains commands to cut, copy, insert, and paste selected cells in the grid as well as options to insert or delete a row.

Clicking the **View** button will bring up a window that illustrates the shape of the transect cross-section.

## C.26 Treatment Editor

The Treatment Editor is invoked whenever the Treatment property of a node is selected from the Property Editor. It displays a list of the project's pollutants with an edit box next to each as shown below. Enter a valid treatment expression in the box next to each pollutant which receives treatment.

![Treatment Editor for Node 10.](../../Manual/images/treatment-editor.png)

A treatment function can be any well-formed mathematical expression involving:
*   the pollutant concentration (use the pollutant name to represent its concentration) – for non-storage nodes this is the mixture concentration of all flow streams entering the node while for storage nodes it is the pollutant concentration within the node's stored volume
*   the removals of other pollutants (use **R_** prefixed to the pollutant name to represent removal)
*   any of the following process variables:
    *   **FLOW** for flow rate into node (in user-defined flow units)
    *   **DEPTH** for water depth above node invert (ft or m)
    *   **AREA** for node surface area (ft² or m²)
    *   **DT** for routing time step (sec)
    *   **HRT** for hydraulic residence time (hours)
*   Any of the following math functions (which are case insensitive) can be used in a treatment expression:
    *   **abs(x)** for absolute value of x
    *   **sgn(x)** which is +1 for x >= 0 or -1 otherwise
    *   **step(x)** which is 0 for x <= 0 and 1 otherwise
    *   **sqrt(x)** for the square root of x
    *   **log(x)** for logarithm base e of x
    *   **log10(x)** for logarithm base 10 of x
    *   **exp(x)** for e raised to the x power
    *   the standard trig functions (sin, cos, tan, and cot)
    *   the inverse trig functions (asin, acos, atan, and acot)
    *   the hyperbolic trig functions (sinh, cosh, tanh, and coth)
    along with the standard operators +, -, *, /, ^ (for exponentiation ) and any level of nested parentheses.

> Care must be taken to avoid circular references when specifying treatment functions. For example, the expression `R = 0.75 * R_TSS` would not be computable if it were used to compute fractional removal of TSS.

## C.27 Unit Hydrograph Editor

The Unit Hydrograph Editor is invoked whenever a new unit hydrograph object is created or an existing one is selected for editing. It is used to specify the shape parameters and rain gage for a group of triangular unit hydrographs. These hydrographs are used to compute rainfall-dependent infiltration and inflow (RDII) flow at selected nodes of the drainage system.

![Unit Hydrograph Editor.](../../Manual/images/uh-editor.png)

A UH group can contain up to 12 sets of unit hydrographs (one for each month of the year), and each set can consist of up to 3 individual hydrographs (for short-term, intermediate-term, and long-term responses, respectively) as well as parameters that describe any initial abstraction losses. The editor contains the following data entry fields:

*   **Name of UH Group**: Enter the name assigned to the UH Group.
*   **Rain Gage Used**: Type in (or select from the dropdown list) the name of the rain gage that supplies rainfall data to the unit hydrographs in the group.
*   **Hydrographs For**: Select a month from the dropdown list box for which hydrograph parameters will be defined. Select **All Months** to specify a default set of hydrographs that apply to all months of the year. Then select specific months that need to have special hydrographs defined. Months listed with a (*) next to them have had hydrographs assigned to them.
*   **Unit Hydrographs**: Select this tab to provide the R-T-K shape parameters for each set of unit hydrographs in selected months of the year. The first row is used to specify parameters for a short-term response hydrograph (i.e., small value of T), the second for a medium-term response hydrograph, and the third for a long-term response hydrograph (largest value of T). It is not required that all three hydrographs be defined and the sum of the three R-values do not have to equal 1. The shape parameters for each UH consist of:
    *   **R**: the fraction of rainfall volume that enters the sewer system
    *   **T**: the time from the onset of rainfall to the peak of the UH in hours
    *   **K**: the ratio of time to recession of the UH to the time to peak
*   **Initial Abstraction Depth**: Select this tab to provide parameters that describe how rainfall will be reduced by any initial abstraction depth available (i.e., interception and depression storage) before it is processed through the unit hydrographs defined for a specific month of the year. Different initial abstraction parameters can be assigned to each of the three unit hydrograph responses. These parameters are:
    *   **Dmax**: the maximum depth of initial abstraction available (in rain depth units)
    *   **Drec**: the rate at which any utilized initial abstraction is made available again (in rain depth units per day)
    *   **Do**: the amount of initial abstraction that has already been utilized at the start of the simulation (in rain depth units).

If a grid cell is left empty its corresponding parameter value is assumed to be 0. Right-clicking over a data entry grid will make a popup Edit menu appear. It contains commands to cut, copy, and paste text to or from selected cells in the grid.