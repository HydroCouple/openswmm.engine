#  Chapter 2: Meteorology

## 2.1 Precipitation

### 2.1.1 Representation

Precipitation is the principal driving force in rainfall-runoff-quality
simulation. Stormwater runoff and nonpoint source runoff quality are
directly dependent on the precipitation time series. These time series
can range from just a few time periods for a single event to thousands
of time periods used for a multi-year simulation. Within SWMM, the Rain
Gage object is used to represent a source of precipitation data. Any
number of Rain Gages may be used, data permitting, to represent spatial
variability in precipitation patterns. Precipitation data for a specific
Rain Gage is supplied either as a user-defined Time Series or through an
external data file. Several different file formats are supported for
data distributed by the U.S. National Climatic Data Center and
Environment Canada as well as a standard user-prepared format. Because
SWMM is a fully dynamic model that accounts for physical processes whose
time scales are on the order of minutes or less, SWMM should not be run
with either daily average or storm-averaged precipitation data.

Note that precipitation is often used synonymously with rainfall, but
precipitation data may also include snowfall. Because both are simply
reported as incremental intensities or depths, the SWMM program
differentiates between rainfall and snowfall by a user-supplied dividing
temperature. In natural areas, a surface temperature of 34° to 35° F
(1-2° C) provides the dividing line between equal probabilities of rain
and snow (Eagleson, 1970; Corps of Engineers, 1956). However, this
separation temperature might need to be somewhat lower in urban areas
due to warmer surface temperatures.

### 2.1.2 Single Event v. Continuous Simulation

Models might be used to aid in urban drainage design for protection
against flooding for a certain return period (e.g., five or ten years),
or to protect against pollution of receiving waters at a certain
frequency (e.g., only one combined sewer overflow per year). In these
contexts, the frequency or return period needs to be associated with a
very specific parameter. That is, for rainfall one may speak of
frequency distributions of inter-event times, total storm depth, total
storm duration or average storm intensity, all of which are different
(Eagleson, 1970, pp. 183-190). But for the aforementioned objectives,
and in fact, for almost all urban hydrology work, the frequencies of
runoff and quality parameters are required, not those of rainfall. Thus,
one may speak of the frequencies of maximum flow rate, total runoff
volume, or total pollutant loads. These distributions are in no way the
same as for similar rainfall parameters, although they may be related
through analytical methods (Howard, 1976; Chan and Bras, 1979;
Hydroscience, 1979; Adams and Papa, 2000). Finally, for pollution
control, the real interest may lie in the frequency of water quality
standards violations in the receiving water, which leads to further
complications.

SWMM is capable of simulating both single rainfall events as well as
long-term time histories (e.g. several years or more) of a continuous
precipitation record. In fact, the only distinctions between the two as
far as SWMM is concerned is the simulation duration requested by the
user and the need to supply meaningful initial conditions when only a
single event is simulated.

Continuous simulation offers an excellent, if not the only method for
obtaining the frequency of events of interest, be they related to
quantity or quality. But it has the disadvantages of a higher run time
and the need for a continuous rainfall record. This has led to the use
of a "design storm" or "design rainfall" or "design event" in a single
event simulation instead. Of course, this idea long preceded continuous
simulation, before the advent of modern computers. However, because of
inherent simplifications, the choice of a design event leads to
problems.

### 2.1.3 Temporal Rainfall Variations

The required time interval used to describe rainfall variations over
time is a function of the catchment response to rainfall input. Small,
steep, smooth, impervious catchments have fast response times, while
large, flat, pervious catchments have slower response times. As a
generality, shorter time increment data are preferable to longer time
increment data, but for a large (e.g., 10 mi<sup>2</sup> or 26 km<sup>2</sup>)
subcatchment (coarse schematization), even the hourly inputs usually
used for continuous simulation may be appropriate. Rainfall data with
intervals larger than 1-hour (such as average daily rainfall or
event-averaged rainfall) must be suitably disaggregated (Socolofsky et
al., 2001) before they can be used in SWMM.

The rain gage itself is usually the limiting factor. It is possible to
reduce data from 24-hour charts from standard 24-hour, weighing-bucket
gages to obtain 7.5-minute or 5-minute increment data, and some USGS
float gages produce no better than 5-minute values. Shorter time
increment data may usually be obtained only from tipping bucket gage
installations.

The rainfall records obtained from a gage may be of mixed quality. It
may be possible to define some storms down to 1 to 5 minute rainfall
intensities, while other events may be of such poor quality (because of
poor reproduction of charts or blurred traces of ink) that only 1-hour
increments can be obtained.

### 2.1.4 Spatial Rainfall Variations

Even for small catchments, runoff and consequent model predictions (and
prototype measurements) may be very sensitive to spatial variations of
the rainfall. For instance, thunderstorms (convective rainfall) may be
highly localized, and nearby gages may have very dissimilar readings.
For modeling accuracy (or even more specifically, for a successful
calibration of SWMM), it is essential that rain gages be located within
and adjacent to the catchment.

SWMM accounts for the spatial variability of rainfall by allowing the
user to define any number of Rain Gage objects along with their
individual data sources, and assign any rain gage to a particular SWMM
Subcatchment object (i.e., land parcel) from which runoff is computed.
If multiple gages are available, this is a much better procedure than is
the use of spatially averaged (e.g., Thiessen weighted) data, because
averaged data tend to have short-term time variations removed (i.e.,
rainfall pulses are "lowered" and "spread out"). In general, if the
rainfall is uniform spatially, as might be expected from cyclonic (e.g.,
frontal) systems, these spatial considerations are not as important. In
making this judgment, the storm size and speed in relation to the total
study area size must be considered.

