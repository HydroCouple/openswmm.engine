# Regression Test Data

Place EPA SWMM5 Example network `.inp` files here. They are public domain (CC0)
and ship with the EPA SWMM5 executable distribution:

    https://www.epa.gov/water-research/storm-water-management-model-swmm

Files expected by `test_engine_regression` and `bench_engine_vs_legacy`:
- `Example1.inp`  — small drainage, 6h storm, DW routing
- `Example2.inp`  — LID, combined sewer
- `Example3.inp`  — water quality routing
- `Example6.inp`  — force mains, storage, pumps
- `Example7.inp`  — real-time control rules
- `Example8.inp`  — tidal outfall, backwater

These files are intentionally not committed to the repository; download them
from the EPA distribution or from:
    https://github.com/USEPA/Stormwater-Management-Model
