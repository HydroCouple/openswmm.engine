// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      Default2DOutputPlugin.hpp
 * @brief     Built-in HDF5 output plugin for 2D surface routing results.
 *
 * @details Writes 2D mesh geometry and time-varying state to an HDF5 file
 *          following CF-1.11 and UGRID-1.0 conventions for unstructured
 *          triangular meshes. The file is suitable for visualization in
 *          ParaView, QGIS, or any CF/UGRID-aware tool.
 *
 * @see IOutputPlugin.hpp
 * @see SolverOptions2D.hpp — output_file configuration
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once

#ifdef OPENSWMM_HAS_2D

#include "../../../../include/openswmm/plugin_sdk/IOutputPlugin.hpp"

#include "../../io/CheckpointOutputPublication.hpp"
#include "../../io/CheckpointFile.hpp"
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <hdf5.h>

namespace openswmm {
class SWMMEngine;
}
namespace openswmm::twoD {

struct InertialEdges;
class MomentumPhysics2D;
struct MeshData;        // forward declaration
struct SolverOptions2D; // forward declaration
class SourceResultHistory;

/**
 * @brief Default 2D output plugin: writes CF/UGRID-compliant HDF5 file.
 *
 * @details Injected automatically when `2D_OUTPUT_FILE` is specified in `[2D_OPTIONS]`. Does NOT
 * require a `[PLUGINS]` entry.
 *
 * ## HDF5 File Layout (CF-1.11 / UGRID-1.0)
 *
 * ### Root attributes
 * - Conventions = "CF-1.11 UGRID-1.0"
 *
 * ### Coordinate reference (written once in prepareMeshAndDatasets())
 * - /crs                           scalar georeferencing variable, pointed at by the
 * `openswmm_crs` attribute on /Mesh2 and the node/face coordinate variables.
 *
 * All x/y coordinates in this file are stored in **SI metres**, because the 2D solver runs
 * internally in SI. The model itself may be authored in a different linear unit (US FLOW_UNITS
 * projects are in feet, and their CRS is typically a foot-based projected CRS such as
 * EPSG:2249). The `/crs` variable makes that relationship explicit rather than leaving a
 * consumer to assume the coordinates are already in the model CRS's unit:
 *
 * - `model_crs`             the model's CRS as authored in `[OPTIONS] CRS` (e.g. "EPSG:2249", a
 * PROJ string, or WKT). This is the CRS of the MODEL coordinates, not of the metric values
 * stored here.
 * - `metres_per_model_unit` stored = model x this; model = stored / this. Precisely: the factor
 * SurfaceRouter2D::initialize actually applied — 0.3048 for US FLOW_UNITS, and 1.0 for SI
 * projects AND for any mesh that declared `;; UNITS: SI (m)` (the engine skips its scaling
 * there, so the stored metres already ARE the authored values).
 * - `units`                 "m" — the unit of the stored coordinates.
 *
 * Deliberately NOT a CF grid mapping — not `grid_mapping_name`, not `spatial_ref`/`crs_wkt`, and
 * the pointer attribute is `openswmm_crs` rather than CF's `grid_mapping`. A generic CF/GDAL
 * reader honouring those would place the metric values in the foot-based CRS and reproduce the
 * very offset this variable exists to describe; and a `grid_mapping` pointing at a variable with
 * no `grid_mapping_name` is itself non-conformant, so strict readers could warn or drop the
 * coordinate variables. The OpenSWMM- namespaced pair `model_crs` + `metres_per_model_unit`
 * cannot be half-consumed: neither means anything without the other. See issue #155.
 *
 * ### Static mesh topology (written once in prepare())
 * - /Mesh2                         topology variable (cf_role = "mesh_topology")
 * - /Mesh2_node_x, /Mesh2_node_y  vertex coordinates
 * - /Mesh2_node_z                  vertex elevation
 * - /Mesh2_face_nodes              cell-node connectivity [nFace, edgeStride]
 * - /Mesh2_face_nv                 vertex count per cell (triangles/quads)
 * - /Mesh2_face_x, /Mesh2_face_y  face centroids
 * - /Mesh2_face_z                  face centroid elevation
 * - /Mesh2_face_mannings_n         Manning's roughness
 * - /Mesh2_edge_length             edge lengths [nFace, edgeStride]
 *
 * ### Time-varying fields (appended each update())
 * - /time                          [nTime] days since 1899-12-30 00:00:00
 * - /Mesh2_face_depth              [nTime, nFace] overland flow depth (m)
 * - /Mesh2_face_head               [nTime, nFace] total head (m)
 * - /Mesh2_face_grad_hx            [nTime, nFace] head gradient dh/dx
 * - /Mesh2_face_grad_hy            [nTime, nFace] head gradient dh/dy
 * - /Mesh2_face_grad_hx_lim        [nTime, nFace] limited gradient dh/dx
 * - /Mesh2_face_grad_hy_lim        [nTime, nFace] limited gradient dh/dy
 * - /Mesh2_face_rainfall           [nTime, nFace] rainfall (m/s)
 * - /Mesh2_face_coupling_flux      [nTime, nFace] node coupling (m/s)
 * - /Mesh2_face_net_source         [nTime, nFace] net source/sink (m/s)
 * - /Mesh2_face_infil_rate         [nTime, nFace] held infiltration rate (m/s)
 * - /Mesh2_face_infil_cum          [nTime, nFace] cumulative infiltrated depth (m)
 * - /Mesh2_face_rain_cum           [nTime, nFace] cumulative rainfall volume (m³)
 * - /Mesh2_face_vx                 [nTime, nFace] cell velocity X (m/s)
 * - /Mesh2_face_vy                 [nTime, nFace] cell velocity Y (m/s)
 * - /Mesh2_face_continuity_err     [nTime, nFace] per-cell continuity residual (m³/s)
 * - /Mesh2_edge_flux               [nTime, nFace, edgeStride] edge normal flux
 * - /Mesh2_node_head               [nTime, nNode] reconstructed vertex head (m)
 *
 * ### Cumulative rendering envelopes (fixed [nFace], overwritten each update())
 * - /Mesh2_face_max_depth          [nFace] max overland depth (m)
 * - /Mesh2_face_max_velocity       [nFace] max cell speed |v| (m/s)
 * - /Mesh2_face_max_continuity_err [nFace] max |continuity residual| (m³/s)
 *
 * ### Global mass balance (written once in finalize())
 * - /mass_balance_2d               group of scalar terms (m³) + `continuity_error`
 *
 * @ingroup engine_2d
 */
class Default2DOutputPlugin final : public IOutputPlugin {
public:
    /**
     * @brief Construct with resolved output file path.
     * - Argument h5_path: Absolute or resolved path for the .h5 output file.
     */
    explicit Default2DOutputPlugin(std::string h5_path);
    ~Default2DOutputPlugin() override;