Storm movement can significantly affect hydrographs computed at the
catchment outlet (Yen and Chow, 1968; Surkan, 1974; James and Drake,
1980; James and Shtifter, 1981).When more than one gage is available to
apply to the simulation, it is possible to simulate moving storms, as
rainfall in one part of the basin may be different from rainfall in
another part of the basin. Movement of a storm in the downstream
direction increases the hydrograph peak, while movement upstream tends
to level out the hydrograph (Surkan, 1974; James and Drake, 1980; James
and Shtifter, 1981).

For detailed simulation of large cities, radar rainfall data are very
useful. Commercial firms specializing in provision of radar rainfall
values may be able to place highly spatially and temporally variable
rainfall data into a time series format easily input to SWMM (e.g.,
Hoblit and Curtis, 2002; Meeneghan et al., 2002, 2003; Vallabhaneni,
2002). Radar data are spatially averaged over uniform grid cells of 1
km<sup>2</sup> or larger and therefore each cell would cover a number of runoff
subcatchments. In this case one could simply use a separate Rain Gage
object for each grid cell that overlaps the study area, and assign the
nearest cell as the subcatchment's source of rainfall data. A more
sophisticated approach is to define a separate Rain Gage for each
subcatchment along with a weighting matrix ***W*** whose entries
***w***<sub>ij</sub> represent the fraction of area from subcatchment *i* that is
contained in grid cell *j*. Then at any time *t* the vector of
subcatchment rainfalls ***I***<sub>t</sub> would equal the vector of cell
rainfall values ***R***<sub>t</sub> multiplied by the weighting matrix ***W***.
These data for each time period could be placed in a standard SWMM
user-prepared rainfall file for direct use by SWMM (see below).

## 2.2 Precipitation Data Sources

### 2.2.1 User-Supplied Data

Many SWMM analyses will rely upon rainfall data supplied by the user, on
the basis of measurements made at the closest rain gages to the
catchment, or on an assumed design storm, either "real" (that is,
derived from actual measurements) or "synthetic" (derived from an
assumed duration and temporal distribution). Construction of synthetic
design storms is described in many texts and manuals, e.g., Chow et al.
(1988), King County Department of Public Works (1995), Bedient et al.
(2013); SWMM does not supply synthetic design storms automatically,
since the emphasis is more properly on use of measured data. Measured
data may be from National Weather Service (NWS) or Environment Canada
sites, as described below, from local agencies (e.g., utilities), from
special monitoring programs (e.g., by the USGS or at a university), or
from several other sources, even from home weather stations. Naturally,
the quality of any data source should be investigated.

User-supplied rainfall data are provided to SWMM using a Rain Gage
object. The user specifies the format in which the rainfall data were
recorded (as intensity, volume, or cumulative volume), the time interval
associated with each rainfall reading (e.g., 15 minutes, 1 hour, etc.),
the source of the data (the name of a Time Series object or name of a
Rainfall file), and the ID name of the recording station or data source
if a file is being used.

For rainfall time series, only periods with non-zero precipitation need
be included in the series. Using a Time Series object for user-supplied
rainfall data makes sense for single-event or short duration simulation
periods where there are a limited number of Rain Gage objects. In fact
it is possible to create several different time series for a given rain
gage in a SWMM project, where each contains a different rainfall event
to be analyzed. Then all one needs to do is select the appropriate time
series for the scenario of interest.

If a Rainfall file is used for user-supplied rainfall data then it must
follow SWMM's standard user-prepared format. Each line of the file
contains the station ID, year, month, day, hour, minute, and non-zero
precipitation reading, each separated by one or more spaces. There is no
need to include time periods with zero readings. An excerpt from a
sample user-prepared Rainfall data file might look as follows (i.e.,
Station STA01 recorded 0.12 inches of rainfall between midnight and one
am on June 12, 2004):

| **Station** | **Year** | **Month** | **Day** | **Hour** | **Minute** | **Precipitation (in)** |
|-------------|----------|-----------|---------|----------|------------|------------------------|
| STA01       | 2004     | 6         | 12      | 00       | 00         | 0.12                   |
| STA01       | 2004     | 6         | 12      | 01       | 00         | 0.04                   |
| STA01       | 2004     | 6         | 22      | 16       | 00         | 0.07                   |


Using a Rainfall file to provide precipitation data is more convenient
when a long-term continuous simulation is being made or when there are
many rain gages in a project. Note that it is possible for a single
user-prepared Rainfall file to contain data from more than one recording
station or external data source as would be the case in the radar data
example discussed previously.

SWMM's rainfall Time Series and user-prepared Rainfall files treat the
data as "start-of-interval" values, meaning that each rainfall intensity
or depth is assumed to occur at the start of its associated date/time
value and last for a period of time equal to the gage's recording
interval. Most rainfall recording devices report their readings as
"end-of-interval" values, meaning that the time stamp associated with a
rainfall value is for the end of the recording interval. If such data
are being used to populate a SWMM rainfall time series or user-prepared
rainfall file then their date/time values should be shifted back one
recording interval to make them represent "start-of-interval" values
(e.g., for hourly rainfall, a reading with a time stamp of 10:00 am
should be entered into the time series or file as a 9:00 am value).

### 2.2.2 Data from Government Agencies

SWMM can also use rainfall data from files provided directly from US and
Canadian government agencies. The National Weather Service (NWS) makes
available historical hourly precipitation values (including water
equivalent of snowfall depths) for about 5,500 observational stations
around the U.S., with the periods of record usually beginning in the
late 1940s. Fifteen-minute data are available for over 2,400 stations,
with records typically beginning in the early 1970s. The repository for
U.S. weather data is the National Oceanic and Atmospheric Administration
(NOAA) National Climatic Data Center (NCDC), located in Asheville, North
Carolina. Key access information is provided below:

