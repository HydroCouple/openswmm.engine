# Mesh checkpoint regression fix — 2026-10-20

An isolated snapshot of engine `c5ca4cfd` reproduced the same **43 failures**
reported during multispecies qualification: 129/172 passed. Forty-two refusals
stopped at the fresh mesh-header guard; one read-only-state assertion also
observed the changing serialized result prefix.

HDF5 2.1.1 reclaimed a zero-filled unused allocation tail after the initial
flush. In the diagnostic example the file shrank from 11,469 to 9,933 bytes;
only the superblock EOF/checksum and zero tail differed. All common object/data
bytes after the superblock were identical. The baseline library included a
header-dump probe only, preserving the pre-fix guard and model behavior.

The fix settles that allocation-tail update with a second checked flush before
capturing bytes. Exact fresh-header checks, saved-prefix schema/data validation,
file identity checks and atomic multi-output publication are unchanged. No
existing test assertions or numerical solvers were changed.

**Final checks:** 174/174 engine checkpoint tests and 31/31 mesh-output tests pass.
Two new regressions check an initial restore/read-only header and continuation
across a wall-clock delay. Native binary output keeps byte comparisons; HDF5
results use physical-state and dataset/metadata comparisons because compressed
allocation layouts may differ after reopening. The new initial-restore regression
fails against the pre-fix library and passes with the fix. The targeted three-case
run also includes coupled thermal/Richards LID continuation.

The complete suite covers malformed prefixes, failed publication and retry,
source histories, report clocks/averaging, boundary/provider expiry, thermal/GW/LID
stores, damaged companions and relocated bundles. These are bounded CPU synthetic
profiles, not universal platform/backend, crash or power-loss qualification.

Development and testing used an independent source/build/output directory under
`/tmp/openswmm-mesh-checkpoint-20261020`; active groundwater/routing work was not
modified. [Checks and provenance](checks.json) retain source/binary hashes and all
43 prior failure names. All test outcomes, the counterfactual and targeted result
are retained beside this record. Long binary-payload diagnostic renderings are
summarized; unabridged logs remain in the isolated run directory and their hashes
are in checks.json.

Reproduce with a configured HDF5-enabled CPU build (use a new output folder):

```sh
OMP_NUM_THREADS=1 SWMM_R4_SESSION_OUT=/tmp/mesh-checkpoint-checks \
  /path/to/build/bin/Release/test_engine_checkpoint
/path/to/build/bin/Release/test_engine_2d_output_options
```

The suite writes fixed fixture names and never empties a folder supplied through
SWMM_R4_SESSION_OUT. Do not reuse another active test session's output folder.
