"""Type stubs for :mod:`openswmm.engine._climate`."""

from typing import List, Sequence

from ._solver import Solver


class Climate:
    def __init__(self, solver: Solver) -> None: ...

    # Temperature
    temp_source: int
    temp_timeseries: str
    temp_file_start: float
    elevation: float
    latitude: float
    longitude_correction: float

    # Evaporation
    evap_type: int
    evap_monthly: List[float]
    evap_timeseries: str
    pan_coeff: List[float]
    evap_recovery: str

    # Wind speed
    wind_type: int
    wind_monthly: List[float]

    # Snowmelt globals
    snow_temp: float
    ati_weight: float
    neg_melt_ratio: float

    # Areal-depletion curves
    adc_impervious: List[float]
    adc_pervious: List[float]

    # Monthly adjustments
    adjust_temperature: List[float]
    adjust_evaporation: List[float]
    adjust_rainfall: List[float]
    adjust_conductivity: List[float]
