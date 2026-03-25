#
# src/CMakeLists.txt
#
# Registers all library subdirectories in build order.
#
# Libraries built:
#   1. openswmm_legacy_engine  (src/legacy/engine/)  — original EPA SWMM 5.x solver
#   2. openswmm_legacy_output  (src/legacy/output/)  — original binary output reader
#   3. openswmm_plugin_sdk     (src/plugin_sdk/)     — header-only plugin interfaces
#   4. openswmm_engine         (src/engine/)         — new data-oriented engine
#   5. openswmm_cli            (src/cli/)            — command-line interface
#
# See: docs/MASTER_IMPLEMENTATION_PLAN.md — Phase 1 (Legacy Module Physical Relocation)
#

# ---- Legacy libraries (preserved unmodified from EPA SWMM 5.x) ----
# Source files physically located in src/legacy/ after Phase 1 relocation.
# Public headers are at include/openswmm/legacy/engine/ and include/openswmm/legacy/output/.
add_subdirectory(legacy)   # → openswmm_legacy_engine + openswmm_legacy_output

# ---- Plugin SDK (header-only; must come before engine so engine can link it) ----
add_subdirectory(plugin_sdk)  # → openswmm_plugin_sdk (INTERFACE, no .so)

# ---- New engine (Phase 2+) ----
add_subdirectory(engine)   # → openswmm_engine (output name: openswmm.engine)

# ---- CLI ----
add_subdirectory(cli)

# ---- openswmm2d (2D FVHM solver — future coupling target; disabled for now) ----
# add_subdirectory(openswmm2d)

# ---- Legacy C++ wrapper (disabled; superseded by new engine) ----
# add_subdirectory(openswmm)