    // Non-copyable
    Default2DOutputPlugin(const Default2DOutputPlugin&) = delete;
    Default2DOutputPlugin& operator=(const Default2DOutputPlugin&) = delete;

    // -----------------------------------------------------------------------
    // IOutputPlugin interface
    // -----------------------------------------------------------------------

    /**
     * @brief Read the current plugin lifecycle state.
     * @return Stored UNLOADED/INITIALIZED/VALIDATED/PREPARED/FINALIZED/ERROR state. This does
     * not probe file handles, flush pending output or clear a prior error. Caller serializes
     * with lifecycle writes.
     */
    PluginState state() const noexcept override { return state_; }

    /**
     * @brief Mark the built-in writer initialized without opening the output file.
     * @param[in] init_args Unused plugin arguments; output path was supplied to the constructor.
     * @param[in] info Unused component metadata pointer; null is accepted and never
     * dereferenced.
     * @return Zero after setting INITIALIZED. Does not validate path, reset datasets or clear
     * old errors; call once in the normal lifecycle with exclusive owner access.
     */
    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    /**
     * @brief Validate that the configured output path is nonempty.
     * @param[in] ctx Retained unused interface argument; no engine state is read.
     * @return Zero and VALIDATED for nonempty path; one, ERROR and diagnostic for empty path.
     * Does not check path existence, permissions, HDF5 support or dataset configuration.
     * Exclusive access.
     */
    int validate(const SimulationContext& ctx) override;

    /**
     * @brief Create the results file and copy CRS/native species metadata for subsequent dataset
     * setup.
     * @param[in] ctx Prepared context with matching names/units and preferred spatial/options
     * CRS; borrowed only during the call and copied into writer-owned metadata.
     * @return Zero and PREPARED on successful tracked preparation; one and ERROR on create
     * failure or checkpoint-fresh tracked write failure.
     * @note Serializes HDF5 through the global recursive result mutex. Normal mode truncates an
     * existing path; checkpoint_fresh mode requires exclusive creation. Writes root attributes
     * but no mesh yet; prepareMeshAndDatasets() follows. Metadata allocation/name lookup can
     * throw after file creation. Normal-mode HDF5 attribute failures are not all promoted to
     * return failure; no rollback is offered. Requires validated single lifecycle; repeated
     * prepare without closing the old file is unsupported.
     */
    int prepare(const SimulationContext& ctx) override;

