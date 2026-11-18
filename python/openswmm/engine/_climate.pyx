# SPDX-License-Identifier: Apache-2.0
#
# Copyright 2026 Caleb Buahin
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Climatology configuration (Pythonic v1 surface)
===============================================

:author: Caleb Buahin
:copyright: Copyright (c) 2026 Caleb Buahin
:license: Apache-2.0

The :class:`Climate` view, reached via ``solver.climate``, reads and edits the
model's climate *configuration* — the data parsed from the ``[TEMPERATURE]``,
``[EVAPORATION]``, ``[WINDSPEED]`` (a ``[TEMPERATURE]`` sub-keyword),
``[ADJUSTMENTS]`` sections plus the snowmelt globals and areal-depletion
curves. This complements ``solver.forcing`` (runtime climate *forcing* while a
simulation is RUNNING); the accessors here edit the *inputs* before a run.

Values use the project's display units, exactly as in the ``.inp`` file.
Setters require an editable lifecycle state (before ``initialize()``); they
raise :class:`EngineError` (lifecycle) otherwise.

.. code-block:: python

    from openswmm.engine import Solver

    with Solver("model.inp") as s:
        s.climate.evap_type = 1                       # MONTHLY
        s.climate.evap_monthly = [0.1] * 12
        s.climate.latitude = 41.5
        s.climate.adc_impervious = [1.0, 0.9, 0.8, 0.7, 0.6,
                                    0.5, 0.4, 0.3, 0.2, 0.1]
