# R1 / P0b factor correction

`affected_deck_manifest.json` names and hashes 37 retained inputs with before/after library hashes and full API-series hashes: 15 intentional changes, 22 unchanged trajectories. `compare.py` reproduces the comparison against staged P0a and candidate engines using the common API worker. Controls and conductivity cases run a 10-minute first burst, a dry interval, then a second burst. Green–Ampt recovery fixtures use a 20-hour inter-event gap in a 23:59-hour run; CN recovery uses 0.5-day drying time. Each worker checks actual cumulative depth × SI cell area against the infiltration ledger.

The monthly ponded CONSTANT fixture crosses January/February. Initial capacity is 0.4 × authored rate; after the next INFIL_STEP refresh in February it is 0.8 × authored rate. Both expected values are checked independently. Recovery can be configured without changing an observable trajectory; the separate ten-case surface suite explicitly drains the last film and verifies recovery state and next-storm capacity for all five stateful methods.

`merged_summary.json` is a compatibility check of the combined working tree, not the isolated bit-identity gate. It records every maximum difference and its declared tolerances. Sources contain unrelated changes; the baseline/candidate attribution uses isolated libraries only.

Registered corpus: 25/25 unchanged under default factors. Mesh census: 7 unchanged, 3 non-running fixtures skipped. Existing zero Horton drying-time behavior is preserved.
