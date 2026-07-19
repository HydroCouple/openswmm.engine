/**
 * @file Default2DOutputPlugin.hpp
 * @brief Built-in HDF5 output plugin for 2D surface routing results.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#pragma once

#ifdef OPENSWMM_HAS_2D

#include "../../../../include/openswmm/plugin_sdk/IOutputPlugin.hpp"

#include <string>
#include <hdf5.h>

namespace openswmm::twoD {

struct MeshData;  // forward declaration

/**
 * @brief Default 2D output plugin: writes CF/UGRID-compliant HDF5 file.
 *
 * @details Injected automatically when `2D_OUTPUT_FILE` is specified in
 *          `[2D_OPTIONS]`. Does NOT require a `[PLUGINS]` entry.
 *
 * ## HDF5 File Layout (CF-1.11 / UGRID-1.0)
 *
 * ### Root attributes
 *   - Conventions = "CF-1.11 UGRID-1.0"
 *
 * ### Static mesh topology (written once in prepare())
 *   - /Mesh2                         topology variable (cf_role = "mesh_topology")
 *   - /Mesh2_node_x, /Mesh2_node_y  vertex coordinates
 *   - /Mesh2_node_z                  vertex elevation
 *   - /Mesh2_face_nodes              face-node connectivity [nFace, 3]
 *   - /Mesh2_face_x, /Mesh2_face_y  face centroids
 *   - /Mesh2_face_z                  face centroid elevation
 *   - /Mesh2_face_mannings_n         Manning's roughness
 *   - /Mesh2_edge_length             edge lengths [nFace, 3]
 *
 * ### Time-varying fields (appended each update())
 *   - /time                          [nTime] seconds since simulation start
 *   - /Mesh2_face_depth              [nTime, nFace] overland flow depth (m)
 *   - /Mesh2_face_head               [nTime, nFace] total head (m)
 *   - /Mesh2_face_grad_hx            [nTime, nFace] head gradient dh/dx
 *   - /Mesh2_face_grad_hy            [nTime, nFace] head gradient dh/dy
 *   - /Mesh2_face_grad_hx_lim        [nTime, nFace] limited gradient dh/dx
 *   - /Mesh2_face_grad_hy_lim        [nTime, nFace] limited gradient dh/dy
 *   - /Mesh2_face_rainfall           [nTime, nFace] rainfall (m/s)
 *   - /Mesh2_face_coupling_flux      [nTime, nFace] node coupling (m/s)
 *   - /Mesh2_face_net_source         [nTime, nFace] net source/sink (m/s)
 *   - /Mesh2_face_vx                 [nTime, nFace] cell velocity X (m/s)
 *   - /Mesh2_face_vy                 [nTime, nFace] cell velocity Y (m/s)
 *   - /Mesh2_face_continuity_err     [nTime, nFace] per-cell continuity residual (m³/s)
 *   - /Mesh2_edge_flux               [nTime, nFace, 3] edge normal flux
 *   - /Mesh2_node_head               [nTime, nNode] reconstructed vertex head (m)
 *
 * ### Cumulative rendering envelopes (fixed [nFace], overwritten each update())
 *   - /Mesh2_face_max_depth          [nFace] max overland depth (m)
 *   - /Mesh2_face_max_velocity       [nFace] max cell speed |v| (m/s)
 *   - /Mesh2_face_max_continuity_err [nFace] max |continuity residual| (m³/s)
 *
 * ### Global mass balance (written once in finalize())
 *   - /mass_balance_2d               group of scalar terms (m³) + @continuity_error
 *
 * @ingroup engine_2d
 */
class Default2DOutputPlugin final : public IOutputPlugin {
public:
    /**
     * @brief Construct with resolved output file path.
     * @param h5_path  Absolute or resolved path for the .h5 output file.
     */
    explicit Default2DOutputPlugin(std::string h5_path);
    ~Default2DOutputPlugin() override;