    /**
     * @brief Publish one due immutable surface snapshot to the open HDF5 result.
     * - Argument snapshot (in): Borrowed stable snapshot; absolute DateTime days and SI mesh
     * arrays, with matching immutable species/thermal/layer identity. Definition uses snap and
     * details full extents.
     * @return Zero on success/skip, one on reported failure. May leave a partially extended row
     * or advanced cadence after late refusal; inspect state/error. Holds global HDF5 mutex and
     * can allocate.
     */
    int update(const SimulationSnapshot& snapshot) override;

    /**
     * @brief Drain accepted histories, write final water ledgers and close all result resources.
     * @param[in] ctx Stable completed context with final SI surface/aquifer balance terms,
     * borrowed.
     * @return Zero and FINALIZED if no prior/source/fresh-checkpoint tracked failure; one and
     * ERROR otherwise. Closes datasets/file even when prior state was ERROR.
     * @note Holds global result mutex, writes optional surface/aquifer scalar groups then closes
     * handles. Does not advance simulation or reset cumulative ledgers. No idempotence guarantee
     * for repeated final writes to an open group; normal HDF5 failures are not all promoted to
     * error. Temporary allocation or history exceptions may escape before closure; destructor
     * later closes remaining resources.
     */
    int finalize(const SimulationContext& ctx) override;

    /**
     * @brief Borrow the most recently stored output diagnostic.
     * @return Nonnull NUL-terminated string owned by this object, empty if none stored. Any
     * later error, mutation or destruction may invalidate its address; copy before that point.
     * No error reset, file access or synchronization occurs in this getter.
     */
    const char* last_error_message() const noexcept override { return last_error_.c_str(); }

    /**
     * @brief Write static mesh topology and create time-varying datasets.
     *
     * @details Called by SWMMEngine after 2D initialization, when the mesh is fully built. Must
     * be called between prepare() and the first update().
     *
     * @param mesh  The 2D mesh data (vertices, triangles, properties).
     *
     * @note Requires a successfully prepared open file, positive consistent mesh counts/shapes
     * and prior output/coordinate configuration. Writes SI static topology, CRS,
     * precision/filter metadata and selected extensible fields plus fixed envelopes. HDF5 calls
     * hold the global result mutex. Stores owned dataset handles/counts; no mesh pointer is
     * retained. Not idempotent: names already exist after first call. Allocation or HDF5 failure
     * can leave partial datasets; fresh-checkpoint mode tracks writes and captures a header
     * image, reporting failure through state_/last_error_. Other HDF5 setup statuses are not all
     * checked. Callers inspect state before enabling output.
     */
    void prepareMeshAndDatasets(const MeshData& mesh);

    /**
     * @brief Declare the factor already applied to the stored coordinates.
     *
     * @details The model CRS reaches the plugin through prepare()'s SimulationContext, but the
     * mesh scale factor does not — SurfaceRouter2D owns it. SWMMEngine calls this between
     * prepare() and prepareMeshAndDatasets(). Left uncalled, `/crs` reports 1.0, i.e. it claims
     * nothing it cannot substantiate.
     *
     * @param metres_per_model_unit Metres per model-CRS linear unit — the factor already applied
     * to the stored coordinates (0.3048 for US, 1.0 for SI).
     *
     * @note Stores any value greater than zero, including positive infinity; nonpositive/NaN
     * leaves the previous factor. Caller supplies a finite correct conversion and invokes before
     * static CRS write. Does not scale geometry, rewrite the file or validate a CRS; no
     * allocation occurs.
     */
    void setMeshCoordinateScale(double metres_per_model_unit);

