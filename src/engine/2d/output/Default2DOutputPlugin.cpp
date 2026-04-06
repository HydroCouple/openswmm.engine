/**
 * @file Default2DOutputPlugin.cpp
 * @brief HDF5 output plugin for 2D surface routing results (CF/UGRID).
 *
 * @see Default2DOutputPlugin.hpp
 * @ingroup engine_2d
 */

#ifdef OPENSWMM_HAS_2D

#include "Default2DOutputPlugin.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../2d/SurfaceRouter2D.hpp"

#include <filesystem>
#include <cstring>

namespace openswmm::twoD {

// ============================================================================
// Helpers
// ============================================================================

void Default2DOutputPlugin::writeStringAttr(hid_t loc, const char* name,
                                             const char* value) {
    hid_t atype = H5Tcopy(H5T_C_S1);
    H5Tset_size(atype, std::strlen(value));
    H5Tset_strpad(atype, H5T_STR_NULLTERM);

    hid_t aspace = H5Screate(H5S_SCALAR);
    hid_t attr   = H5Acreate2(loc, name, atype, aspace, H5P_DEFAULT, H5P_DEFAULT);
    H5Awrite(attr, atype, value);
    H5Aclose(attr);
    H5Sclose(aspace);
    H5Tclose(atype);
}

hid_t Default2DOutputPlugin::createUnlimitedDataset(const char* name, int rank,
                                                      const hsize_t* dims,
                                                      const hsize_t* chunk_dims) {
    // Create dataspace with unlimited first dimension
    std::vector<hsize_t> maxdims(dims, dims + rank);
    maxdims[0] = H5S_UNLIMITED;
    hid_t space = H5Screate_simple(rank, dims, maxdims.data());

    // Enable chunking (required for unlimited dimensions)
    hid_t plist = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_chunk(plist, rank, chunk_dims);
    H5Pset_deflate(plist, 4);  // zlib compression level 4

    hid_t ds = H5Dcreate2(file_id_, name, H5T_NATIVE_DOUBLE, space,
                           H5P_DEFAULT, plist, H5P_DEFAULT);
    H5Pclose(plist);
    H5Sclose(space);
    return ds;
}

void Default2DOutputPlugin::extendAndWrite2D(hid_t ds, const double* data,
                                               hsize_t n_cols) {
    // Extend dataset by one row in the time dimension
    hsize_t new_dims[2] = { n_steps_ + 1, n_cols };
    H5Dset_extent(ds, new_dims);

    // Select the new row hyperslab
    hid_t filespace = H5Dget_space(ds);
    hsize_t offset[2] = { n_steps_, 0 };
    hsize_t count[2]  = { 1, n_cols };
    H5Sselect_hyperslab(filespace, H5S_SELECT_SET, offset, nullptr, count, nullptr);

    // Memory space for one row
    hid_t memspace = H5Screate_simple(2, count, nullptr);
    H5Dwrite(ds, H5T_NATIVE_DOUBLE, memspace, filespace, H5P_DEFAULT, data);
    H5Sclose(memspace);
    H5Sclose(filespace);
}

void Default2DOutputPlugin::extendAndWrite3D(hid_t ds, const double* data,
                                               hsize_t dim1, hsize_t dim2) {
    hsize_t new_dims[3] = { n_steps_ + 1, dim1, dim2 };
    H5Dset_extent(ds, new_dims);

    hid_t filespace = H5Dget_space(ds);
    hsize_t offset[3] = { n_steps_, 0, 0 };
    hsize_t count[3]  = { 1, dim1, dim2 };
    H5Sselect_hyperslab(filespace, H5S_SELECT_SET, offset, nullptr, count, nullptr);

    hid_t memspace = H5Screate_simple(3, count, nullptr);
    H5Dwrite(ds, H5T_NATIVE_DOUBLE, memspace, filespace, H5P_DEFAULT, data);
    H5Sclose(memspace);
    H5Sclose(filespace);
}

// ============================================================================
// Lifecycle
// ============================================================================

Default2DOutputPlugin::Default2DOutputPlugin(std::string h5_path)
    : h5_path_(std::move(h5_path)) {}

Default2DOutputPlugin::~Default2DOutputPlugin() {
    if (file_id_ != H5I_INVALID_HID) {
        H5Fclose(file_id_);
        file_id_ = H5I_INVALID_HID;
    }
}

int Default2DOutputPlugin::initialize(const std::vector<std::string>& /*init_args*/,
                                        const IPluginComponentInfo* /*info*/) {
    state_ = PluginState::INITIALIZED;
    return 0;
}

int Default2DOutputPlugin::validate(const SimulationContext& /*ctx*/) {
    if (h5_path_.empty()) {
        last_error_ = "Default2DOutputPlugin: empty output file path";
        state_ = PluginState::ERROR;
        return 1;
    }
    state_ = PluginState::VALIDATED;
    return 0;
}

// ============================================================================
// prepare() — write static mesh topology, create time-varying datasets
// ============================================================================

void Default2DOutputPlugin::writeMeshTopology(const SimulationContext& ctx) {
    // Access the 2D mesh via the surface router stored in context
    // Note: prepare() is called from main thread so this is safe
    // We need to get the mesh data from the context. The SurfaceRouter2D
    // is on SWMMEngine, but the mesh data should be accessible.
    // For now, we store mesh references during prepare via a temporary approach.
    // The mesh data pointers are stored in ctx for the 2D module.

    // Since we can't access SWMMEngine directly from a plugin, the mesh
    // geometry must come from the SimulationContext. Let's write what we
    // can access. For the initial implementation, we rely on the snapshot
    // having tri/vert counts and the mesh being accessible via the 2d module.

    // Actually, plugins receive SimulationContext in prepare(). We need
    // to add mesh data to the context or pass it differently.
    // For now, we'll write a minimal topology placeholder and populate
    // the real data via a dedicated method called from the engine.
    (void)ctx;
}

int Default2DOutputPlugin::prepare(const SimulationContext& ctx) {
    // Create HDF5 file (truncate if exists)
    file_id_ = H5Fcreate(h5_path_.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_id_ < 0) {
        last_error_ = "Default2DOutputPlugin: cannot create file '" + h5_path_ + "'";
        state_ = PluginState::ERROR;
        return 1;
    }

    // Root attributes (CF-1.11 / UGRID-1.0)
    writeStringAttr(file_id_, "Conventions", "CF-1.11 UGRID-1.0");
    writeStringAttr(file_id_, "title", "OpenSWMM 2D Surface Routing Results");
    writeStringAttr(file_id_, "institution", "OpenSWMM / HydroCouple");
    writeStringAttr(file_id_, "source", "OpenSWMM Engine 6.0");

    state_ = PluginState::PREPARED;
    return 0;
}

// ============================================================================
// prepareMesh() — called by SWMMEngine after 2D initialization
// ============================================================================

void writeMeshToHDF5(hid_t file_id, const MeshData& mesh,
                      hsize_t& n_faces, hsize_t& n_nodes,
                      auto& writeStringAttrFn) {
    n_faces = static_cast<hsize_t>(mesh.n_triangles());
    n_nodes = static_cast<hsize_t>(mesh.n_vertices());

    // --- Mesh2 topology variable (UGRID convention) ---
    {
        hid_t space = H5Screate(H5S_SCALAR);
        hid_t ds = H5Dcreate2(file_id, "Mesh2", H5T_NATIVE_INT, space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);

        writeStringAttrFn(ds, "cf_role", "mesh_topology");
        writeStringAttrFn(ds, "topology_dimension", "2");
        writeStringAttrFn(ds, "node_coordinates", "Mesh2_node_x Mesh2_node_y");
        writeStringAttrFn(ds, "face_node_connectivity", "Mesh2_face_nodes");
        writeStringAttrFn(ds, "face_coordinates", "Mesh2_face_x Mesh2_face_y");

        // Write a dummy value
        int dummy = 0;
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &dummy);
        H5Dclose(ds);
        H5Sclose(space);
    }

