# Per-member port-graph failure dumps (PORT_HAMILTONIAN O-6)

Three member solves from the coupled Bellinge storm peak (`demo_ph5.inp`, per-member
stores ON, HEAD 1fda9e04 + the dump hook, 2026-11-04) whose Newton failed, written by
`uncertainty/RomPortGraphDump.hpp` (`OPENSWMM_PH_DUMP_DIR`) and compacted for the
repository by `PortGraphDump.CompactDumps` in `test_rom_port_graph_replay.cpp`: ports
that are exact planes, or whose ends move < 1e-3 ft in the solve, become plane edges or
tangent planes; stores that are exactly a·z (the street ponds) become linear nodes; the
Schur base is left out. Format: binary, version 1 (see the header). Replayed by
`PortGraphDump.CommittedBellingeDumpsConverge` (≤ 20 Newton iterations, no fallback).
The full set (252 dumps, 2.1 GB) is in `SWMM6/data/ph_dumps`, outside the repository.

| file | in the run | k (N set) | class | O-5 solver on this dump | O-6 |
|---|---|---|---|---|---|
| `ph_0002_fb_t16385_m03.bin` | Jacobi fallback | 251 | A: ~20 dry coupled manholes (G72F562, G72R335, …), each a 0.01 ft dry ramp into its street pond; the global kink bound crosses one per iteration | 20 its, fallback | 4 its |
| `ph_0025_nc_t18822_m09.bin` | not converged | 268 | A or B (G80F13P / G80F310) | 20 its, Jacobi fails | 3 its |
| `ph_0032_fb_t21195_m30.bin` | Jacobi fallback | 307 | B: G80F66Y/G80F310/G80F330, a law that curves before its kink; "just past the kink" landed far short of it | 20 its, fallback | 9 its |

Mutation check (scratch builds of `RomPortGraph.hpp`, 2026-11-04): without the per-node
bound `ph_0002` falls back again (20 its); without the kink estimate `ph_0032` does.