    /**
     * @brief Apply the results-file size controls from [2D_OPTIONS] (OUTPUT_PRECISION,
     * OUTPUT_COMPRESSION, REPORT_2D_VARIABLES, REPORT_2D_SPECIES, REPORT_2D_STEP). Called by
     * SWMMEngine before prepareMeshAndDatasets(); datasets not selected are never created, so
     * readers must treat every time-varying dataset as optional.
     * @param report_step_sec  [OPTIONS] REPORT_STEP, the cadence update() is called at;
     * REPORT_2D_STEP is validated against it.
     * @return Empty on success, else a validation message (REPORT_2D_STEP not a positive
     * multiple of REPORT_STEP).
     *
     * @param[in] opts Resolved precision, compression, variable mask, species names and report
     * stride. Copies selections, clamps compression to [0,9], forces DEPTH on and clears cached
     * species row mapping.
     * @note Mutates precision/filter/cadence fields before report multiple validation; a
     * returned error does not restore previous configuration. Positive REPORT_2D_STEP is
     * converted s-to-days; a nonpositive report_step_sec skips the multiple check. Caller
     * supplies finite values and configures before creating datasets. Does not rewrite an open
     * file or change solver physics; string/vector copying may throw and all configuration
     * access must be serialized.
     */
    std::string configureOutput(const SolverOptions2D& opts, double report_step_sec);

    /**
     * @brief Bitmask of report2d::Var groups in effect (for tests / the C API).
     *
     * @return Current effective report2d::Var mask, with DEPTH forced when configureOutput()
     * ran. Read-only and allocation-free; no file probing or synchronization. Requires stable
     * configuration.
     */
    unsigned reportVariables() const noexcept { return report_vars_; }

    /**
     * @brief Producer handoff of immutable accepted source histories; HDF5 remains exclusively
     * on the output owner thread. Optional internal hook, no automatic runtime ownership
     * activation or plugin SDK payload change.
     *
     * @param[in] history Nonnull shared immutable accepted-history snapshot. Queued ownership is
     * retained until the output thread drains it; producer may release its own shared pointer
     * after handoff.
     * @throws std::invalid_argument Null history.
     * @note Locks the source queue only and performs no HDF5 work. Allocation can throw. The
     * history must remain immutable and extend prior published history; extension validation
     * happens during drain. Producer enqueue can overlap owner-thread writing, but
     * lifecycle/object destruction remains serialized.
     */
    void enqueueSourceHistory(std::shared_ptr<const SourceResultHistory> history);

private:
    friend class openswmm::SWMMEngine;
    // Internal single-file, water-only continuation. IO must be idle.
    bool checkpoint_fresh_ = false;
    bool checkpoint_write_failed_ = false;
    void checkpointWriteStatus(herr_t);
    std::string checkpoint_header_;
    struct PreparedCheckpoint;
    std::string checkpointBytes(const SimulationContext&, double next_report_ms,
                                const SimulationSnapshot&);
    std::shared_ptr<PreparedCheckpoint> prepareCheckpoint(const SimulationContext&,
                                                          double next_report_ms,
                                                          const SimulationSnapshot&,
                                                          const SourceResultHistory*, std::string);
    void installCheckpoint(PreparedCheckpoint&);
    io::CheckpointOutputPublication::Output checkpointPublication(const PreparedCheckpoint&) const;
    void adoptCheckpoint(PreparedCheckpoint&) noexcept;
    void validateCheckpoint(hid_t, const SimulationContext&, double, const SimulationSnapshot&,
                            hsize_t, double, bool) const;
    std::vector<hid_t*> checkpointDatasets();
    void closeDatasets() noexcept;
    std::string fileBytes() const;
    std::string captureFileCheckpoint(const SimulationContext&, double, const SimulationSnapshot&,
                                      const std::filesystem::path&,
                                      std::shared_ptr<io::OwnedCheckpointFile>&);
    std::shared_ptr<PreparedCheckpoint>
    prepareFileCheckpoint(const SimulationContext&, double, const SimulationSnapshot&, std::string);
    void resolveCheckpointDatasets(PreparedCheckpoint&, const SimulationSnapshot&);
    std::mutex source_mutex_;
    std::vector<std::shared_ptr<const SourceResultHistory>> source_pending_;
    std::shared_ptr<const SourceResultHistory> source_written_;
    int writePendingSourceHistory();
    // ---- results-file size controls (configureOutput) ----
    hid_t storage_type_ = H5T_NATIVE_DOUBLE;  ///< H5T_IEEE_F32LE when FLOAT32
    int compression_ = 4;                     ///< zlib level; 0 = none
    unsigned report_vars_ = 0x7FFu;           ///< legacy direct-writer default until configured
    std::vector<std::string> species_filter_; ///< empty = every row
    std::vector<int> species_rows_;           ///< row indices written (resolved on first update)
    double report_2d_step_days_ = 0.0;        ///< 0 = every update() call
    double next_due_days_ = -1.0;             /**
     * @brief < next write instant (days)
     *
     * @details Test whether any requested reporting bit is enabled.
     * @param[in] bit report2d::Var bit or bit set.
     * @return (report_vars_ & bit)!=0, rather than requiring every supplied bit. No file access,
     * allocation, configuration mutation or synchronization occurs.
     */
    bool want(unsigned bit) const noexcept { return (report_vars_ & bit) != 0; }
    /**
     * @brief Write \p n_sel selected species rows of a [n_species × n_faces] block.
     *
     * @param[in] all Nonnull readable row-major [n_species_in*n_faces_] native concentration
     * doubles.
     * @param[in] n_species_in Input row count; resolved species_rows_ selection and n_species_
     * must match the intended immutable layout. Products must fit memory extents.
     * @note Writes directly for identity mapping, otherwise allocates a selected block; invalid
     * selected positive rows remain zero. Writes the n_steps_ record without advancing it.
     * Caller holds global result mutex and valid species dataset. Allocation may throw; HDF5
     * write can leave partial extension. Does not change input inventory, filter names or units.
     */
    void writeSpeciesRows(const double* all, hsize_t n_species_in);