    // --- Vertex coordinates ---
    {
        hsize_t dim = n_nodes;
        hid_t space = H5Screate_simple(1, &dim, nullptr);

        // Mesh2_node_x
        hid_t ds_x = H5Dcreate2(file_id, "Mesh2_node_x", H5T_NATIVE_DOUBLE,
                                  space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(ds_x, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.vx.data());
        writeStringAttrFn(ds_x, "standard_name", "projection_x_coordinate");
        writeStringAttrFn(ds_x, "units", "m");
        H5Dclose(ds_x);

        // Mesh2_node_y
        hid_t ds_y = H5Dcreate2(file_id, "Mesh2_node_y", H5T_NATIVE_DOUBLE,
                                  space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(ds_y, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.vy.data());
        writeStringAttrFn(ds_y, "standard_name", "projection_y_coordinate");
        writeStringAttrFn(ds_y, "units", "m");
        H5Dclose(ds_y);

        // Mesh2_node_z (elevation)
        hid_t ds_z = H5Dcreate2(file_id, "Mesh2_node_z", H5T_NATIVE_DOUBLE,
                                  space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(ds_z, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.vz.data());
        writeStringAttrFn(ds_z, "standard_name", "altitude");
        writeStringAttrFn(ds_z, "units", "m");
        writeStringAttrFn(ds_z, "positive", "up");
        H5Dclose(ds_z);

        H5Sclose(space);
    }

    // --- Face-node connectivity [nFace, 3] ---
    {
        hsize_t dims[2] = { n_faces, 3 };
        hid_t space = H5Screate_simple(2, dims, nullptr);
        hid_t ds = H5Dcreate2(file_id, "Mesh2_face_nodes", H5T_NATIVE_INT,
                                space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);

        // Interleave v0, v1, v2 into [nFace, 3] row-major
        std::vector<int> conn(n_faces * 3);
        for (hsize_t i = 0; i < n_faces; ++i) {
            conn[i * 3 + 0] = mesh.tri_v0[i];
            conn[i * 3 + 1] = mesh.tri_v1[i];
            conn[i * 3 + 2] = mesh.tri_v2[i];
        }
        H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, conn.data());

        writeStringAttrFn(ds, "cf_role", "face_node_connectivity");
        writeStringAttrFn(ds, "start_index", "0");
        H5Dclose(ds);
        H5Sclose(space);
    }

    // --- Face centroids ---
    {
        hsize_t dim = n_faces;
        hid_t space = H5Screate_simple(1, &dim, nullptr);

        hid_t ds_cx = H5Dcreate2(file_id, "Mesh2_face_x", H5T_NATIVE_DOUBLE,
                                   space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(ds_cx, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.tri_cx.data());
        writeStringAttrFn(ds_cx, "units", "m");
        H5Dclose(ds_cx);

        hid_t ds_cy = H5Dcreate2(file_id, "Mesh2_face_y", H5T_NATIVE_DOUBLE,
                                   space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(ds_cy, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.tri_cy.data());
        writeStringAttrFn(ds_cy, "units", "m");
        H5Dclose(ds_cy);

        hid_t ds_cz = H5Dcreate2(file_id, "Mesh2_face_z", H5T_NATIVE_DOUBLE,
                                   space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(ds_cz, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.tri_cz.data());
        writeStringAttrFn(ds_cz, "units", "m");
        H5Dclose(ds_cz);

        // Manning's n
        hid_t ds_n = H5Dcreate2(file_id, "Mesh2_face_mannings_n", H5T_NATIVE_DOUBLE,
                                  space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(ds_n, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.mannings_n.data());
        writeStringAttrFn(ds_n, "long_name", "Manning roughness coefficient");
        writeStringAttrFn(ds_n, "units", "s m^(-1/3)");
        writeStringAttrFn(ds_n, "mesh", "Mesh2");
        writeStringAttrFn(ds_n, "location", "face");
        H5Dclose(ds_n);

        // Face area
        hid_t ds_a = H5Dcreate2(file_id, "Mesh2_face_area", H5T_NATIVE_DOUBLE,
                                  space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        H5Dwrite(ds_a, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.tri_area.data());
        writeStringAttrFn(ds_a, "long_name", "face planimetric area");
        writeStringAttrFn(ds_a, "units", "m2");
        writeStringAttrFn(ds_a, "mesh", "Mesh2");
        writeStringAttrFn(ds_a, "location", "face");
        H5Dclose(ds_a);

        H5Sclose(space);
    }
}

void Default2DOutputPlugin::prepareMeshAndDatasets(const MeshData& mesh) {
    auto writeAttr = [this](hid_t loc, const char* name, const char* val) {
        writeStringAttr(loc, name, val);
    };
    writeMeshToHDF5(file_id_, mesh, n_faces_, n_nodes_, writeAttr);

    // --- Create time-varying datasets with unlimited time dimension ---
    // Chunk size: 1 time step x all faces (or nodes)
    hsize_t face_chunk[2] = { 1, n_faces_ };
    hsize_t node_chunk[2] = { 1, n_nodes_ };
    hsize_t edge_chunk[3] = { 1, n_faces_, 3 };
    hsize_t time_chunk[1] = { 64 };

    hsize_t zero2[2] = { 0, n_faces_ };
    hsize_t zero2n[2] = { 0, n_nodes_ };
    hsize_t zero3[3] = { 0, n_faces_, 3 };
    hsize_t zero1[1] = { 0 };

    // Time coordinate
    ds_time_ = createUnlimitedDataset("time", 1, zero1, time_chunk);
    writeStringAttr(ds_time_, "standard_name", "time");
    writeStringAttr(ds_time_, "units", "days since simulation start");
    writeStringAttr(ds_time_, "calendar", "standard");

    // Helper lambda for face datasets
    auto createFaceDS = [&](const char* name, const char* long_name,
                             const char* units, const char* std_name = nullptr) -> hid_t {
        hid_t ds = createUnlimitedDataset(name, 2, zero2, face_chunk);
        writeStringAttr(ds, "long_name", long_name);
        writeStringAttr(ds, "units", units);
        writeStringAttr(ds, "mesh", "Mesh2");
        writeStringAttr(ds, "location", "face");
        if (std_name)
            writeStringAttr(ds, "standard_name", std_name);
        return ds;
    };

    ds_face_depth_         = createFaceDS("Mesh2_face_depth",
                                           "overland flow depth", "m", "water_surface_height_above_reference_datum");
    ds_face_head_          = createFaceDS("Mesh2_face_head",
                                           "total hydraulic head", "m", "hydraulic_head");
    ds_face_grad_hx_       = createFaceDS("Mesh2_face_grad_hx",
                                           "head gradient dh/dx (unlimited)", "1");
    ds_face_grad_hy_       = createFaceDS("Mesh2_face_grad_hy",
                                           "head gradient dh/dy (unlimited)", "1");
    ds_face_grad_hx_lim_   = createFaceDS("Mesh2_face_grad_hx_lim",
                                           "head gradient dh/dx (slope limited)", "1");
    ds_face_grad_hy_lim_   = createFaceDS("Mesh2_face_grad_hy_lim",
                                           "head gradient dh/dy (slope limited)", "1");
    ds_face_rainfall_      = createFaceDS("Mesh2_face_rainfall",
                                           "rainfall intensity", "m s-1", "rainfall_rate");
    ds_face_coupling_flux_ = createFaceDS("Mesh2_face_coupling_flux",
                                           "coupling flux with SWMM node", "m s-1");
    ds_face_net_source_    = createFaceDS("Mesh2_face_net_source",
                                           "net volumetric source/sink", "m s-1");

    // Edge flux [nTime, nFace, 3]
    ds_edge_flux_ = createUnlimitedDataset("Mesh2_edge_flux", 3, zero3, edge_chunk);
    writeStringAttr(ds_edge_flux_, "long_name", "normal flux through cell edges");
    writeStringAttr(ds_edge_flux_, "units", "m2 s-1");
    writeStringAttr(ds_edge_flux_, "mesh", "Mesh2");
    writeStringAttr(ds_edge_flux_, "location", "edge");

    // Node head [nTime, nNode]
    ds_node_head_ = createUnlimitedDataset("Mesh2_node_head", 2, zero2n, node_chunk);
    writeStringAttr(ds_node_head_, "long_name", "reconstructed vertex head");
    writeStringAttr(ds_node_head_, "units", "m");
    writeStringAttr(ds_node_head_, "mesh", "Mesh2");
    writeStringAttr(ds_node_head_, "location", "node");
}

// ============================================================================
// update() — append one time step (called from IO thread)
// ============================================================================

int Default2DOutputPlugin::update(const SimulationSnapshot& snap) {
    if (file_id_ < 0 || snap.surface_tri_count == 0) return 0;

    // Write time value
    {
        hsize_t new_dim = n_steps_ + 1;
        H5Dset_extent(ds_time_, &new_dim);
        hid_t fspace = H5Dget_space(ds_time_);
        hsize_t offset = n_steps_, count = 1;
        H5Sselect_hyperslab(fspace, H5S_SELECT_SET, &offset, nullptr, &count, nullptr);
        hid_t mspace = H5Screate_simple(1, &count, nullptr);
        H5Dwrite(ds_time_, H5T_NATIVE_DOUBLE, mspace, fspace, H5P_DEFAULT, &snap.sim_time);
        H5Sclose(mspace);
        H5Sclose(fspace);
    }

    // Write per-face fields
    extendAndWrite2D(ds_face_depth_,         snap.surface_depth.data(),          n_faces_);
    extendAndWrite2D(ds_face_head_,          snap.surface_head.data(),           n_faces_);
    extendAndWrite2D(ds_face_grad_hx_,       snap.surface_grad_hx.data(),       n_faces_);
    extendAndWrite2D(ds_face_grad_hy_,       snap.surface_grad_hy.data(),       n_faces_);
    extendAndWrite2D(ds_face_grad_hx_lim_,   snap.surface_grad_hx_lim.data(),   n_faces_);
    extendAndWrite2D(ds_face_grad_hy_lim_,   snap.surface_grad_hy_lim.data(),   n_faces_);
    extendAndWrite2D(ds_face_rainfall_,      snap.surface_rainfall.data(),      n_faces_);
    extendAndWrite2D(ds_face_coupling_flux_, snap.surface_coupling_flux.data(), n_faces_);
    extendAndWrite2D(ds_face_net_source_,    snap.surface_net_source.data(),    n_faces_);

    // Write per-edge fields [nFace, 3]
    extendAndWrite3D(ds_edge_flux_, snap.surface_edge_flux.data(), n_faces_, 3);

    // Write per-node fields
    extendAndWrite2D(ds_node_head_, snap.surface_vert_head.data(), n_nodes_);

    ++n_steps_;
    return 0;
}

// ============================================================================
// finalize()
// ============================================================================

int Default2DOutputPlugin::finalize(const SimulationContext& /*ctx*/) {
    // Close all datasets
    auto closeDS = [](hid_t& ds) {
        if (ds != H5I_INVALID_HID) { H5Dclose(ds); ds = H5I_INVALID_HID; }
    };
    closeDS(ds_time_);
    closeDS(ds_face_depth_);
    closeDS(ds_face_head_);
    closeDS(ds_face_grad_hx_);
    closeDS(ds_face_grad_hy_);
    closeDS(ds_face_grad_hx_lim_);
    closeDS(ds_face_grad_hy_lim_);
    closeDS(ds_face_rainfall_);
    closeDS(ds_face_coupling_flux_);
    closeDS(ds_face_net_source_);
    closeDS(ds_edge_flux_);
    closeDS(ds_node_head_);

    if (file_id_ != H5I_INVALID_HID) {
        H5Fclose(file_id_);
        file_id_ = H5I_INVALID_HID;
    }

    state_ = PluginState::FINALIZED;
    return 0;
}

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
