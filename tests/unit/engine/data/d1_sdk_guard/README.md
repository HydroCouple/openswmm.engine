# D1 guard fixtures (program plan §C.1, decision D-C6)

Inputs for `cmake/CheckNoHydroCoupleSDK.cmake`'s own falsification. These are
**not** built and are excluded from the real-tree scan.

| dir | contains | the guard must |
|---|---|---|
| `planted_include/` | a C++ file that `#include`s from `hydrocouplesdk/` | **fail** |
| `planted_link/` | a CMake file that `find_package`s and links `HydroCoupleSDK` | **fail** |
| `prose_only/` | files that *name* the SDK only in comments and strings | **pass** |

`prose_only` is the one that keeps the guard switched on: the files that state
D-C6 have to mention the SDK, and a guard that failed on its own documentation
would not survive a week.