    std::string h5_path_;
    PluginState state_ = PluginState::UNLOADED;
    std::string last_error_;

    std::unordered_map<std::string, std::string>
        native_species_units_;           // keyed by identity, not surface row
    std::string model_crs_;              ///< `[OPTIONS] CRS`, verbatim
    double metres_per_model_unit_ = 1.0; ///< stored = model x this

    hid_t file_id_ = H5I_INVALID_HID; ///< HDF5 file handle

    // Dataset handles for time-varying fields (unlimited time dimension)
    hid_t ds_time_ = H5I_INVALID_HID;
    hid_t ds_face_depth_ = H5I_INVALID_HID;
    hid_t ds_face_head_ = H5I_INVALID_HID;
    hid_t ds_face_grad_hx_ = H5I_INVALID_HID;
    hid_t ds_face_grad_hy_ = H5I_INVALID_HID;
    hid_t ds_face_grad_hx_lim_ = H5I_INVALID_HID;
    hid_t ds_face_grad_hy_lim_ = H5I_INVALID_HID;
    hid_t ds_face_rainfall_ = H5I_INVALID_HID;
    hid_t ds_face_coupling_flux_ = H5I_INVALID_HID;
    hid_t ds_face_net_source_ = H5I_INVALID_HID;
    hid_t ds_face_infil_rate_ = H5I_INVALID_HID;
    hid_t ds_face_infil_cum_ = H5I_INVALID_HID;
    hid_t ds_face_rain_cum_ = H5I_INVALID_HID;
    std::array<hid_t, meshSnowFields.size()> ds_snow_ = [] {
        std::array<hid_t, meshSnowFields.size()> ids;
        ids.fill(H5I_INVALID_HID);
        return ids;
    }();
    void createSnowDatasets(const SimulationSnapshot&);