> National Climatic Data Center
>
> Climate Services Branch
>
> 151 Patton Avenue
>
> Asheville, NC 28801
>
> Telephone: 828-271-4800
>
> Web: <http://www.ncdc.noaa.gov/>

The NCDC digital data bases that house the precipitation data are
designated as DSI-3240 for hourly precipitation and DSI-3260 for
15-minute precipitation. NOAA's Climate Data Online (CDO) service at
<http://www.ncdc.noaa.gov/cdo-web> provides free access to these
archives in addition to station history information. It features an
interactive map application that helps locate a recording station
closest to a site of interest and allows one to request precipitation
data for a stipulated period of record. After a data request has been
made through CDO the user receives an email with a link to a web page
where the data can be viewed with a web browser. The page can then be
saved to file for future use with SWMM.

When requesting data from CDO be sure to specify the TEXT format option
and not the CSV option so that SWMM can automatically recognize the file
format and parse its contents. In addition, select the QPCP
precipitation option, not the QGAG option, for 15-minute precipitation
and make sure that the data flags are included.

Table 2.1 shows 15-minute precipitation data downloaded for station
410427 from Austin, Texas. The column headings represent:

| **Field** | **Description** |
|-----------|-----------------|
| **Station** | Cooperative recording station identifier |
| **Date** | Date and time at end of fifteen minute recording period |
| **QPCP** | Precipitation amount in hundredths of an inch (where 9999 or 99999 indicates a missing value) |
| **Measurement Flag** | If present, a flag that denotes either the start or end of an accumulation period, a deleted period or a missing period |
| **Quality Flag** | If present, a flag that indicates if the data value is erroneous |
| **Units** | A flag indicating the precision of the recorded value where HI is for hundredths and HT for tenths of an inch |

Hourly precipitation has a similar format except that the label 'HPCP'
(for hourly precipitation) replaces 'QPCP' and there is no Units column
since the data precision is always HT. These data sets only include
periods with non-zero precipitation, use time stamps that mark the end
of the recording interval, and use a time of '00:00' to refer to
midnight of the previous day. SWMM recognizes these conventions, as well
as missing value codes, when it reads a precipitation data file.

**Table 2-1 15-minute precipitation data from NCDC Climate Data Online**

| **STATION** | **DATE** | **QPCP** | **Measurement Flag** | **Quality Flag** | **Units** |
|-------------|----------|----------|---------------------|------------------|-----------|
| COOP:410427 | 19970729 07:45 | 10 | | | HT |
| COOP:410427 | 19970730 16:15 | 70 | | | HT |
| COOP:410427 | 19970730 16:30 | 20 | | | HT |
| COOP:410427 | 19970730 16:45 | 30 | | | HT |
| COOP:410427 | 19970730 17:00 | 50 | | | HT |
| COOP:410427 | 19970730 17:15 | 30 | | | HT |
| COOP:410427 | 19970730 17:30 | 10 | | | HT |
| COOP:410427 | 19970730 18:00 | 20 | | | HT |
| COOP:410427 | 19970730 18:15 | 20 | | | HT |
| COOP:410427 | 19970730 18:45 | 10 | | | HT |
| COOP:410427 | 19970730 19:30 | 10 | | | HT |
| COOP:410427 | 19970731 08:30 | 10 | | | HT |

