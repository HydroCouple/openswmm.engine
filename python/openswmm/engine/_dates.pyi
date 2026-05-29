"""Type stubs for :mod:`openswmm.engine._dates`.

High-level helpers that convert between SWMM's native DateTime ``double``
(decimal days since 1899-12-30) and :class:`datetime.datetime`. The
calendar arithmetic is delegated to the C API in ``openswmm_datetime.h``.
"""

from datetime import datetime


def oadate_to_datetime(value: float) -> datetime: ...
def datetime_to_oadate(dt: datetime) -> float: ...