    // Non-copyable
    Default2DOutputPlugin(const Default2DOutputPlugin&) = delete;
    Default2DOutputPlugin& operator=(const Default2DOutputPlugin&) = delete;

    // -----------------------------------------------------------------------
    // IOutputPlugin interface
    // -----------------------------------------------------------------------

    PluginState state() const noexcept override { return state_; }

    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    int validate(const SimulationContext& ctx) override;

    int prepare(const SimulationContext& ctx) override;

    int update(const SimulationSnapshot& snapshot) override;

    int finalize(const SimulationContext& ctx) override;

    const char* last_error_message() const noexcept override {
        return last_error_.c_str();
    }

    /**
     * @brief Write static mesh topology and create time-varying datasets.
     *
     * @details Called by SWMMEngine after 2D initialization, when the mesh
     *          is fully built. Must be called between prepare() and the first
     *          update().
     *
     * @param mesh  The 2D mesh data (vertices, triangles, properties).
     */
    void prepareMeshAndDatasets(const MeshData& mesh);

private:
    std::string  h5_path_;
    PluginState  state_ = PluginState::UNLOADED;
    std::string  last_error_;

    hid_t file_id_   = H5I_INVALID_HID;  ///< HDF5 file handle

    // Dataset handles for time-varying fields (unlimited time dimension)
    hid_t ds_time_                 = H5I_INVALID_HID;
    hid_t ds_face_depth_           = H5I_INVALID_HID;
    hid_t ds_face_head_            = H5I_INVALID_HID;
    hid_t ds_face_grad_hx_         = H5I_INVALID_HID;
    hid_t ds_face_grad_hy_         = H5I_INVALID_HID;
    hid_t ds_face_grad_hx_lim_     = H5I_INVALID_HID;
    hid_t ds_face_grad_hy_lim_     = H5I_INVALID_HID;
    hid_t ds_face_rainfall_        = H5I_INVALID_HID;
    hid_t ds_face_coupling_flux_   = H5I_INVALID_HID;
    hid_t ds_face_net_source_      = H5I_INVALID_HID;
    hid_t ds_face_vx_              = H5I_INVALID_HID;
    hid_t ds_face_vy_              = H5I_INVALID_HID;
    hid_t ds_face_continuity_err_  = H5I_INVALID_HID;
    hid_t ds_edge_flux_            = H5I_INVALID_HID;
    hid_t ds_node_head_            = H5I_INVALID_HID;
    hid_t ds_node_depth_           = H5I_INVALID_HID;

    // Cumulative envelopes — fixed [nFace], overwritten in place each update()
    hid_t ds_face_max_depth_          = H5I_INVALID_HID;
    hid_t ds_face_max_velocity_       = H5I_INVALID_HID;
    hid_t ds_face_max_continuity_err_ = H5I_INVALID_HID;

    hsize_t n_faces_  = 0;
    hsize_t n_nodes_  = 0;
    hsize_t n_steps_  = 0;  ///< Current time step count (grows with each update)

    // Helpers
    void writeMeshTopology(const SimulationContext& ctx);
    hid_t createUnlimitedDataset(const char* name, int rank,
                                  const hsize_t* dims,
                                  const hsize_t* chunk_dims);
    void writeStringAttr(hid_t loc, const char* name, const char* value);
    void writeDoubleAttr(hid_t loc, const char* name, double value);
    void extendAndWrite2D(hid_t ds, const double* data, hsize_t n_cols);
    void extendAndWrite3D(hid_t ds, const double* data, hsize_t dim1, hsize_t dim2);
    /// Create a fixed [nFace] face dataset (no time dimension) for an envelope.
    hid_t createFaceEnvelopeDataset(const char* name, const char* long_name,
                                    const char* units);
    /// Overwrite a fixed [nFace] dataset in place with the latest envelope.
    void writeFaceEnvelope(hid_t ds, const double* data);
};

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