The NOAA-NCDC web site also allows one to access the complete set of
hourly and 15-minute precipitation data for a particular station through
an FTP server (see <http://www.ncdc.noaa.gov/cdo-web/datasets>). For
each station, there is one file that houses data from 1948 (1971 for
15-minute data) to 1998 and then separate files for each year afterward.
Each line in these files contains one day's worth of precipitation data
using the format shown in Table 2.2. Note that the third and fourth
lines are "wrapped around" as a continuation of the long second line.
These are the same Austin, Texas data listed in Table 2.1 with the
addition of an hour '2500' entry on each line that contains the daily
total. Also these files use hour '2400' to represent midnight unlike
hour '00:00' used in the Climate Data Online format.

**Table 2-2 15-minute precipitation data in NCDC FTP file format**

| **Data Record** |
|-----------------|
| 15M41042707QPCPHT19970700290020745 00010  2500 00010 |
| 15M41042707QPCPHT19970700300111615 00070  1630 00020  1645 00030  1700 00050  1715 00030  1730 00010  1800 00020  1815 00020  1845 00010  1930 00010  2500 00270 |
| 15M41042707QPCPHT19970700310020830 00010  2500 00010 |

Earlier online data formats used by NCDC can also be recognized by SWMM.
Examples of these formats, for the 15-minute Austin, Texas data, are
shown in Tables 2.3 through 2.5. The formats for hourly data are
identical, except that HPCP replaces QPCP and time stamps are always for
hours only.

Long precipitation records are subject to meter malfunctions and missing
data (for any reason). The NWS has special codes for its DSI-3240 and
DSI-3260 formats denoting these conditions. They are explained in the
NCDC documentation for each type. SWMM will note the number of recording
periods with missing data, often denoted with a 9999 in the rainfall
column. Rainfall time series used by the subcatchment object contain
only good, non-zero precipitation data.

**Table 2-3 15-minute precipitation data in comma-delimited format**

| **COOPID** | **CD** | **ELEM** | **UN** | **YEAR** | **MO** | **DA** | **TIME** | **VALUE** | **F** | **F** |
|------------|--------|----------|--------|----------|--------|--------|----------|-----------|-------|-------|
| 410427 | 07 | QPCP | HT | 1997 | 07 | 29 | 0745 | 00010 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 29 | 2500 | 00010 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1615 | 00070 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1630 | 00020 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1645 | 00030 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1700 | 00050 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1715 | 00030 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1730 | 00010 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1800 | 00020 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1815 | 00020 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1845 | 00010 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1930 | 00010 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 2500 | 00270 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 31 | 0830 | 00010 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 31 | 2500 | 00010 | | |


**\**

**Table 2-4 15-minute precipitation data in space-delimited format**

| **COOPID** | **CD** | **ELEM** | **UN** | **YEAR** | **MO** | **DA** | **TIME** | **VALUE** | **F** | **F** |
|------------|--------|----------|--------|----------|--------|--------|----------|-----------|-------|-------|
| 410427 | 07 | QPCP | HT | 1997 | 07 | 29 | 0745 | 00010 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 29 | 2500 | 00010 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1615 | 00070 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1630 | 00020 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1645 | 00030 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1700 | 00050 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1715 | 00030 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1730 | 00010 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1800 | 00020 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1815 | 00020 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1845 | 00010 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 1930 | 00010 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 30 | 2500 | 00270 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 31 | 0830 | 00010 | | |
| 410427 | 07 | QPCP | HT | 1997 | 07 | 31 | 2500 | 00010 | | |


**Table 2-5 15-minute precipitation data in fixed-length format**

| **Data Record** |
|-----------------|
| 15M41042707QPCPHT19970700290020745 00010 |
| 15M41042707QPCPHT19970700290022500 00010 |
| 15M41042707QPCPHT19970700300111615 00070 |
| 15M41042707QPCPHT19970700300111630 00020 |
| 15M41042707QPCPHT19970700300111645 00030 |
| 15M41042707QPCPHT19970700300111700 00050 |
| 15M41042707QPCPHT19970700300111715 00030 |
| 15M41042707QPCPHT19970700300111730 00010 |
| 15M41042707QPCPHT19970700300111800 00020 |
| 15M41042707QPCPHT19970700300111815 00020 |
| 15M41042707QPCPHT19970700300111845 00010 |
| 15M41042707QPCPHT19970700300111930 00010 |
| 15M41042707QPCPHT19970700300112500 00270 |
| 15M41042707QPCPHT19970700310020830 00010 |
| 15M41042707QPCPHT19970700310022500 00010 |


SWMM can also automatically recognize and read Canadian precipitation
data that are stored in climatologic files available from Environment
Canada: (<http://www.climate.weather.gc.ca>). SWMM accepts hourly data
from HLY03 and HLY21 files and 15-minute data from FIF21 files:

(<http://climate.weather.gc.ca/prods_servs/documentation_index_e.html>).
Tables 2-6 and 2-7 show the layout of the data records in these files,
respectively. The "*ELEM*" field would contain the code 123 for
rainfall, the "*S*" field is for a numerical sign, the "*VALUE*" field
has units of 0.1 mm, and the "*F*" and "*FLG*" fields are for data
quality flags. SWMM makes the proper adjustment from "end-of-interval"
to "start-of-interval" when processing the Canadian precipitation files.
As of this writing, these files are only available through custom
requests made to Environment Canada for a fee.

**Table 2-6 Record layout of Canadian HYL0 and HLY21 hourly precipitation files**

Daily Record of Hourly Data (HLY) - Length 186

| **STN ID** | **YEAR** | **MO** | **DY** | **ELEM** | **S** | **VALUE** | **F** |
|------------|----------|--------|--------|----------|-------|-----------|-------|
|            |          |        |        |          |       |           |       |

*These fields are repeated 24 times.*

**Table 2-7 Record layout of Canadian FIF21 15-minute precipitation files**

Daily Record of 15 Minute Data (FIF) - Length 691

| **STN ID** | **YEAR** | **MO** | **DY** | **ELEM** | **S** | **VALUE** | **F** | **FLG** |
|------------|----------|--------|--------|----------|-------|-----------|-------|---------|
|            |          |        |        |          |       |           |       |         |

*These fields are repeated 96 times.*

When a SWMM rain gage object utilizes any of the standard NCDC or
Canadian formatted files, the only information required from the user is
the name of file that contains the data and a station ID. The latter
need not be the same as the station ID referenced in the file. Other
user-editable rain gage properties, such as data format, interval, and
units are overridden by the values associated with the particular data
file. SWMM will also convert the depth units used in the file to the
user's choice of unit system. For example, if an NCDC fifteen-minute
rainfall file is used in a SWMM project that employs SI metric units
then SWMM knows that the file's data must first be converted from tenths
of an inch per fifteen minute period to mm/hr before they are used for
any runoff calculations.

### 2.2.3 Rainfall Interface File

When precipitation data are supplied to SWMM from one or more external
data files, the program first collates the data from these files into a
single binary formatted Rainfall Interface file. It is this file that is
accessed during the time steps of a SWMM simulation rather than the
original rainfall data files. The Rainfall Interface file can be saved
to disk and re-used in subsequent runs should the user care to do so.
The layout of the interface file is as follows:

**File Header:**
- File stamp ("SWMM5-RAIN") (10 bytes)
- Number of SWMM rain gages in file (4-byte integer)

**For each rain gage:**
- Recording station ID (80 bytes)
- Gage recording interval (seconds) (4-byte integer)
- Starting byte of rainfall data in file (4-byte integer)
- Ending byte+1 of rainfall data in file (4-byte integer)

**For each rain gage:**
**For each time period with non-zero rainfall:**
- Date/time for start of period (8-byte double)
- Rain depth (inches) (4-byte float)


The date/time value used here represents the number of decimal days from
midnight of December 31, 1899 (i.e., the start of year 1900) expressed
as a double precision floating point number. This is the same
representation that SWMM uses internally for all date/time values.

## 2.3 Temperature Data

SWMM requires representative air temperature data when simulating snow
melt or when using the Hargreaves method to compute potential
evapotranspiration. A single set of time-dependent temperatures is
applied throughout the study area. These data can come either from a
user-generated time series or from a climate file. If a time series is
used, then linear interpolation is used to obtain temperature values for
times that fall in between those recorded in the time series. The first
recorded temperature in the series is used for dates prior to the
beginning date of the series while the last recorded temperature is used
for dates beyond the end of the series. Temperatures should be in
degrees F for SWMM projects built in US units or in degrees C for
projects built in metric units.

A SWMM climate file contains values for minimum and maximum daily
temperatures, (and optionally, evaporation and wind speed). Three
climate file formats are supported:

- the current NCDC GHCN-Daily Climate Data Online format

- the older NCDC DS3200 (aka TD-3200) format,

- Environment Canada's DLY daily climatologic file format, and

- a standard user-prepared format.

The National Climatic Data Center's Global Historical Climatology
Network - Daily (GHCN-Daily) dataset integrates daily climate
observations from approximately 30 different data sources for about
30,000 stations across the globe. As with precipitation data, NOAA's
Climate Data Online (CDO) service (<http://www.ncdc.noaa.gov/cdo-web>)
provides free access to these archives. When making an online request
for data to be used with SWMM users should do the following:

- select the "Daily Summaries" dataset

- select a range of dates to retrieve data from

- use the interactive search feature to identify the recording station
  of interest

- select the "Custom GHCN-Daily Text" output format

- do not select any of the Station Detail and Data Flag options

- select the maximum (TMAX) and minimum (TMIN) air temperature data
  types

- select the average daily wind speed (AWND) and pan evaporation rate
  (EVAP) data types if available and if so desired.

Some stations will offer 24-hour wind movement (WDMV) instead of average
daily wind speed which can be also be selected.

Table 2-8 shows the format of the data retrieved for Austin, Texas using
the steps listed above. Note that the pan evaporation has units of
tenths of millimeters, temperatures are in tenths of a degree Celsius,
and 24-hour wind movement is in kilometers. (Had average daily wind
speed (AWND) been available it would have units of tenths of meters per
second). Data fields with all 9's in them indicate missing values. SWMM
automatically makes the necessary unit conversions when reading this
type of climate file.

The DS3200 (aka TD-3200) dataset was a predecessor to the GHCN that was
discontinued in 2011. SWMM is able to read data files in this older
format, an example of which is shown in Table 2-9 for June 1997 for
Austin, Texas. Each line of the file begins with "DLY" and contains
daily data for an entire month for a specific variable; hence the lines
in the table are displayed in wrap around fashion. Table 2-10 describes
the format of the ID portion of each record while Table 2-11 does the
same for the data portion of the record.

**Table 2-8 Contents of an NCDC GHCN-Daily climate file**

| **STATION** | **DATE** | **EVAP** | **TMAX** | **TMIN** | **WDMV** |
|-------------|----------|----------|----------|----------|----------|
| GHCND:USC00410427 | 19970706 | 13 | 350 | 228 | 0.7 |
| GHCND:USC00410427 | 19970707 | 15 | 356 | 233 | 0.8 |
| GHCND:USC00410427 | 19970708 | 10 | 344 | 239 | 1.0 |
| GHCND:USC00410427 | 19970709 | 18 | 356 | 217 | 2.5 |
| GHCND:USC00410427 | 19970710 | 61 | 361 | 222 | 1.9 |
| GHCND:USC00410427 | 19970711 | 30 | 356 | 222 | 1.0 |
| GHCND:USC00410427 | 19970712 | 41 | 356 | 222 | 0.8 |

**Table 2-9 Contents of an NCDC DS3200 climate file**

| **Data Record** |
|-----------------|
| DLY41042707EVAPHI19970699990060319 00004 00419 00043 00519 00000 00619 00036 01919 00075 03019 00018 0 |
| DLY41042707TMAX F19970699990300119 00086 00219 00091 00319 00091 00419 00091 00519 00089 00619 00088 00719 00083 00819 00087 00919 00088 01019 00087 01119 00090 01219 00091 01319 00092 01419 00093 01519 00094 01619 00092 01719 00093 01819 00094)N1919 00095 02019 00092 02119 00089 02219 00085 02319 00090 02419 00090 02519 00093 02619 00092 02719 00092 02819 00094 02919 00093 03019 00096 0 |
| DLY41042707TMIN F19970699990330119 00067 00219 00055 00319 00062 00419 00063 00519 00069 00619 00068 00719 00063 00819 00067 00919 00066 01019 00068 01119 00069 01219 00072 01319 00079 01419 00077 01519 00076 01619 00074 01719 00075 01819 00070)N1919 00074 02019 00073 02119 00069 02219 00067 02319 00085 22319 00077)S2419 00082 22419 00073 S2519 00089 22519 00069)N2619 00067 02719 00072 02819 00073 02919 00080 03019 00077 0 |
| DLY41042707WDMV M19970699990300119 00027 00219 00025 00319 00017 00419 00016 00519 00022 00619 00022 00719 00018 00819 00016 00919 00020 01019 00050 01119 00022 01219 00018 01319 00053 01419 00039 01519 00037 01619 00005 01719 00051 01819 00079 01919 99999SS2019 00065A02119 00045 02219 00036 02319 00072 02419 00027 02519 00013 02619 00025 02719 00022 02819 00045 02919 00015 03019 00037 0 |

