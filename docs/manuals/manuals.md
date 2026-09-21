@page manuals OpenSWMM Manuals

OpenSWMM Manuals
=================================

The following manuals document the use, application, and technical reference for OpenSWMM, the next generation of the EPA Storm Water Management Model. These manuals build on the original EPA SWMM documentation, updated and extended for OpenSWMM.

See @ref authors for the full list of authors and contributors.

* Engine Manual (@ref engine_manual) — the objects a model is built from, the input
  file format, the files the engine reads and writes, its reports, the C and Python
  interfaces, and every error and warning message.
* GUI Manual — the graphical application, SWMMVis, is documented separately at
  <https://www.hydrocouple.org/openswmm.gui/>
* Application Manual (@ref application_manual) — which formulation to use and what
  changes if you pick another: six decision workflows and thirteen chapters, each
  worked from a deck in the repository with every figure generated from its runs.
* Reference Manuals — the theory and numerics behind each formulation, with the
  status of each alternative marked Implemented, Experimental, Planned or Retired.
    * Hydrology Reference Manual (@ref hydrology_reference_manual) — meteorology,
      runoff, infiltration, groundwater, snowmelt, RDII, and the distributed
      surface and subsurface processes on the 2D mesh.
    * Hydraulics Reference Manual (@ref hydraulics_reference_manual) — the four flow
      routers, pressurisation, cross-sections, pumps and regulators, the explicit
      finite-volume solver and two-dimensional overland flow.
    * Quality Reference Manual (@ref quality_reference_manual) — buildup, washoff,
      treatment, LID controls, the three transport engines, multi-species reactions,
      water age and heat, and surface quality on the mesh.
* C API Reference
    * The OpenSWMM Engine v6 C API is organized by domain into 31 public headers.
      See the [Modules](modules.html) page for the full API reference generated from
      the `include/openswmm/engine/` headers.
    * Start with @ref openswmm_engine.h for the engine lifecycle, then explore
      domain-specific headers such as @ref openswmm_nodes.h, @ref openswmm_links.h,
      and @ref openswmm_model.h.
* Changelog — see [CHANGELOG.md](../../CHANGELOG.md) for the complete list of
  changes in each release.