#!/usr/bin/env python3
"""Regenerate rain_wide.csv (checked in next to this script).

Fixture for test_multicolumn_series_file.cpp: a CSV with 2000 data columns
whose header row and every data row are far wider than the 4096-byte fgets
buffer the pre-2026-08-17 loader used (plan gap P2 — a wide row was silently
split across reads, and a wide header shifted every column index).

Layout (deterministic so the test can spot-check high columns):
  header:  DateTime,G0001,...,G2000
  row r:   2020-01-01 0r:00, value(c, r) ... where value = c + r/10
"""

NCOLS = 2000
NROWS = 6

with open("rain_wide.csv", "w", newline="\n") as f:
    f.write("DateTime," + ",".join(f"G{c:04d}" for c in range(1, NCOLS + 1)) + "\n")
    for r in range(NROWS):
        cells = ",".join(f"{c + r / 10:.1f}" for c in range(1, NCOLS + 1))
        f.write(f"2020-01-01 {r:02d}:00,{cells}\n")

print(f"rain_wide.csv written: {NROWS} rows x {NCOLS} data columns")