**Table 2-10 Layout of the ID portion of an NCDC DS3200 climate file record**

| **Field** | **Width** |
|-----------|-----------|
| Record Type (always = DLY) | 3 |
| Station ID | 8 |
| Element Type. Possible types used by SWMM are:<br>TMAX = daily maximum temperature, deg. F<br>TMIN = daily minimum temperature, deg. F<br>EVAP = daily evaporation, in or 1/100 in<br>WDMV = daily wind movement, miles | 4 |
| Element Measurement Units Code | 2 |
| Year | 4 |
| Month | 2 |
| Filler (= 9999) | 4 |
| Number of data portions that follow | 3 |


**Table 2-11 Layout of the data portion of an NCDC DS3200 climate file record**

| **Field** | **Width** |
|-----------|-----------|
| Day of Month | 2 |
| Hour of Observation | 2 |
| Sign of Measured Value | 1 |
| Measured Value | 5 |
| Quality Control Flag 1 | 1 |
| Quality Control Flag 2 | 1 |

*(Repeated as many times as needed to contain one month of data)*

The record layout of the Canadian DLY daily climatologic files is
depicted in Table 2-12. The "*ELEM*" field contains 001 for daily
maximum temperature and 002 for daily minimum temperature, the "*S*"
field is for a numerical sign, the "*VALUE*" field has units of 0.1 deg
C, and the "*F*" field is for a data quality flag. Note that only a
single temperature file is used containing records for both daily
maximum and daily minimum temperatures. More information on how to
obtain these files from Environment Canada can be found at
<http://www.climate.weather.gc.ca>.

