"""Sanity check for openswmm/engine/_dates.py round-trip behaviour.

Runs without the compiled Cython module by stubbing the ``_datetime``
module with a pure-Python equivalent of the C API encode/decode pair.
That stub is then good enough to exercise the date / sub-second arithmetic
in ``_dates.py``.

Run from the repo root:
    python3 scripts/verify_dates_python.py
"""

from __future__ import annotations

import math
import sys
import types
from datetime import datetime, timedelta
from pathlib import Path


# --- Pure-Python stand-ins for the C API encode/decode -----------------
DATE_DELTA = 693594  # matches SWMM_DATETIME_DATE_DELTA


def encode_date(year: int, month: int, day: int) -> float:
    """Replicate openswmm::datetime::encodeDate (date portion only)."""
    leap = (year % 4 == 0) and ((year % 100 != 0) or (year % 400 == 0))
    dpm = [31, 29 if leap else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    if not (1 <= year <= 9999 and 1 <= month <= 12 and 1 <= day <= dpm[month - 1]):
        return float(-DATE_DELTA)
    d = day
    for j in range(month - 1):
        d += dpm[j]
    y = year - 1
    return y * 365 + y // 4 - y // 100 + y // 400 + d - DATE_DELTA


def decode_date(value: float) -> tuple[int, int, int]:
    """Replicate openswmm::datetime::decodeDate (calendar arithmetic only)."""
    t = int(math.floor(value)) + DATE_DELTA
    if t <= 0:
        return (0, 1, 1)
    t -= 1
    D1, D4, D100, D400 = 365, 1461, 36524, 146097
    y = 1
    while t >= D400:
        t -= D400
        y += 400
    i, d = divmod(t, D100)
    if i == 4:
        i -= 1
        d += D100
    y += i * 100
    i, d = divmod(d, D4)
    y += i * 4
    i, d = divmod(d, D1)
    if i == 4:
        i -= 1
        d += D1
    y += i
    leap = (y % 4 == 0) and ((y % 100 != 0) or (y % 400 == 0))
    dpm = [31, 29 if leap else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    m = 1
    while True:
        n = dpm[m - 1]
        if d < n:
            break
        d -= n
        m += 1
    return (y, m, d + 1)


# Install the stub as openswmm.engine._datetime so _dates.py imports succeed.
fake_pkg = types.ModuleType("openswmm.engine._datetime")
fake_pkg.encode_date = encode_date
fake_pkg.decode_date = decode_date
sys.modules["openswmm.engine._datetime"] = fake_pkg

# Add the package root to sys.path so we can import _dates standalone.
repo_root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(repo_root / "python"))

# Skip the package __init__ (which imports compiled Cython modules) and load
# _dates.py directly from disk so the test stays self-contained.
import importlib.util

spec = importlib.util.spec_from_file_location(
    "openswmm.engine._dates",
    repo_root / "python" / "openswmm" / "engine" / "_dates.py",
)
_dates = importlib.util.module_from_spec(spec)
sys.modules["openswmm.engine._dates"] = _dates
spec.loader.exec_module(_dates)


# --- Tests ---------------------------------------------------------------
failures = 0


def check(name: str, cond: bool) -> None:
    global failures
    if not cond:
        print(f"FAIL {name}")
        failures += 1


def approx_equal(a: float, b: float, tol: float = 1e-9) -> bool:
    return abs(a - b) <= tol


# Round-trip whole-second datetime.
dt = datetime(2024, 6, 15, 13, 30, 45)
oad = _dates.datetime_to_oadate(dt)
back = _dates.oadate_to_datetime(oad)
check("whole-second round-trip", back == dt)

# Round-trip with microseconds.
dt_us = datetime(2024, 6, 15, 13, 30, 45, 123456)
oad_us = _dates.datetime_to_oadate(dt_us)
back_us = _dates.oadate_to_datetime(oad_us)
# We allow one-microsecond slop, since 1us / day ~ 1.16e-11 is near double
# precision and round-trip should still land within +/-1 us.
delta_us = abs((back_us - dt_us).total_seconds()) * 1_000_000
check(f"microsecond round-trip (|delta|={delta_us:.3f}us)", delta_us <= 1.0)

# Epoch: 1899-12-30 -> 0.0
check("epoch encodes to 0", _dates.datetime_to_oadate(datetime(1899, 12, 30)) == 0.0)
check("epoch decodes from 0", _dates.oadate_to_datetime(0.0) == datetime(1899, 12, 30))

# Noon is 0.5.
check(
    "noon at epoch is 0.5",
    approx_equal(_dates.datetime_to_oadate(datetime(1899, 12, 30, 12, 0, 0)), 0.5),
)

# Negative OADate (pre-epoch).
pre = datetime(1800, 1, 1)
oad_pre = _dates.datetime_to_oadate(pre)
back_pre = _dates.oadate_to_datetime(oad_pre)
check("pre-epoch round-trip", back_pre == pre)

if failures == 0:
    print("OK — all _dates.py checks passed.")
    sys.exit(0)
print(f"FAIL — {failures} check(s) failed.")
sys.exit(1)