"""

# cython: language_level=3

from datetime import datetime
from operator import index
from libcpp.vector cimport vector
from ._common cimport *
from ._dates import datetime_to_oadate
from ._enums import EvapType, EvapPreviewStatus
from ._exceptions import raise_for_code


cdef int _evap_preview(SWMM_Engine h, SWMM_EvapPreviewConfig* config,
                       double start, double end, double interval,
                       SWMM_EvapPreviewSample* samples, int capacity, int* count,
                       SWMM_EvapPreviewSummary* summary, char* diagnostic) noexcept nogil:
    if config == NULL:
        return swmm_climate_preview_evaporation(h, start, end, interval,
            samples, capacity, count, summary, diagnostic, 4096)
    return swmm_climate_preview_evaporation_with_config(h, config, start, end,
        interval, samples, capacity, count, summary, diagnostic, 4096)


cdef inline SWMM_Engine _ch(solver):
    return <SWMM_Engine><size_t>solver.handle


# Function-pointer typedefs so the array get/set helpers are written once.
ctypedef int (*_arr_get_fn)(SWMM_Engine, double*, int)
ctypedef int (*_arr_set_fn)(SWMM_Engine, const double*, int)


cdef list _read_arr(SWMM_Engine h, _arr_get_fn fn, int n):
    cdef double buf[12]   # 12 covers both monthly (12) and ADC (10) tables
    _check(fn(h, buf, n))
    return [buf[i] for i in range(n)]


cdef void _write_arr(SWMM_Engine h, _arr_set_fn fn, values, int n) except *:
    cdef double buf[12]
    seq = list(values)
    if len(seq) != n:
        raise ValueError(f"expected {n} values, got {len(seq)}")
    cdef int i
    for i in range(n):
        buf[i] = <double>seq[i]
    _check(fn(h, buf, n))


class Climate:
    """Climate configuration accessor exposed as ``solver.climate``.

    Scalar attributes are plain properties; monthly tables (12 values) and
    areal-depletion curves (10 values) are list-valued properties. Enum-like
    fields (``temp_source``, ``evap_type``, ``wind_type``) are integers whose
    meanings match the C ``SWMM_TempSource`` / ``SWMM_EvapType`` /
    ``SWMM_WindType`` enums. All accessors raise :class:`EngineError` on C API
    failure.
    """

    def __init__(self, solver):
        self._solver = solver

    # -- Temperature ------------------------------------------------

    @property
    def temp_source(self) -> int:
        """Air-temperature source: 0=NONE, 1=TIMESERIES, 2=FILE."""
        cdef int v = 0
        _check(swmm_climate_get_temp_source(_ch(self._solver), &v))
        return v

    @temp_source.setter
    def temp_source(self, int value) -> None:
        _check(swmm_climate_set_temp_source(_ch(self._solver), value))

    @property
    def temp_timeseries(self) -> str:
        """Temperature time-series id (empty if none)."""
        return _read_engine_string(_ch(self._solver), swmm_climate_get_temp_timeseries)

    @temp_timeseries.setter
    def temp_timeseries(self, value) -> None:
        cdef bytes b = value.encode('utf-8')
        _check(swmm_climate_set_temp_timeseries(_ch(self._solver), b))

    @property
    def temp_file_start(self) -> float:
        """Climate-file start date (DateTime decimal days; 0 = file start)."""
        cdef double v = 0.0
        _check(swmm_climate_get_temp_file_start(_ch(self._solver), &v))
        return v

    @temp_file_start.setter
    def temp_file_start(self, double value) -> None:
        _check(swmm_climate_set_temp_file_start(_ch(self._solver), value))

    @property
    def temp_units(self) -> int:
        """Climate-file temperature units: 0=tenths-degC (C10), 1=degC, 2=degF;
        -1 = unspecified (reader's per-format default)."""
        cdef int v = 0
        _check(swmm_climate_get_temp_units(_ch(self._solver), &v))
        return v

    @temp_units.setter
    def temp_units(self, int value) -> None:
        _check(swmm_climate_set_temp_units(_ch(self._solver), value))

    @property
    def elevation(self) -> float:
        """Site elevation (project length units)."""
        cdef double v = 0.0
        _check(swmm_climate_get_elevation(_ch(self._solver), &v))
        return v

    @elevation.setter
    def elevation(self, double value) -> None:
        _check(swmm_climate_set_elevation(_ch(self._solver), value))

    @property
    def latitude(self) -> float:
        """Site latitude (degrees, -90..90)."""
        cdef double v = 0.0
        _check(swmm_climate_get_latitude(_ch(self._solver), &v))
        return v

    @latitude.setter
    def latitude(self, double value) -> None:
        _check(swmm_climate_set_latitude(_ch(self._solver), value))

    @property
    def longitude_correction(self) -> float:
        """Longitude/solar-time correction (minutes; 0 = true solar time)."""
        cdef double v = 0.0
        _check(swmm_climate_get_longitude_correction(_ch(self._solver), &v))
        return v

    @longitude_correction.setter
    def longitude_correction(self, double value) -> None:
        _check(swmm_climate_set_longitude_correction(_ch(self._solver), value))

    # -- Evaporation ------------------------------------------------

    @property
    def evap_type(self) -> int:
        """Evaporation method: 0=CONSTANT,1=MONTHLY,2=TIMESERIES,3=TEMPERATURE,4=FILE."""
        cdef int v = 0
        _check(swmm_climate_get_evap_type(_ch(self._solver), &v))
        return v

    @evap_type.setter
    def evap_type(self, int value) -> None:
        _check(swmm_climate_set_evap_type(_ch(self._solver), value))

    @property
    def evap_monthly(self) -> list:
        """Twelve monthly evaporation rates (project evap-rate units)."""
        return _read_arr(_ch(self._solver), swmm_climate_get_evap_monthly, 12)

    @evap_monthly.setter
    def evap_monthly(self, values) -> None:
        _write_arr(_ch(self._solver), swmm_climate_set_evap_monthly, values, 12)

    @property
    def evap_timeseries(self) -> str:
        """Evaporation time-series id (empty if none)."""
        return _read_engine_string(_ch(self._solver), swmm_climate_get_evap_timeseries)

    @evap_timeseries.setter
    def evap_timeseries(self, value) -> None:
        cdef bytes b = value.encode('utf-8')
        _check(swmm_climate_set_evap_timeseries(_ch(self._solver), b))

    @property
    def pan_coeff(self) -> list:
        """Twelve monthly pan coefficients (used with the FILE method)."""
        return _read_arr(_ch(self._solver), swmm_climate_get_pan_coeff, 12)

    @pan_coeff.setter
    def pan_coeff(self, values) -> None:
        _write_arr(_ch(self._solver), swmm_climate_set_pan_coeff, values, 12)

    @property
    def evap_recovery(self) -> str:
        """Soil-moisture recovery pattern id (empty if none)."""
        return _read_engine_string(_ch(self._solver), swmm_climate_get_evap_recovery)

    @evap_recovery.setter
    def evap_recovery(self, value) -> None:
        cdef bytes b = value.encode('utf-8')
        _check(swmm_climate_set_evap_recovery(_ch(self._solver), b))

    # -- Wind speed -------------------------------------------------

    @property
    def wind_type(self) -> int:
        """Wind-speed source: 0=MONTHLY, 1=FILE."""
        cdef int v = 0
        _check(swmm_climate_get_wind_type(_ch(self._solver), &v))
        return v

    @wind_type.setter
    def wind_type(self, int value) -> None:
        _check(swmm_climate_set_wind_type(_ch(self._solver), value))

    @property
    def wind_monthly(self) -> list:
        """Twelve monthly average wind speeds (project wind-speed units)."""
        return _read_arr(_ch(self._solver), swmm_climate_get_wind_monthly, 12)

    @wind_monthly.setter
    def wind_monthly(self, values) -> None:
        _write_arr(_ch(self._solver), swmm_climate_set_wind_monthly, values, 12)

    # -- Humidity ---------------------------------------------------

    @property
    def humidity_type(self) -> int:
        """Humidity source: 0=CONSTANT, 1=MONTHLY, 2=TIMESERIES."""
        cdef int v = 0
        _check(swmm_climate_get_humidity_type(_ch(self._solver), &v))
        return v

    @humidity_type.setter
    def humidity_type(self, int value) -> None:
        _check(swmm_climate_set_humidity_type(_ch(self._solver), value))

    @property
    def humidity_variable(self) -> int:
        """Humidity quantity: 0=RELATIVE (%), 1=DEWPOINT (project temperature units)."""
        cdef int v = 0
        _check(swmm_climate_get_humidity_variable(_ch(self._solver), &v))
        return v

    @humidity_variable.setter
    def humidity_variable(self, int value) -> None:
        _check(swmm_climate_set_humidity_variable(_ch(self._solver), value))

    @property
    def humidity_monthly(self) -> list:
        """Twelve monthly humidity values (RH % or dew point per ``humidity_variable``)."""
        return _read_arr(_ch(self._solver), swmm_climate_get_humidity_monthly, 12)

    @humidity_monthly.setter
    def humidity_monthly(self, values) -> None:
        _write_arr(_ch(self._solver), swmm_climate_set_humidity_monthly, values, 12)

    @property
    def humidity_timeseries(self) -> str:
        """Humidity time-series id (empty if none). Setting it selects TIMESERIES."""
        return _read_engine_string(_ch(self._solver), swmm_climate_get_humidity_timeseries)

    @humidity_timeseries.setter
    def humidity_timeseries(self, value) -> None:
        cdef bytes b = value.encode('utf-8')
        _check(swmm_climate_set_humidity_timeseries(_ch(self._solver), b))

    # -- Snowmelt globals -------------------------------------------

    @property
    def snow_temp(self) -> float:
        """Snow/rain dividing temperature (project temperature units)."""
        cdef double v = 0.0
        _check(swmm_climate_get_snow_temp(_ch(self._solver), &v))
        return v

    @snow_temp.setter
    def snow_temp(self, double value) -> None:
        _check(swmm_climate_set_snow_temp(_ch(self._solver), value))

    @property
    def ati_weight(self) -> float:
        """Antecedent-temperature-index weight (TIPM, 0..1)."""
        cdef double v = 0.0
        _check(swmm_climate_get_ati_weight(_ch(self._solver), &v))
        return v

    @ati_weight.setter
    def ati_weight(self, double value) -> None:
        _check(swmm_climate_set_ati_weight(_ch(self._solver), value))

    @property
    def neg_melt_ratio(self) -> float:
        """Negative-melt-coefficient ratio (RNM, 0..1)."""
        cdef double v = 0.0
        _check(swmm_climate_get_neg_melt_ratio(_ch(self._solver), &v))
        return v

    @neg_melt_ratio.setter
    def neg_melt_ratio(self, double value) -> None:
        _check(swmm_climate_set_neg_melt_ratio(_ch(self._solver), value))

    # -- Areal-depletion curves -------------------------------------

    @property
    def adc_impervious(self) -> list:
        """Impervious areal-depletion curve (10 fractions, 0..1)."""
        return _read_arr(_ch(self._solver), swmm_climate_get_adc_impervious, 10)

    @adc_impervious.setter
    def adc_impervious(self, values) -> None:
        _write_arr(_ch(self._solver), swmm_climate_set_adc_impervious, values, 10)

    @property
    def adc_pervious(self) -> list:
        """Pervious areal-depletion curve (10 fractions, 0..1)."""
        return _read_arr(_ch(self._solver), swmm_climate_get_adc_pervious, 10)

    @adc_pervious.setter
    def adc_pervious(self, values) -> None:
        _write_arr(_ch(self._solver), swmm_climate_set_adc_pervious, values, 10)

    # -- Monthly adjustments ----------------------------------------

    @property
    def adjust_temperature(self) -> list:
        """Twelve monthly temperature adjustment offsets (project temp units)."""
        return _read_arr(_ch(self._solver), swmm_climate_get_adjust_temperature, 12)

    @adjust_temperature.setter
    def adjust_temperature(self, values) -> None:
        _write_arr(_ch(self._solver), swmm_climate_set_adjust_temperature, values, 12)

    @property
    def adjust_evaporation(self) -> list:
        """Twelve monthly evaporation adjustment multipliers (1.0 = none)."""
        return _read_arr(_ch(self._solver), swmm_climate_get_adjust_evaporation, 12)

    @adjust_evaporation.setter
    def adjust_evaporation(self, values) -> None:
        _write_arr(_ch(self._solver), swmm_climate_set_adjust_evaporation, values, 12)

    @property
    def adjust_rainfall(self) -> list:
        """Twelve monthly rainfall adjustment multipliers (1.0 = none)."""
        return _read_arr(_ch(self._solver), swmm_climate_get_adjust_rainfall, 12)

    @adjust_rainfall.setter
    def adjust_rainfall(self, values) -> None:
        _write_arr(_ch(self._solver), swmm_climate_set_adjust_rainfall, values, 12)

    @property
    def adjust_conductivity(self) -> list:
        """Twelve monthly hydraulic-conductivity adjustment multipliers (1.0 = none)."""
        return _read_arr(_ch(self._solver), swmm_climate_get_adjust_conductivity, 12)

    @adjust_conductivity.setter
    def adjust_conductivity(self, values) -> None:
        # Values <= 0 are stored as 1.0 by the engine (legacy behaviour).
        _write_arr(_ch(self._solver), swmm_climate_set_adjust_conductivity, values, 12)

    def preview_evaporation(self, start, end, double interval_days, *, config=None):
        """Read-only evaporation preview over [start, end), in display rate units.

        Dates accept OADate floats or naive datetimes; interval_days is in days.
        A partial config mapping overrides SWMM_EvapPreviewConfig fields for this
        preview only. Monthly arrays must contain twelve values. Results contain
        samples, summary and diagnostic; live runtime forcing is excluded.
        """
        cdef SWMM_Engine handle = _ch(self._solver)
        cdef double first = datetime_to_oadate(start) if isinstance(start, datetime) else float(start)
        cdef double last = datetime_to_oadate(end) if isinstance(end, datetime) else float(end)
        cdef SWMM_EvapPreviewConfig draft
        cdef SWMM_EvapPreviewConfig* draft_ptr = NULL
        cdef SWMM_EvapPreviewSummary summary
        cdef vector[SWMM_EvapPreviewSample] samples
        cdef char diagnostic[4096]
        cdef int count = 0, rc, i
        cdef bytes evap_ts, temp_ts, climate_path
        diagnostic[0] = 0
        if config is not None:
            values = dict(config)
            known = {'evap_type', 'evap_monthly', 'pan_coeff', 'adjust_evaporation',
                     'evap_timeseries', 'temp_source', 'temp_timeseries',
                     'climate_file_path', 'climate_file_start', 'climate_file_temp_units',
                     'latitude', 'longitude_correction_minutes', 'adjust_temperature'}
            unknown = values.keys() - known
            if unknown:
                raise ValueError(f"Unknown evaporation preview fields: {sorted(unknown)}")
            absolute = _read_engine_file_path(handle, <SWMM_FilePathRole>7, b'')[0]
            draft.evap_type = index(values.get('evap_type', self.evap_type))
            draft.temp_source = index(values.get('temp_source', self.temp_source))
            draft.climate_file_start = values.get('climate_file_start', self.temp_file_start)
            draft.climate_file_temp_units = index(values.get('climate_file_temp_units', self.temp_units))
            draft.latitude = values.get('latitude', self.latitude)
            draft.longitude_correction_minutes = values.get('longitude_correction_minutes', self.longitude_correction)
            arrays = []
            for name in ('evap_monthly', 'pan_coeff', 'adjust_evaporation', 'adjust_temperature'):
                array_values = list(values.get(name, getattr(self, name)))
                if len(array_values) != 12:
                    raise ValueError(f"{name} must contain twelve values")
                arrays.append(array_values)
            for i in range(12):
                draft.evap_monthly[i] = arrays[0][i]
                draft.pan_coeff[i] = arrays[1][i]
                draft.adjust_evaporation[i] = arrays[2][i]
                draft.adjust_temperature[i] = arrays[3][i]
            strings = [values.get('evap_timeseries', self.evap_timeseries),
                       values.get('temp_timeseries', self.temp_timeseries),
                       values.get('climate_file_path', absolute)]
            if any('\0' in value for value in strings):
                raise ValueError("Preview source strings must not contain NUL")
            evap_ts, temp_ts, climate_path = [value.encode('utf-8') for value in strings]
            draft.evap_timeseries = evap_ts
            draft.temp_timeseries = temp_ts
            draft.climate_file_path = climate_path
            draft_ptr = &draft
        with self._solver._operation(<size_t>handle):
            with nogil:
                rc = _evap_preview(handle, draft_ptr, first, last, interval_days,
                                   NULL, 0, &count, &summary, diagnostic)
            if rc:
                raise_for_code(rc, (<bytes>diagnostic).decode('utf-8'))
            samples.resize(count)
            with nogil:
                rc = _evap_preview(handle, draft_ptr, first, last, interval_days,
                                   samples.data(), count, &count, &summary, diagnostic)
            if rc:
                raise_for_code(rc, (<bytes>diagnostic).decode('utf-8'))
        return dict(
            samples=tuple(dict(date=samples[i].date, source_rate=samples[i].source_rate,
                               adjustment=samples[i].adjustment, effective_rate=samples[i].effective_rate,
                               status=EvapPreviewStatus(samples[i].status)) for i in range(count)),
            summary=dict(source_type=EvapType(summary.source_type),
                         available_count=summary.available_count, held_count=summary.held_count,
                         missing_count=summary.missing_count, estimated_count=summary.estimated_count,
                         runtime_forcing_unknown=bool(summary.runtime_forcing_unknown),
                         minimum_rate=summary.minimum_rate, maximum_rate=summary.maximum_rate),
            diagnostic=(<bytes>diagnostic).decode('utf-8'))
