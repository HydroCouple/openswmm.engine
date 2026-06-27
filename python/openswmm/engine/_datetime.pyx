# :author: Caleb Buahin
# :copyright: Copyright (c) 2026 Caleb Buahin
# :license: MIT
#
# _datetime.pyx — Python bindings for the SWMM DateTime conversion C API.
#
# SWMM's native DateTime is a ``double`` whose integer part is days since
# 1899-12-30 (OLE Automation epoch) and fractional part is the time-of-day
# fraction. This module exposes the encode/decode primitives published in
# ``openswmm_datetime.h`` so Python callers can move between the SWMM
# DateTime double and ``(year, month, day, hour, minute, second)`` tuples
# without reimplementing the legacy epoch arithmetic.
#
# cython: language_level=3

from ._common cimport (
    swmm_datetime_encode_date,
    swmm_datetime_encode_time,
    swmm_datetime_decode_date,
    swmm_datetime_decode_time,
    swmm_datetime_add_seconds,
    swmm_datetime_time_diff,
)


def encode_date(int year, int month, int day) -> float:
    """Encode a calendar date as a SWMM DateTime (date portion only).

    :param year:  Year in 1..9999.
    :param month: Month in 1..12.
    :param day:   Day of month, valid for the given year/month.
    :returns:     Integer-valued SWMM DateTime double.
    :raises ValueError: if the date is out of range.
    """
    cdef double out = 0.0
    if swmm_datetime_encode_date(year, month, day, &out) != 0:
        raise ValueError(
            f"invalid SWMM date: year={year} month={month} day={day}"
        )
    return out


def encode_time(int hour, int minute, int second) -> float:
    """Encode a wall-clock time as the fractional-day part of a SWMM DateTime.

    :returns: Fractional-day value in [0.0, 1.0).
    :raises ValueError: if any component is negative.
    """
    cdef double out = 0.0
    if swmm_datetime_encode_time(hour, minute, second, &out) != 0:
        raise ValueError(
            f"invalid SWMM time: hour={hour} minute={minute} second={second}"
        )
    return out


def decode_date(double value) -> tuple:
    """Decode the date portion of a SWMM DateTime to ``(year, month, day)``."""
    cdef int y = 0, m = 0, d = 0
    swmm_datetime_decode_date(value, &y, &m, &d)
    return (y, m, d)


def decode_time(double value) -> tuple:
    """Decode the time-of-day portion of a SWMM DateTime to ``(hour, minute, second)``.

    The decomposition matches the legacy SWMM engine bit-for-bit
    (``floor(fracDay * 86400 + 0.5)``), so it is in whole seconds.
    """
    cdef int h = 0, mi = 0, s = 0
    swmm_datetime_decode_time(value, &h, &mi, &s)
    return (h, mi, s)


def add_seconds(double value, double seconds) -> float:
    """Return ``value`` shifted by ``seconds`` (may be negative).

    Uses the legacy decompose-recompose path so results are bit-identical
    to a legacy SWMM run.
    """
    cdef double out = 0.0
    swmm_datetime_add_seconds(value, seconds, &out)
    return out


def time_diff(double value1, double value2) -> int:
    """Return ``value1 - value2`` rounded to the nearest whole second."""
    cdef long out = 0
    swmm_datetime_time_diff(value1, value2, &out)
    return out
