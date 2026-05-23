"""Type stubs for :mod:`openswmm.engine._dates`."""

from datetime import datetime


def oadate_to_datetime(value: float) -> datetime: ...
def datetime_to_oadate(dt: datetime) -> float: ...