    /// Overland transport S1: [nTime, nSpecies, nFace] species concentration.
    /// Created lazily on the first update() carrying species; absent when the
    /// model has no 2D transport.
    hid_t ds_face_species_conc_ = H5I_INVALID_HID;
    hsize_t n_species_ = 0;
    /// S7: [nTime, nSurfaceSpecies, nFace] cell buildup per unit land area
    /// (the pollutant and MSX rows). Created lazily; absent when no
    /// [2D_COVERAGES] row resolved or BUILDUP is off.
    hid_t ds_face_buildup_ = H5I_INVALID_HID;
    hsize_t n_buildup_ = 0;
    /// G-O: the two-zone aquifer's results — ten per-cell [nTime, nFace]
    /// fields, the domain ledger [nTime, 11], the per-bed cumulative node
    /// exchange [nTime, nBeds] and (GW_DETAILED) the σ columns
    /// [nTime, mLayers, nFace]. Created lazily on the first update() with a
    /// live kernel; absent otherwise. `Mesh2_face_gw_bed_elev` and
    /// `Mesh2_face_gw_closure` are static [nFace] and written once.
    static constexpr int kGwFaceFields = 27;      // G-X3: + Mesh2_face_gw_link_seepage
    static constexpr hsize_t kGwLedgerTerms = 15; // G-X3: + link (LED 10), residual last
    hid_t ds_gw_face_[kGwFaceFields] = {
        H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID,
        H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID,
        H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID,
        H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID,
        H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID, H5I_INVALID_HID,
        H5I_INVALID_HID, H5I_INVALID_HID};
    hid_t ds_gw_ledger_ = H5I_INVALID_HID;
    /// T7.5: the aquifer's species fields, created with the rest of the
    /// groundwater group when the kernel carries a tuple.
    static constexpr hsize_t kGwSpeciesLedgerTerms = 15;
    hid_t ds_gw_sat_conc_ = H5I_INVALID_HID;
    hid_t ds_gw_unsat_conc_ = H5I_INVALID_HID;
    hid_t ds_gw_species_ledger_ = H5I_INVALID_HID;
    hsize_t n_gw_species_ = 0;
    hid_t ds_gw_bed_exchange_ = H5I_INVALID_HID;
    hid_t ds_gw_theta_ = H5I_INVALID_HID;
    hsize_t n_gw_beds_ = 0;
    hsize_t n_gw_layers_ = 0;
    hid_t ds_gw_layer_mass_ = H5I_INVALID_HID, ds_gw_layer_conc_ = H5I_INVALID_HID;
    hid_t ds_gw_boundary_water_ = H5I_INVALID_HID, ds_gw_boundary_species_ = H5I_INVALID_HID;
    hid_t ds_gw_transport_ledger_ = H5I_INVALID_HID;
    hsize_t n_gw_transport_layers_ = 0, n_gw_boundaries_ = 0;
    bool gw_created_ = false;
    void createGroundwaterDatasets(const SimulationSnapshot& snap);
    bool heat_created_ = false;
    hsize_t n_heat_layers_ = 0;
    hsize_t n_heat_lid_layers_ = 0;
    hid_t ds_heat_water_temp_ = H5I_INVALID_HID;
    hid_t ds_heat_bed_temp_ = H5I_INVALID_HID;
    hid_t ds_heat_bed_energy_ = H5I_INVALID_HID;
    hid_t ds_heat_bed_initial_energy_ = H5I_INVALID_HID;
    hid_t ds_heat_bed_capacity_ = H5I_INVALID_HID;
    hid_t ds_heat_bed_pore_volume_ = H5I_INVALID_HID;
    hid_t ds_heat_bed_species_conc_ = H5I_INVALID_HID;
    hid_t ds_heat_bed_species_initial_amount_ = H5I_INVALID_HID;
    hid_t ds_heat_bed_species_exchange_ = H5I_INVALID_HID;
    hid_t ds_heat_bed_age_generation_ = H5I_INVALID_HID;
    hsize_t n_heat_bed_species_ = 0;
    hid_t ds_heat_surface_budget_ = H5I_INVALID_HID;
    hid_t ds_heat_evaporation_ = H5I_INVALID_HID;
    hid_t ds_heat_ground_temp_ = H5I_INVALID_HID;
    hid_t ds_heat_ground_energy_ = H5I_INVALID_HID;
    hid_t ds_heat_ground_capacity_ = H5I_INVALID_HID;
    hid_t ds_heat_layer_temp_ = H5I_INVALID_HID;
    hid_t ds_heat_layer_energy_ = H5I_INVALID_HID;
    hid_t ds_heat_layer_capacity_ = H5I_INVALID_HID;
    hid_t ds_heat_ground_budget_ = H5I_INVALID_HID;
    hid_t ds_heat_lid_temp_ = H5I_INVALID_HID;
    hid_t ds_heat_lid_energy_ = H5I_INVALID_HID;
    hid_t ds_heat_lid_capacity_ = H5I_INVALID_HID;
    hid_t ds_heat_lid_ground_contact_ = H5I_INVALID_HID;
    void createThermalDatasets(const SimulationSnapshot& snap);
    const MeshData* mixing_mesh_ = nullptr; // borrowed immutable prepared geometry
    std::unique_ptr<InertialEdges> mixing_edges_;
    std::unique_ptr<MomentumPhysics2D> mixing_diagnostics_;
    bool mixing_enabled_ = false, momentum_velocity_ = false;
    double mixing_cs_ = .1, mixing_dry_ = .001, coriolis_f_ = 0;
    hid_t ds_face_eddy_viscosity_ = H5I_INVALID_HID;
    hid_t ds_face_strain_rate_ = H5I_INVALID_HID;
    hid_t ds_face_vx_ = H5I_INVALID_HID;
    hid_t ds_face_vy_ = H5I_INVALID_HID;
    hid_t ds_face_continuity_err_ = H5I_INVALID_HID;
    hid_t ds_edge_flux_ = H5I_INVALID_HID;
    hid_t ds_node_head_ = H5I_INVALID_HID;
    hid_t ds_node_depth_ = H5I_INVALID_HID;