**Table 2-12 Record layout of Canadian DLY daily climatologic files**

Monthly Record of Daily Data (DLY) - Length 233

| **STN ID** | **YEAR** | **MO** | **ELEM** | **S** | **VALUE** | **F** |
|------------|----------|--------|----------|-------|-----------|-------|
|            |          |        |          |       |           |       |

*These two fields are repeated 31 times.*

A user-prepared climate file is a plain text file where each line
contains the following items, each separated by one or more spaces:

- recording station name (no spaces allowed)

- 4-digit year,

- 2-digit month (Jan =1, Feb = 2, etc),

- day of the month,

- maximum temperature (deg F or C ),

- minimum temperature (deg F or C),

- evaporation rate (optional, in/day or mm/day),

- wind speed (optional, miles/hr or km/hr).

The units used for the various data items must be compatible with the
unit system being used in the SWMM project. For temperatures, this means
degrees F for US units or degrees C for metric units. If no data are
available for a given item on a particular date, then an asterisk should
be entered as its value. Table 2-13 is an example of how the contents of
the GHCN-Daily file of Table 2-1 would look in user-prepared format
under US units.

**Table 2-13 Example user-prepared climate file**

| **Station** | **Year** | **Month** | **Day** | **Max Temp** | **Min Temp** | **Evaporation** | **Wind Speed** |
|-------------|----------|-----------|---------|--------------|--------------|-----------------|----------------|
| 410427 | 1997 | 07 | 06 | 95.0 | 73.0 | 0.051 | 0.7 |
| 410427 | 1997 | 07 | 07 | 96.1 | 73.9 | 0.059 | 0.8 |
| 410427 | 1997 | 07 | 08 | 93.9 | 75.0 | 0.039 | 1.0 |
| 410427 | 1997 | 07 | 09 | 96.1 | 71.1 | 0.071 | 2.5 |
| 410427 | 1997 | 07 | 10 | 97.0 | 72.0 | 0.240 | 1.9 |
| 410427 | 1997 | 07 | 11 | 96.1 | 72.0 | 0.118 | 1.0 |
| 410427 | 1997 | 07 | 12 | 96.1 | 72.0 | 0.161 | 0.8 |


Whenever a climate file is used in SWMM the user can specify a date,
different from the simulation starting date, where the program begins
reading from. From this date on the daily values are read from the file
sequentially, without regard for what date the simulation clock is
actually at. This feature is useful if one wants to use a rainfall file
that covers one span of years and a climate file that covers another. An
error message is issued and the program terminates if this starting date
does not fall within the dates contained in the file. The same holds
true if no file start date was supplied and the simulation start date
does not fall within the dates contained in the climate file. When the
simulation reaches a date that falls outside the last date in the file,
then he program will keep using the temperature values that were last
read from the file. The same convention applies whenever there is a gap
of missing days or missing data in the file.

## 2.4 Continuous Temperature Records

When temperature data come from a climate file, a mechanism is needed to
convert the daily max-min readings into instantaneous values at any
point in time during the day. To do this, the minimum temperature is
assumed to occur at sunrise each day, and the maximum is assumed to
occur three hours prior to sunset. This scheme obviously cannot account
for many meteorological phenomena that would create other
temperature-time distributions but is apparently an appropriate one
under the circumstances. Given the max-min temperatures and their
assumed hours of occurrence, temperatures at any other time between
these are found by sinusoidal interpolation, as sketched in Figure 2-2.
The interpolation is performed, using three different periods: 1)
between the maximum of the previous day and the minimum of the present,
2) between the minimum and maximum of the present, and 3) between the
maximum of the present and minimum of the following day.

<figure>
<img src="VolumeI/media/media/image4.png"
style="width:5.98958in;height:3.125in" alt="ii_01" />
<figcaption><p><span id="_Toc426447667"
class="anchor"></span><strong>Figure 2-1 Sinusoidal interpolation of
hourly temperatures.</strong></p></figcaption>
</figure>

The time of day of sunrise and sunset are easily obtained as a function
of latitude and longitude of the catchment and the date. Techniques for
these computations are explained, for example, by List (1966) and by the
TVA (1972). Approximate (but sufficiently accurate) formulas used in
SWMM are given in the latter reference. (Snowmelt computations that
utilize temperatures are generally insensitive to these effects in
SWMM.) Their use is explained briefly below.

