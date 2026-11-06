# Synthetic placeholder series for `refactored_small.inp` / `legacy_small.inp`

Both fixtures carry two `[TIMESERIES] … FILE` rows that point here:

```
NOAA_RIC_2004_2022          FILE ".../NOAA_RIC_2004_2022.dat"
USGS_James_River_2002_2022  FILE ".../USGS_James_River_2002_2022.dat"
```

The real data — NOAA precipitation for Richmond VA (RIC) and USGS James River
observations — is **not redistributable**, so it was never checked in. The two
`.dat` files here are **synthetic, all-zero stand-ins**, not observations. Do
not use them for anything but opening these two fixtures.

## Why they exist

A missing `FILE` timeseries used to be skipped silently: the series loaded
empty and read `0.0` at every lookup. It now fails the open with `ERROR 361`,
matching the legacy engine (`src/legacy/engine/project.c` → `table.c`), which
has always errored on these same two references. Without these placeholders
every strict `open()` of either fixture fails, which is ~18 python engine tests
(`python/tests/engine/test_param_runtime.py`, `test_state_injection.py`,
`python/tests/test_pet_parity.py`, `python/tests/engine/test_forcing_quality.py`).

## Why the values are zero, at a daily interval

`NOAA_RIC_2004_2022` drives the model's only rain gage
(`[RAINGAGES] … INTENSITY 0:05 TIMESERIES NOAA_RIC_2004_2022`), and it
previously loaded as an *empty* series — i.e. zero rainfall. These files must
preserve that exactly so the existing tests' numbers do not move:

- **All values are `0.0`** ⇒ zero rainfall, zero runoff, as before.
- **Daily rows at `00:00`** spanning `START_DATE 10/07/2012` →
  `END_DATE 10/07/2013` (plus one trailing day). The interval is deliberately
  *coarse*: `computeRunoffTimestep()`
  (`src/engine/core/SWMMEngine.cpp`, legacy `gage_getNextRainDate`) shortens the
  runoff step to the next rain-series boundary only when that gap is **less
  than** the current max step. This model uses
  `WET_STEP = DRY_STEP = 00:05:00` (300 s), and every daily-midnight gap is a
  multiple of 300 s, so nothing is ever shortened and the time-step sequence is
  identical to the empty-series behavior. A 5-minute placeholder would also be
  harmless here but is 105k rows; a *non*-300 s-aligned interval would not be.

`USGS_James_River_2002_2022` is defined in both fixtures but referenced by no
object. It gets the same treatment because the loader loads every FILE-backed
series unconditionally.

## Regenerating

```sh
cd tests/unit/engine/data
python3 - <<'PY'
import datetime
day, end = datetime.date(2012, 10, 7), datetime.date(2013, 10, 8)
rows = []
while day <= end:
    rows.append("%02d/%02d/%04d 00:00  0.0" % (day.month, day.day, day.year))
    day += datetime.timedelta(days=1)
print("\n".join(rows))
PY
```

Whitespace `date time value` with `;` comment lines is the plain-timeseries
grammar the legacy loader accepts (`PostParseResolver.cpp`
`load_external_timeseries_files`, legacy `table.c:table_readFileTable`). No
commas or tabs, so `looks_like_multicolumn_series_file()` correctly keeps these
files on the legacy path rather than the multi-column parser.
