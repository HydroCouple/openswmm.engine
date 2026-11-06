# Kokkos backend selection and qualification

The GUI uses the built-in CPU marcher for an authored `BACKEND AUTO` run. New GUI models are authored with `BACKEND CPU`. An explicit model request or `OPENSWMM_2D_BACKEND` remains an override. CUDA, HIP and SYCL are experimental choices in the GUI.

The engine API `swmm_2d_set_auto_backend_cpu(engine, 1)` applies the GUI's AUTO policy to one BUILDING or OPENED handle. Apply it after opening and before initializing, including when opening a checkpoint session. It does not rewrite the authored backend, alter checkpoint identity, or change process environment variables. Other clients retain the engine's existing AUTO selection policy. Reapply the policy after reopening a handle.

## Plugin boundary

The discovery functions have C linkage, but the surface solver object has a C++ interface. This is not a compiler-independent solver ABI. ABI 5 introduced the requirement for a matching build contract before the engine calls the probe or factory. The contract hashes engine headers and identifies the host compiler, platform, pointer size, build configuration and common compilation flags. Rebuild and package the engine and accelerator plugin together. The live ABI is now 8 following the canonical age-generation, accepted-rainfall and gross boundary-water receipt layout changes; the build contract remains mandatory. Older ABI 3/4/5/6/7 plugins must be rebuilt; they cannot be made compatible by editing a version number. The change to `ISurfaceSolver::completedSourceTime()` is one reason those older virtual tables are unsafe.

A matched plugin exports its own destruction function. The core owns an adapter and never deletes a plugin object with the core allocator. Plugin constructors convert C++ failures to a null result with a diagnostic; the loader also catches discovery/factory exceptions. These safeguards do not contain access violations, driver aborts, or failures in shared-library initialization. Device fault containment requires a separate worker process before unattended accelerator execution can be considered resilient.

All core-controlled surface plugin calls and the optional packed groundwater recharge calls use one controlling host thread. Calls are synchronous to the engine. Creation, initialization, stepping, synchronization and destruction retain thread affinity; concurrent GUI workers queue their accelerator work. This deliberately sacrifices concurrent device dispatch until explicit execution instances, stream ownership and buffer lifetime rules have separate qualification. The owner, its containing engine module, plugin libraries and Kokkos runtime remain process-resident once discovery is requested. No owner-thread join occurs in DLL teardown. This avoids the loader-lock synchronization problem described in [Microsoft's DLL guidance](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices). CPU-only clients that never request discovery do not create or pin an accelerator owner.

Performance counters belong to each solver and are activated only during that solver's calls. Interleaved runs must not reset or attribute each other's counters. External Kokkos profiling tools retain precedence over the built-in callbacks.

## Physics coverage

| Operator | Current execution contract |
| --- | --- |
| Triangle local-inertial surface water, FLAT/VFR | Kokkos surface path, subject to backend qualification |
| Quadrilateral/mixed meshes, FULL_SWE, diffusive wave | Built-in CPU; isolated device helper tests do not establish an integrated solver |
| Surface pollutants, water age, temperature | Built-in CPU; the Kokkos surface solver rejects transport rows |
| Coupled 2D groundwater and its canonical state | Built-in CPU surface/subsurface ownership |
| CLOSED_FORM raw groundwater recharge | Optional explicitly requested Kokkos OpenMP operator; water stores, receipts, quality and heat remain host-owned |
| Groundwater advection, dispersion, sorption, reactions and layered chemistry | Host-owned; no device qualification implied by recharge acceleration |
| Mesh/porous/ground/channel heat and thermal coupling | Host-owned; preserve energy stores, interfaces and restart identity before porting |

A successful surface parity test is insufficient to expand any row in this table. Each new operator needs CPU-reference and analytical/manufactured-solution checks; donor budgets, nonnegativity and dry/rewet behavior; water, species and energy ledgers; order-of-accuracy checks; rejection/rollback; and checkpoint continuation. Runtime activation must require the complete model's capabilities, not just the availability of a device.

## Local and platform validation

Configure GPU tests from the engine root; the old standalone test project omitted dependencies of the production CPU solver. `tests/gpu` now links the actual engine and builds the selected Kokkos solver implementation against it. OpenMP parity checks assert the execution-space identity and requested concurrency.

The hardware-independent loader tests use a deliberately failing fake CUDA plugin. They verify that CPU policy and stale-contract rejection do not call its probe, that explicit overrides are honored, and that a throwing factory is contained. Real OpenMP tests must load the real plugin and exercise concurrent model lifecycles. Groundwater operator gates require the plugin rather than skipping when discovery fails.

For platform qualification, retain compiler/Kokkos/toolkit/driver/device identities, source hashes, effective backend and thread count, full failing inputs, and sanitizer logs. CUDA validation additionally requires actual device-memory execution and Compute Sanitizer memcheck, initcheck, racecheck and synccheck on representative surface and coupled workloads. CPU/OpenMP or sanitizer success here is not a Windows/CUDA qualification.

Surface ABI7 includes the accepted per-cell rainfall receipt in shared surface state. Rebuild the core and surface plugins together; an ABI6 probe is refused before factory construction even when its build-contract string matches. The independent C-only raw groundwater recharge ABI remains1. Actual OpenMP qualification uses the matching plugin from the same core configure; no CUDA/HIP/SYCL execution is inferred from host or fallback checks.

Surface ABI 8 adds separate applied boundary inflow/outflow arrays. Kokkos accumulates gross volumes at the volume-clamped boundary update and publishes them additively; the engine drains them into its own boundary receipts after each advance. This preserves reversals within a window and supplies the continuity and checkpoint paths without mutating the engine receipt map inside a plugin. Evaporation reporting uses the applied cumulative loss for all backends. Core and surface plugins must be rebuilt together.

The OpenMP end-to-end gates compare every 2D continuity report term with CPU results under FLAT, VFR, and nonzero evaporation, require both boundary directions and a closed ledger, and reject fallback. The direct alignment suite covers all non-wall boundary laws, a within-window reversal, and orifice/weir-orifice node drain/spill with shared pond budgets. These host checks do not qualify CUDA hardware.