The hour angle of the sun, *h*, is the angular distance between the
instantaneous meridian of the sun (i.e., the meridian through which
passes a line from the center of the earth to the sun) and the meridian
of the observer (i.e., the meridian of the catchment). It may be
measured in degrees or radians or readily converted to hours, since 24
hours is equivalent to 360 degrees or 2π radians. The hour angle is a
function of latitude, declination of the earth, and time of day and is
zero at noon, true solar time, and positive in the afternoon. However,
at sunrise and sunset, the solar altitude of the sun (vertical angle of
the sun measured from the earth's surface) is zero, and the hour angle
is computed only as a function of latitude and declination,

$$cos\ h = -tan\ \delta \cdot tan\ \phi$$ (2-1)

where

  ---------------------------------------------------------------------------
  *h*   =   hour angle at sunrise or sunset, radians,
  ----- --- -----------------------------------------------------------------
  *δ*   =   earth's declination, a function of season (date), radians, and

  *φ*   =   latitude of observer, radians.

  ---------------------------------------------------------------------------

The earth's declination is provided in tables (e.g., List, 1966), but
for programming purposes an approximate formula is used (TVA, 1972):

$$\delta = \left( \frac{23.45\ \pi}{180} \right)\cos\left\lbrack \frac{2\pi}{365}(172 - D) \right\rbrack$$ (2-2)

where *D* is number of the day of the year (no leap year correction is
warranted) and *δ* is in radians. Having the latitude as an input
parameter, the hour angle is thus computed in [hours]{.underline},
positive for sunset, negative for sunrise, as

$$h = \left(\frac{12}{\pi}\right)\cos^{-1}(-tan\ \delta \cdot tan\ \phi)$$ (2-3)

The computation is valid for any latitude between the Arctic and
Antarctic Circles, and no correction is made for obstruction of the
horizon.

The hour of sunrise and sunset is symmetric about noon, true solar time.
True solar noon occurs when the sun is at its highest elevation for the
day. It differs from standard zone time, i.e., the time on clocks)
because of a longitude effect and because of the "equation of time". The
latter is of astronomical origin and causes a correction that varies
seasonally between approximately ± 15 minutes; it is neglected here. The
longitude correction accounts for the time difference due to the
separation of the meridian of the observer and the meridian of the
standard time zone. These are listed in Table 2-14. Note that time zone
boundaries are very irregular and often are quite displaced from what
might be expected on the basis of the local longitude, e.g., most of
Alaska is much further west than the standard meridian for Alaska time
of 135<sup>o</sup>W. The longitude correction is readily computed as

$$\Delta T_{LONG} = 4\frac{minutes}{degree}(\theta - SM)$$ (2-4)

where Δ*T*<sub>LONG</sub> = longitude correction, minutes (of time), *θ* =
longitude of the observer, degrees, and SM = standard meridian of the
time zone, degrees, from Table 2-14.

Note that Δ*T*<sub>LONG</sub> can be either positive or negative, and the sign
should be retained. For instance, Boston at approximately 71°W has
Δ*T*<sub>LONG</sub> = -16 minutes, meaning that mean solar noon precedes EST noon
by 16 minutes. (Mean solar time differs from true solar time by the
neglected "equation of time.")

The time of day of sunrise is then

$$H_{SR} = 12 - h + \frac{\Delta T_{LONG}}{60}$$ (2-5)

and the time of day of sunset is

$$H_{SS} = 12 + h + \frac{\Delta T_{LONG}}{60}$$ (2-6)

From these times, the hours at which the minimum (*T*<sub>min</sub>) and maximum
(*T*<sub>max</sub>) temperatures

occur are *H*<sub>min</sub> = *H*<sub>SR</sub> and *H*<sub>max</sub> = *H*<sub>SS</sub> - 3, respectively.

**Table 2-14 Time zones and standard meridians (degrees west longitude)**

| **Time Zone** | **Example Cities** | **Standard Meridian** |
|---------------|-------------------|----------------------|
| Newfoundland Std. Time | St. Johns's, Newfoundland | 52.5<sup>a</sup> |
| Atlantic Std. Time | Halifax, Nova Scotia<br>San Juan, Puerto Rico | 60 |
| Eastern Std. Time | New York, New York<br>Toronto, Ontario | 75 |
| Central Std. Time | Chicago, Illinois<br>Winnipeg, Manitoba<br>Saskatoon, Saskatchewan<sup>b</sup> | 90 |
| Mountain Std. Time | Denver, Colorado<br>Edmonton, Alberta | 105 |
| Pacific Std. Time | San Francisco, California<br>Vancouver, British Columbia<br>Whitehorse, Yukon | 120 |
| Alaska Std. Time | Anchorage, Alaska | 135 |
| Aleutian Std. Time | Atka, Alaska | 150 |
| Hawaiian Std. Time | Honolulu, Hawaii | |

<sup>a</sup>The time zone of the island of Newfoundland is offset one half hour from other zones.

<sup>b</sup>Saskatchewan summer time is Mountain, winter is Central.

The temperature *T* at any hour *H* of the day can now be computed as
follows:

1.  If *H* < *H*<sub>min</sub> then

$$T = T_{\min} + \frac{\Delta T_{1}}{2}\ sin\left( \frac{\pi\left( H_{\min} - H \right)}{H_{\min} + 24 - H_{\max}} \right)$$ (2-7)

> where Δ*T*<sub>1</sub> is the difference between the previous day's maximum
> temperature and the current day's minimum temperature.

2.  If *H*<sub>min</sub> ≤ *H* ≤ *H*<sub>max</sub> then

$$T = T_{avg} + \frac{\Delta T}{2}\ sin\left( \frac{\pi\left( H_{avg} - H \right)}{H_{\min} - H_{\max}} \right)$$ (2-8)

> where *T*<sub>avg</sub> is the average of *T*<sub>min</sub> and *T*<sub>max</sub>, Δ*T* is the
> difference between *T*<sub>max</sub> and *T*<sub>min</sub>, and *H*<sub>avg</sub> is the average
> of *H*<sub>min</sub> and *H*<sub>max</sub>.

3.  If *H* > *H*<sub>max</sub> then

$$T = T_{\max} - \frac{\Delta T}{2}\ sin\left( \frac{\pi\left( H - H_{max} \right)}{H_{\min} + 24 - H_{\max}} \right)$$ (2-9)

