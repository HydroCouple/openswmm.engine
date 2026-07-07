# Benchmark Data Conventions

## Unit system

openswmm.engine uses US customary units internally throughout its solver:

| Quantity             | Internal unit | Conversion from common user unit |
|----------------------|--------------|----------------------------------|
| Length / depth       | ft           | 1 in = 1/12 ft                   |
| Flow rate            | ft³/s (cfs)  | 1 in/hr = 1/(12·3600) ft/s over unit area |
| Infiltration rate    | ft/s         | 1 in/hr = 1/(12·3600) ft/s       |
| Velocity             | ft/s         | —                                |
| Time                 | s            | 1 hr = 3600 s                    |
| Area                 | ft²          | —                                |
| Volume               | ft³          | —                                |

Reference CSV files use the internal (ft, s) units for all columns that are
compared directly against solver output. Where a human-readable counterpart is
helpful, additional columns in in/hr, inches, or hours may be included with a
`_in_per_hr`, `_in`, or `_hr` suffix.

## Time origin

`t = 0` is the start of the scenario. All reference datasets begin at `t = 0`.

## Sign conventions

- Infiltration rate: positive downward (loss from surface water).
- Groundwater lateral flow: positive toward the drainage network.
- Storage exfiltration: positive downward (loss from storage).
- Routing flow: positive in the downstream direction.

## Variable naming in CSV columns

| Pattern          | Meaning |
|------------------|---------|
| `t_s`            | time in seconds |
| `t_hr`           | time in hours |
| `*_ft`           | length or depth in feet |
| `*_ft_per_s`     | rate in ft/s |
| `*_ft3_per_s`    | volumetric flow in ft³/s |
| `*_in`           | depth in inches |
| `*_in_per_hr`    | rate in in/hr |
| `y_exact`        | dimensionless exact solution (ODE benchmarks) |
| `err_abs`        | absolute error (computed - reference) |

## Interpolation

Test code that queries reference values at times not listed in the CSV should
use linear interpolation unless the benchmark's `provenance.yaml` specifies
otherwise. Benchmarks with smooth exact solutions should be sampled densely
enough that linear interpolation error is negligible at the intended tolerance.

## CSV format

- First line(s) beginning with `#` are comments and should be ignored by parsers.
- The first non-comment line is the header row.
- All subsequent lines are data rows with no blank lines.
- Delimiter: comma.
- Floating-point values: full double-precision representation (no rounding to
  fewer digits than needed to round-trip the value).