    // Cumulative envelopes — fixed [nFace], overwritten in place each update()
    hid_t ds_face_max_depth_ = H5I_INVALID_HID;
    hid_t ds_face_max_velocity_ = H5I_INVALID_HID;
    hid_t ds_face_max_continuity_err_ = H5I_INVALID_HID;

    hsize_t n_faces_ = 0;
    hsize_t n_nodes_ = 0;
    hsize_t edge_stride_ = 3; ///< Public edge-slot stride (3 all-tri, 4 mixed)
    hsize_t n_steps_ = 0;     ///< Current time step count (grows with each update)

    // Helpers
    /**
     * @brief Retain the legacy no-op static-topology hook.
     * @param[in] ctx Unused interface context; no mesh or engine state is read.
     * @note Performs no file write or state change. Real topology is written by
     * prepareMeshAndDatasets() through writeMeshToHDF5(); this hook alone does not prepare
     * output datasets.
     */
    void writeMeshTopology(const SimulationContext& ctx);
    /**
     * @brief Write the scalar `/crs` georeferencing variable (see class docs).
     *
     * @note Requires global HDF5 result mutex and prepared file/CRS scale metadata. No-op for
     * invalid file; otherwise creates the scalar CRS descriptor and model-to-SI attributes, then
     * closes local handles. A failed dataset creation returns silently. Does not convert stored
     * mesh coordinates or claim a CF grid mapping; HDF5 write tracking applies only in
     * fresh-checkpoint mode.
     */
    void writeCrsVariable();
    /**
     * @param type  Storage type; -1 (the default) follows OUTPUT_PRECISION. The time axis passes
     * H5T_NATIVE_DOUBLE explicitly: a day number in float32 loses ~0.1 ms on an hour, which the
     * readers' time lookup cannot absorb.
     *
     * @brief Create a chunked field with unlimited leading time dimension in the owned results
     * file.
     * @param[in] name Nonnull terminated unused dataset path/name.
     * @param[in] rank Positive rank, with matching dims/chunk_dims extents; unchecked before
     * maxdims[0].
     * @param[in] dims Readable rank extents including current leading time length (usually
     * zero).
     * @param[in] chunk_dims Readable rank positive HDF5 chunk extents, valid for element
     * size/layout.
     * @return New owned dataset ID, to be closed by this plugin/caller, or negative HDF5 failure
     * ID.
     * @note Uses configured storage precision unless type overrides it, enables shuffle+deflate
     * for positive compression, and closes temporary dataspace/property IDs. Requires prepared
     * file and global result mutex held by caller. Allocates maxdims; no cleanup transaction on
     * allocation/HDF5 failure and setup return codes are not all promoted to checkpoint error.
     */
    hid_t createUnlimitedDataset(const char* name, int rank, const hsize_t* dims,
                                 const hsize_t* chunk_dims, hid_t type = -1);
    /**
     * @brief Create one scalar fixed-length HDF5 string attribute.
     * @param[in] loc Valid open HDF5 object, borrowed and unchanged in ownership.
     * @param[in] name Nonnull terminated new attribute name; existing-name replacement is
     * unsupported.
     * @param[in] value Nonnull terminated readable value. Empty string uses one NUL byte;
     * although length calculation tolerates null, the pointer is still passed to H5Awrite, so
     * callers provide nonnull text.
     * @note Uses strlen bytes for nonempty values and H5T_STR_NULLTERM padding; does not include
     * an extra terminator in that nonempty fixed extent. Closes temporary handles, tracks only
     * write status in fresh checkpoint mode and does not return errors. Caller holds global
     * result mutex and handles failure state.
     */
    void writeStringAttr(hid_t loc, const char* name, const char* value);
    /**
     * @brief Create a scalar native-double HDF5 attribute.
     * @param[in] loc Valid borrowed open HDF5 object.
     * @param[in] name Nonnull terminated new attribute name; no replacement handling.
     * @param[in] value Scalar in the attribute's caller-defined units; no finite validation.
     * @note Closes local handles and tracks H5Awrite only in fresh-checkpoint mode. Does not
     * throw/report ordinary HDF5 status itself or acquire the global result mutex; caller
     * supplies serialized access.
     */
    void writeDoubleAttr(hid_t loc, const char* name, double value);
    /**
     * @brief Extend one time-record field and write its current n_steps_ hyperslab.
     * @param[in] ds Valid borrowed dataset of the matching rank and unlimited leading dimension.
     * @param[in] data Nonnull readable row-major double buffer covering the product of all
     * trailing extents; values remain in caller-defined dataset units and HDF5 converts storage
     * precision.
     * @param[in] n_cols Fixed columns extent, matching the created dataset; products must fit
     * buffer/address limits.
     * @note Sets leading extent to n_steps_+1, writes row n_steps_, and closes local spaces.
     * Does not increment n_steps_ or acquire mutex. Caller holds global result mutex and ensures
     * matching shapes; no pointer/shape checks or rollback of extension on write failure.
     * Fresh-checkpoint mode records negative extent/select/write status; normal-mode errors are
     * otherwise left to HDF5.
     */
    void extendAndWrite2D(hid_t ds, const double* data, hsize_t n_cols);
    /**
     * @brief Extend one time-record field and write its current n_steps_ hyperslab.
     * @param[in] ds Valid borrowed dataset of the matching rank and unlimited leading dimension.
     * @param[in] data Nonnull readable row-major double buffer covering the product of all
     * trailing extents; values remain in caller-defined dataset units and HDF5 converts storage
     * precision.
     * @param[in] a Fixed first trailing dimension extent, matching the created dataset; products
     * must fit buffer/address limits.
     * @param[in] b Fixed second trailing dimension extent, matching the created dataset;
     * products must fit buffer/address limits.
     * @param[in] c Fixed third trailing dimension extent, matching the created dataset; products
     * must fit buffer/address limits.
     * @note Sets leading extent to n_steps_+1, writes row n_steps_, and closes local spaces.
     * Does not increment n_steps_ or acquire mutex. Caller holds global result mutex and ensures
     * matching shapes; no pointer/shape checks or rollback of extension on write failure.
     * Fresh-checkpoint mode records negative extent/select/write status; normal-mode errors are
     * otherwise left to HDF5.
     */
    void extendAndWrite4D(hid_t ds, const double* data, hsize_t a, hsize_t b, hsize_t c);
    /**
     * @brief Extend one time-record field and write its current n_steps_ hyperslab.
     * @param[in] ds Valid borrowed dataset of the matching rank and unlimited leading dimension.
     * @param[in] data Nonnull readable row-major double buffer covering the product of all
     * trailing extents; values remain in caller-defined dataset units and HDF5 converts storage
     * precision.
     * @param[in] dim1 Fixed first trailing dimension extent, matching the created dataset;
     * products must fit buffer/address limits.
     * @param[in] dim2 Fixed second trailing dimension extent, matching the created dataset;
     * products must fit buffer/address limits.
     * @note Sets leading extent to n_steps_+1, writes row n_steps_, and closes local spaces.
     * Does not increment n_steps_ or acquire mutex. Caller holds global result mutex and ensures
     * matching shapes; no pointer/shape checks or rollback of extension on write failure.
     * Fresh-checkpoint mode records negative extent/select/write status; normal-mode errors are
     * otherwise left to HDF5.
     */
    void extendAndWrite3D(hid_t ds, const double* data, hsize_t dim1, hsize_t dim2);
    /**
     * @brief Create a fixed [nFace] face dataset (no time dimension) for an envelope.
     *
     * @param[in] name Nonnull terminated unused dataset path/name.
     * @param[in] long_name Nonnull terminated descriptive attribute.
     * @param[in] units Nonnull terminated unit label, copied to the dataset attribute.
     * @return New owned fixed [n_faces_] dataset ID using configured precision, or negative on
     * HDF5 creation failure. Attaches mesh/location/time-maximum metadata and closes temporary
     * space.
     * @note Caller holds global result mutex with a prepared file and valid positive face count.
     * No chunking/compression or initial data write; later writeFaceEnvelope() publishes the
     * cumulative field. HDF5 setup failures are not all checked or rolled back.
     */
    hid_t createFaceEnvelopeDataset(const char* name, const char* long_name, const char* units);
    /**
     * @brief Overwrite a fixed [nFace] dataset in place with the latest envelope.
     *
     * @param[in] ds Borrowed fixed face dataset ID, or H5I_INVALID_HID for a no-op.
     * @param[in] data Nonnull readable n_faces_ doubles in dataset units, stable through HDF5
     * copy.
     * @note Replaces the full fixed field. Does not extend time or verify monotonicity; caller
     * holds global result mutex and supplies matching layout. Tracks write error in
     * fresh-checkpoint mode only; normal HDF5 write failure is not returned. No owner buffer is
     * retained.
     */
    void writeFaceEnvelope(hid_t ds, const double* data);
};

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