## 

## 2.5 Evaporation Data

Evaporation can occur in SWMM for standing water on subcatchment
surfaces, for subsurface water in groundwater aquifers, for water
flowing in open channels, for water held in storage units, and for water
held in low impact development controls (e.g., green roofs, rain
gardens, etc.). Single event simulations are usually insensitive to the
evaporation rate, but evaporation can make up a significant component of
the water budget during continuous simulation. SWMM allows evaporation
rates to be stated as:

- a single constant value,

- a set of monthly average values,

- a user-defined time series of daily values,

- daily values read from an external climate file,

- daily values computed from the daily temperatures in an external
  climate file.

Monthly and seasonal averages for evaporation are available in NOAA
(1974) and Farnsworth and Thompson (1982). Another source of evaporation
and evapotranspiration data in the U.S. is the AgriMet program of the
U.S. Bureau of Reclamation:

(<http://www.usbr.gov/pn/agrimet/proginfo.html>).

However, AgriMet is aimed primarily at agricultural use, containing
information on crop water use requirements, for instance. Generally,
local evaporation data are difficult to obtain. Fortunately, totals are
likely to represent large spatial areas more so than for precipitation.
State climate agencies are often useful when searching for weather data.
For instance, the Oregon Climate Service (<http://www.ocs.orst.edu>)
includes daily pan evaporation data among its weather archives, and
links are provided to other climate agencies regionally and nationwide.

The climate file source of evaporation data is the same climate file
used to supply daily max-min temperatures that was described in section
2.3. For NCDC GHCN-Daily files one would request that records for the
element EVAP be included in the file while for the Canadian DLY files
one would do the same for daily pan evaporation (element code 151). For
the user-supplied climate file, one simply adds an evaporation rate
value after the daily minimum temperature entry in each record. If the
file were only being used to supply evaporation and not temperatures one
still has to enter asterisks (\*) in the max and min temperature fields
so that the file is read correctly.

Note that both the NCDC and Canadian DLY files report pan evaporation
while SWMM expects actual evaporation. SWMM will accept a set of monthly
pan coefficients, typically on the order of 0.7, used to convert pan
evaporation to actual evaporation (Chow et al., 1988; Bedient et al.,
2013). Also SWMM will automatically convert the units used for
evaporation in these files into the ft/sec units used internally by
SWMM. For all other data sources, the evaporation rate values must be in
the same unit system as the rest of the data in a project. For US
standard units this is inches/day while for SI metric units it is
mm/day.

SWMM can also use the Hargreaves method (Hargreaves and Samani, 1985) to
compute evaporation rates from the daily max-min temperatures contained
in a climate file and the study area's latitude. The governing equation
is:

$$E = 0.0023\left( \frac{R_{a}}{\lambda} \right)T_{r}^{1/2}\left( T_{a} + 17.8 \right)$$ (2-10)

where:

- *E* = evaporation rate (mm/day)
- *R*<sub>a</sub> = water equivalent of incoming extraterrestrial radiation (MJm<sup>-2</sup>d<sup>-1</sup>)
- *T*<sub>r</sub> = average daily temperature range for a period of days (deg C)
- *T*<sub>a</sub> = average daily temperature for a period of days (deg C)
- *λ* = latent heat of vaporization (MJkg<sup>-1</sup>)

  $$λ = 2.50 - 0.002361T_a$$

As noted in Hargreaves and Merkley (1998), for the equation to provide
satisfactory results *T*<sub>r</sub> and *T*<sub>a</sub> must be averaged over a period of
5 or more days. SWMM therefore uses a 7-day running average of these
variables derived from the record of daily max-min temperatures. The
extraterrestrial radiation *R*<sub>a</sub> is computed as:

$$R_{a} = 37.6d_{r}\left( w_{s}sin(\phi)\ sin(\delta) + cos(\phi)\ cos(\delta)\ sin(w_{s}) \right)$$ (2-11)

where:

- *d*<sub>r</sub> = relative earth-sun distance

  $$d_r = 1 + 0.033\cos\left( \frac{2\pi J}{365} \right)$$

- *J* = Julian day (1 to 365)
- *w*<sub>s</sub> = sunset hour angle (radians)

  $$w_s = \cos^{-1}\left( - \tan(\phi)\tan(\delta) \right)$$

- *φ* = latitude (radians)
- *δ* = solar declination (radians)

  $$\delta = 0.4093\sin\left( \frac{2\pi(284 + J)}{365} \right)$$

## 2.6 Wind Speed Data

SWMM uses wind speed to refine the calculation of a melting rate for
accumulated snow during times when there is precipitation in the form of
rainfall (see Section 6.3.2). There are two options for providing wind
speed data to SWMM:

- as an average value for each month of the year (January -- December)

- from the same climate file used to supply daily max-min temperature
  and evaporation.

For the first option the same monthly average applies no matter which
year is being simulated. The wind speed units are miles/hour for US
units or km/hour for metric units. The default monthly values are all 0.
The NCDC has compiled average monthly wind speeds for various locations
throughout the US which can be found at:

[http://www.ncdc.noaa.gov/sites/default/files/attachments/wind1996.pdf](%20http:/www.ncdc.noaa.gov/sites/default/files/attachments/wind1996.pdf).

For the NCDC GHCN-Daily climate file, one can request that records for
the average daily wind speed data element AWND or the 24-hour wind
movement data element WDMV, whichever is available, be included in the
file. For the user-supplied file, wind speed is added after the field
for evaporation in each daily record (remember to place a \* in the
evaporation field if evaporation data is being supplied from some other
source). SWMM automatically converts the units used for wind speed by
the NCDC file, but for the user-supplied file they must be in miles/hour
for US unit system data sets or in km/hour for metric data sets. The
Canadian DLY file does not report daily wind speed.

