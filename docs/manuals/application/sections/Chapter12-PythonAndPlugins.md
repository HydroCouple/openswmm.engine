@page application_manual_ch12_python_plugins Chapter 12: Running Models from Python and Plugins

Everything the other chapters do from a deck and a command line can be done
from Python, and several things can only be done from Python: forcing a
model from a live data source, stepping it under a controller, reading a
result set without writing a file. This chapter is the map from a modelling
task to the interface that performs it.

Three interfaces exist, and they are layers of one engine rather than
alternatives:

| Interface | What it is | Reach for it when |
|---|---|---|
| The command line | `openswmm model.inp model.rpt model.out` | the deck is the whole experiment |
| The Python package | `openswmm.engine`, a Cython binding over the C API | the run is one step in a script: sweeps, forcing, post-processing |
| A plugin | a shared library the engine loads and calls back | the behaviour belongs *inside* every run, including runs someone else starts |

## When the command line is enough

A deck plus a report is a complete experiment, and the runner that produced
every simulated figure in this manual is exactly that:
`scripts/build_manual_figures.py` upserts a few `[OPTIONS]` lines into a
copy of a deck, runs the binary, and reads the report and the binary output.
Nothing in this manual's comparisons needs more.

Two things the command line cannot do: change a model while it is running,
and see a result before the run ends.

## Driving a run from Python

The package exposes the engine as a solver you open, step and close. The
canonical shape is the context manager, which guarantees the close:

```python
from openswmm.engine import Solver

with Solver("model.inp", "model.rpt", "model.out") as solver:
    for elapsed in solver.steps():       # a timedelta since the start
        pass
    print(solver.routing_error())
```

`steps()` advances one routing step per iteration and yields the elapsed
time as a `timedelta`, so a controller reads state, decides, writes back
and continues. `step()` is the same thing one step at a time. The routing
error is a **fraction**, not a percentage — a gate that asserts it is below
0.5 is asserting 50 %, not half a percent.

The guide pages carry the worked forms, and each is a short page:

| Task | Guide page |
|---|---|
| Install the package | [install](python/guide/install.html) |
| The first run, end to end | [quickstart](python/guide/quickstart.html) |
| Open, step, close, and the state machine | [solver](python/guide/solver.html) |
| Build a model with no input file | [model_builder](python/guide/model_builder.html) |
| Prescribe rainfall, evaporation and temperature at runtime | [forcing](python/guide/forcing.html) |
| Read nodes, links and subcatchments while stepping | [nodes](python/guide/nodes.html), [links](python/guide/links.html), [subcatchments](python/guide/subcatchments.html) |
| Cross-section geometry without a model | [xsect_geometry](python/guide/xsect_geometry.html) |
| The 2D surface: mesh, depths, velocities | [2d](python/guide/2d.html) |
| Water quality, reactions, age and heat | [quality](python/guide/quality.html), [reactions](python/guide/reactions.html), [water_age](python/guide/water_age.html), [heat](python/guide/heat.html) |
| Process components from Python | [process_components](python/guide/process_components.html) |
| Read a binary output file after the run | [output_reader](python/guide/output_reader.html) |
| Hot start files | [hotstart](python/guide/hotstart.html) |
| Mass balance and statistics | [massbalance](python/guide/massbalance.html), [statistics](python/guide/statistics.html) |
| Plot results | [plotting](python/guide/plotting.html) |
| Read and write GeoPackage | [geopackage](python/guide/geopackage.html) |

The full class and method reference is the
[Python Bindings API Reference](python/index.html).

**A caution about the state machine.** A solver is past OPENED as soon as
the context manager enters; several setters are refused after that point
because the engine has already resolved the model. The
[solver](python/guide/solver.html) page lists which, and
@ref engine_manual_ch5_api "Chapter 5 of the Engine Manual" gives the same
sequence for the C API, where the states are explicit.

## Three things worth doing from Python

**A parameter sweep that never writes a deck.** The model builder makes the
network in memory, so a sweep over a design variable is a loop rather than a
directory of near-identical files. The comparisons in this manual are the
same idea run from the deck side: upsert the keys, run, read.

**Forcing from outside the model.** Prescribed rainfall, evaporation and
temperature enter *after* the rain-snow split of
@ref hydrology_ref_ch2_meteorology "Hydrology 2, §2.1.6" — override replaces
the gage's value and addition augments it, and neither is scaled by the
gage or subcatchment factors. That is what lets a live feed or a downscaled
climate series drive an existing deck with its gages left in place.

**Reading the 2D surface without the file.** A coupled run's mesh state is
addressable while it steps: depth, velocity and species per cell. The
result file is written for later analysis, not required for it, and it is
not readable while the run is in progress — there is no flush and no
single-writer-multiple-reader mode.

## When the behaviour belongs in a plugin

A plugin is loaded by the engine and called back during the run, so it acts
on every run of a model rather than on the runs a particular script starts.
The engine ships built-in plugins for input, output, reporting and state IO,
and a project names extra ones in `[PLUGINS]`
(@ref engine_manual_sect_PLUGINS). Discovery, version resolution and the
callback sequence are in
@ref engine_manual_ch5_plugins "Chapter 5 of the Engine Manual, §5.8".

The rule of thumb:

- A **process component** adds physics — reactions, heat, water age,
  transport — and is configured from a file the deck names in
  `[PROCESS_COMPONENTS]` (@ref engine_manual_sect_PROCESS_COMPONENTS).
  @ref application_manual_ch9_msx "Chapter 9: multi-species reactions" configures one.
- An **output or report plugin** changes what a run writes without changing
  what it computes.
- A **solver plugin** replaces a kernel. The 2D backends are selected this
  way; a backend that a build does not carry is refused at open rather than
  silently ignored.

## Where to go next

- @ref engine_manual_ch5_api "Engine Manual, Chapter 5" — the C API, the
  state machine, user flags, extension options and the plugin interface
- @ref manual_running — running from the GUI
- @ref manual_plugins — managing plugins from the GUI
- @ref application_manual_ch13_planned "Chapter 13" — what is coming
