"""
SWMM DateTime <-> :class:`datetime.datetime` conversion helpers.

SWMM's native DateTime is a floating-point value where the integer part is
the number of days since 1899-12-30 (the OLE Automation epoch SWMM uses)
and the fractional part is the time-of-day fraction (0.5 = noon).

The calendar arithmetic is delegated to the engine's C API
(``openswmm_datetime.h``, surfaced via :mod:`openswmm.engine._datetime`) so
the same epoch math used inside the simulator drives the Python helpers.
Sub-second precision is preserved by handling the microseconds component
in Python on top of the integer-second C API.
"""

from __future__ import annotations

import math
from datetime import datetime

from ._datetime import decode_date, encode_date

_SECS_PER_DAY = 86400.0


def oadate_to_datetime(value: float) -> datetime:
    """Convert a SWMM DateTime float to a :class:`datetime.datetime`.

    :param value: SWMM DateTime (decimal days since 1899-12-30).
    :returns:     Corresponding naive datetime.
    """
    value = float(value)
    # Calendar portion — exact SWMM math via the C API.
    year, month, day = decode_date(value)
    # Sub-second precision handled in Python so we don't truncate to whole
    # seconds the way the C decode_time helper does. floor() (not int())
    # keeps the fractional remainder in [0, 1) for negative inputs too.
    frac = value - math.floor(value)
    total_us = int(round(frac * _SECS_PER_DAY * 1_000_000))
    hour, rem = divmod(total_us, 3600 * 1_000_000)
    minute, rem = divmod(rem, 60 * 1_000_000)
    second, microsecond = divmod(rem, 1_000_000)
    # The microsecond rollover protects against round(0.999999...) artifacts.
    if hour >= 24:
        hour = 23
        minute = 59
        second = 59
        microsecond = 999_999
    return datetime(year, month, day, hour, minute, second, microsecond)


def datetime_to_oadate(dt: datetime) -> float:
    """Convert a :class:`datetime.datetime` to a SWMM DateTime float.

    :param dt: Naive datetime to encode.
    :returns:  SWMM DateTime (decimal days since 1899-12-30).
    """
    # Integer-day portion via the C API (epoch matches SWMM bit-for-bit).
    date_part = encode_date(dt.year, dt.month, dt.day)
    # Fractional day computed in Python to retain microsecond precision.
    time_part = (
        dt.hour * 3600
        + dt.minute * 60
        + dt.second
        + dt.microsecond / 1_000_000.0
    ) / _SECS_PER_DAY
    return date_part + time_part
