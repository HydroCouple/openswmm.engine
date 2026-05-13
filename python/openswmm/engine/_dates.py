"""
SWMM OADate <-> :class:`datetime.datetime` conversion helpers.

A SWMM OADate is a floating-point value where the integer part is the number
of days since 1899-12-30 (the OLE Automation epoch SWMM uses) and the
fractional part is the time-of-day fraction (0.5 = noon).
"""

from __future__ import annotations

from datetime import datetime, timedelta

_OADATE_EPOCH = datetime(1899, 12, 30)


def oadate_to_datetime(value: float) -> datetime:
    """Convert a SWMM OADate float to a :class:`datetime.datetime`.

    @param value: OADate (decimal days since 1899-12-30).
    @type value: float
    @return: Corresponding naive datetime.
    @rtype: datetime.datetime
    """
    return _OADATE_EPOCH + timedelta(days=float(value))


def datetime_to_oadate(dt: datetime) -> float:
    """Convert a :class:`datetime.datetime` to a SWMM OADate float.

    @param dt: Naive datetime to encode.
    @type dt: datetime.datetime
    @return: OADate (decimal days since 1899-12-30).
    @rtype: float
    """
    return (dt - _OADATE_EPOCH).total_seconds() / 86400.0
