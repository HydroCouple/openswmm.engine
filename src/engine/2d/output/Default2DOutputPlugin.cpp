/**
 * @file      Default2DOutputPlugin.cpp
 * @brief     HDF5 output plugin for 2D surface routing results (CF/UGRID).
 *
 * @see Default2DOutputPlugin.hpp
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifdef OPENSWMM_HAS_2D

#include "Default2DOutputPlugin.hpp"
#include "../../io/Hdf5Mutex.hpp"
#include "../../io/Hdf5ObjectCreation.hpp"
#include "../../io/CheckpointHdf5.hpp"
#include "../subsurface/SourceResultHistory.hpp"
#include "../subsurface/SubsurfaceData.hpp" // G-O: the aquifer ledger at finalize
#include "../../core/SimulationContext.hpp"
#include "../../2d/SurfaceRouter2D.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../data/Report2DVars.hpp"
#include "../solver/MomentumPhysics2D.hpp"
#include "../solver/InertialEdges.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <stdexcept>

namespace openswmm::twoD {

// ============================================================================
// Helpers
// ============================================================================

void Default2DOutputPlugin::writeStringAttr(hid_t loc, const char* name, const char* value) {
    // H5Tset_size rejects 0, and nothing here checks return codes — an empty
    // value would silently write no attribute at all. Store a single NUL
    // instead, which reads back as the empty string.
    const size_t len = (value && *value) ? std::strlen(value) : 1;

    hid_t atype = H5Tcopy(H5T_C_S1);
    H5Tset_size(atype, len);
    H5Tset_strpad(atype, H5T_STR_NULLTERM);

    hid_t aspace = H5Screate(H5S_SCALAR);
    hid_t attr = H5Acreate2(loc, name, atype, aspace, H5P_DEFAULT, H5P_DEFAULT);
    checkpointWriteStatus(H5Awrite(attr, atype, value));
    H5Aclose(attr);
    H5Sclose(aspace);
    H5Tclose(atype);
}

hid_t Default2DOutputPlugin::createUnlimitedDataset(const char* name, int rank, const hsize_t* dims,
                                                    const hsize_t* chunk_dims, hid_t type) {
    // Create dataspace with unlimited first dimension
    std::vector<hsize_t> maxdims(dims, dims + rank);
    maxdims[0] = H5S_UNLIMITED;
    hid_t space = H5Screate_simple(rank, dims, maxdims.data());

    // Enable chunking (required for unlimited dimensions). The byte-shuffle
    // filter precedes deflate: on smooth float fields it typically improves
    // the zlib ratio 20-40% at negligible CPU. Level 0 = no filters.
    hid_t plist = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_chunk(plist, rank, chunk_dims);
    if (compression_ > 0) {
        H5Pset_shuffle(plist);
        H5Pset_deflate(plist, static_cast<unsigned>(compression_));
    }

    // Storage type follows OUTPUT_PRECISION unless the caller forces one; the
    // memory type on every write stays H5T_NATIVE_DOUBLE and HDF5 converts, so
    // no buffer copies.
    hid_t ds = io::createHdf5Dataset(file_id_, name, type >= 0 ? type : storage_type_, space,
                                     H5P_DEFAULT, plist, H5P_DEFAULT);
    H5Pclose(plist);
    H5Sclose(space);
    return ds;
}

void Default2DOutputPlugin::extendAndWrite2D(hid_t ds, const double* data, hsize_t n_cols) {
    // Extend dataset by one row in the time dimension
    hsize_t new_dims[2] = {n_steps_ + 1, n_cols};
    checkpointWriteStatus(H5Dset_extent(ds, new_dims));

    // Select the new row hyperslab
    hid_t filespace = H5Dget_space(ds);
    hsize_t offset[2] = {n_steps_, 0};
    hsize_t count[2] = {1, n_cols};
    checkpointWriteStatus(
        H5Sselect_hyperslab(filespace, H5S_SELECT_SET, offset, nullptr, count, nullptr));

    // Memory space for one row
    hid_t memspace = H5Screate_simple(2, count, nullptr);
    checkpointWriteStatus(H5Dwrite(ds, H5T_NATIVE_DOUBLE, memspace, filespace, H5P_DEFAULT, data));
    H5Sclose(memspace);
    H5Sclose(filespace);
}

void Default2DOutputPlugin::extendAndWrite3D(hid_t ds, const double* data, hsize_t dim1,
                                             hsize_t dim2) {
    hsize_t new_dims[3] = {n_steps_ + 1, dim1, dim2};
    checkpointWriteStatus(H5Dset_extent(ds, new_dims));

    hid_t filespace = H5Dget_space(ds);
    hsize_t offset[3] = {n_steps_, 0, 0};
    hsize_t count[3] = {1, dim1, dim2};
    checkpointWriteStatus(
        H5Sselect_hyperslab(filespace, H5S_SELECT_SET, offset, nullptr, count, nullptr));

    hid_t memspace = H5Screate_simple(3, count, nullptr);
    checkpointWriteStatus(H5Dwrite(ds, H5T_NATIVE_DOUBLE, memspace, filespace, H5P_DEFAULT, data));
    H5Sclose(memspace);
    H5Sclose(filespace);
}

void Default2DOutputPlugin::extendAndWrite4D(hid_t ds, const double* data, hsize_t a, hsize_t b,
                                             hsize_t c) {
    hsize_t dims[4] = {n_steps_ + 1, a, b, c}, offset[4] = {n_steps_, 0, 0, 0},
            count[4] = {1, a, b, c};
    checkpointWriteStatus(H5Dset_extent(ds, dims));
    const hid_t file = H5Dget_space(ds), memory = H5Screate_simple(4, count, nullptr);
    checkpointWriteStatus(
        H5Sselect_hyperslab(file, H5S_SELECT_SET, offset, nullptr, count, nullptr));
    checkpointWriteStatus(H5Dwrite(ds, H5T_NATIVE_DOUBLE, memory, file, H5P_DEFAULT, data));
    H5Sclose(memory);
    H5Sclose(file);
}

void Default2DOutputPlugin::writeDoubleAttr(hid_t loc, const char* name, double value) {
    hid_t aspace = H5Screate(H5S_SCALAR);
    hid_t attr = H5Acreate2(loc, name, H5T_NATIVE_DOUBLE, aspace, H5P_DEFAULT, H5P_DEFAULT);
    checkpointWriteStatus(H5Awrite(attr, H5T_NATIVE_DOUBLE, &value));
    H5Aclose(attr);
    H5Sclose(aspace);
}

hid_t Default2DOutputPlugin::createFaceEnvelopeDataset(const char* name, const char* long_name,
                                                       const char* units) {
    // Fixed [nFace] dataset — no time dimension, no chunking. Overwritten in
    // place each update(); since envelopes are monotone the last write is final.
    hid_t space = H5Screate_simple(1, &n_faces_, nullptr);
    hid_t ds = io::createHdf5Dataset(file_id_, name, storage_type_, space, H5P_DEFAULT, H5P_DEFAULT,
                                     H5P_DEFAULT);
    writeStringAttr(ds, "long_name", long_name);
    writeStringAttr(ds, "units", units);
    writeStringAttr(ds, "mesh", "Mesh2");
    writeStringAttr(ds, "location", "face");
    writeStringAttr(ds, "cell_methods", "time: maximum");
    H5Sclose(space);
    return ds;
}

void Default2DOutputPlugin::writeFaceEnvelope(hid_t ds, const double* data) {
    if (ds == H5I_INVALID_HID)
        return;
    checkpointWriteStatus(H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, data));
}

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * @brief Retain the destination path without opening or creating a result file.
 * @param[in] h5_path Owned result path string, moved into this plugin. Relative path remains
 * relative to caller's eventual file-open environment; model path resolution belongs to the
 * caller.
 * @note Leaves HDF5 handles invalid and datasets uninitialized. configure/initialize later
 * qualify and open the file. No filesystem, physics or clock change; string/member construction
 * may throw.
 */
Default2DOutputPlugin::Default2DOutputPlugin(std::string h5_path) : h5_path_(std::move(h5_path)) {}

/**
 * @brief Close any retained HDF5 datasets and result file under the global result mutex.
 * @note Caller first stops producer/IO work; destruction does not flush queued histories, write
 * final balances or report HDF5 close errors. Invalidates all borrowed strings/handle references
 * and releases shared queued histories. Unexpected throwing mutex/dependencies terminate
 * destruction.
 */
Default2DOutputPlugin::~Default2DOutputPlugin() {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    closeDatasets();
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
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    // Create HDF5 file (truncate if exists)
    file_id_ = io::createCheckpointHdf5(h5_path_.c_str(),
                                        checkpoint_fresh_ ? H5F_ACC_EXCL : H5F_ACC_TRUNC);
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

    // Model CRS for the `/crs` variable written in prepareMeshAndDatasets().
    // ctx.spatial.crs and ctx.options.crs are both set by OptionsHandler from
    // `[OPTIONS] CRS`; prefer the spatial frame, which is what the spatial API
    // mutates at runtime. Empty is legitimate — models need not declare a CRS.
    model_crs_ = !ctx.spatial.crs.empty() ? ctx.spatial.crs : ctx.options.crs;

    // Concentration units belong to species identities. The groundwater
    // row set can differ from the surface row set (including surface off).
    // Capture authored metadata once; never manufacture a dimensionless
    // label for an unresolved species.
    native_species_units_.clear();
    for (std::size_t p = 0; p < ctx.pollutants.units.size(); ++p) {
        std::string unit;
        switch (ctx.pollutants.units[p]) {
        case MassUnits::MG_PER_L:
            unit = "MG/L";
            break;
        case MassUnits::UG_PER_L:
            unit = "UG/L";
            break;
        case MassUnits::COUNTS_PER_L:
            unit = "#/L";
            break;
        }
        native_species_units_.emplace(ctx.pollutant_names.name_of(static_cast<int>(p)), unit);
    }
    for (std::size_t p = 0; p < ctx.reactions.species_name.size(); ++p) {
        if (p < ctx.reactions.species_is_wall.size() && ctx.reactions.species_is_wall[p])
            continue;
        if (p < ctx.reactions.species_units.size())
            native_species_units_.emplace(ctx.reactions.species_name[p],
                                          ctx.reactions.species_units[p]);
    }
    // GW snapshots retain raw age-volume / volume (seconds); only the
    // separate surface snapshot converts its age result to hours.
    native_species_units_["__WATER_AGE__"] = "s";
    native_species_units_["__TEMPERATURE__"] = "degC";

    state_ = checkpoint_write_failed_ ? PluginState::ERROR : PluginState::PREPARED;
    return checkpoint_write_failed_ ? 1 : 0;
}

void Default2DOutputPlugin::setMeshCoordinateScale(double metres_per_model_unit) {
    if (metres_per_model_unit > 0.0)
        metres_per_model_unit_ = metres_per_model_unit;
}

std::string Default2DOutputPlugin::configureOutput(const SolverOptions2D& opts,
                                                   double report_step_sec) {
    storage_type_ =
        (opts.output_precision == OutputPrecision2D::FLOAT64) ? H5T_NATIVE_DOUBLE : H5T_IEEE_F32LE;
    compression_ = std::max(0, std::min(9, opts.output_compression));
    report_vars_ = (opts.report_2d_vars & report2d::ALL_MASK) | report2d::DEPTH;
    momentum_velocity_ = opts.momentumPhysics();
    mixing_enabled_ = opts.turbulence == Turbulence2D::SMAGORINSKY;
    mixing_cs_ = opts.smagorinsky_cs;
    mixing_dry_ = opts.dry_depth;
    coriolis_f_ = opts.coriolisParameter();
    species_filter_ = opts.report_2d_species;
    species_rows_.clear();

    report_2d_step_days_ = 0.0;
    next_due_days_ = -1.0;
    if (opts.report_2d_step > 0.0) {
        if (report_step_sec > 0.0) {
            const double ratio = opts.report_2d_step / report_step_sec;
            const double rounded = std::round(ratio);
            if (rounded < 1.0 || std::fabs(ratio - rounded) > 1e-6) {
                char buf[192];
                std::snprintf(buf, sizeof buf,
                              "[2D_OPTIONS] REPORT_2D_STEP (%.0f s) must be a positive "
                              "multiple of REPORT_STEP (%.0f s).",
                              opts.report_2d_step, report_step_sec);
                return buf;
            }
        }
        report_2d_step_days_ = opts.report_2d_step / 86400.0;
    }
    return {};
}

void Default2DOutputPlugin::writeCrsVariable() {
    if (file_id_ == H5I_INVALID_HID)
        return;

    hid_t space = H5Screate(H5S_SCALAR);
    hid_t ds = io::createHdf5Dataset(file_id_, "crs", H5T_NATIVE_INT, space, H5P_DEFAULT,
                                     H5P_DEFAULT, H5P_DEFAULT);
    if (ds < 0) {
        H5Sclose(space);
        return;
    }

    writeStringAttr(ds, "long_name",
                    "coordinate reference of the MODEL, plus the factor "
                    "relating it to the metric Mesh2 coordinates stored here");
    // Unit of the coordinates AS STORED here. The 2D solver runs in SI, so
    // node/face x/y are metres whatever the model's own unit is.
    writeStringAttr(ds, "units", "m");
    // stored = model x factor; model = stored / factor.
    writeDoubleAttr(ds, "metres_per_model_unit", metres_per_model_unit_);
    writeStringAttr(ds, "comment",
                    "Mesh2 x/y are stored in SI metres. Divide by "
                    "metres_per_model_unit to obtain coordinates in the linear "
                    "unit of model_crs; only then reproject from model_crs.");

    if (!model_crs_.empty()) {
        // CRS of the MODEL coordinates, not of the metric values stored here.
        // Named model_crs rather than spatial_ref/crs_wkt on purpose: a
        // generic reader honouring those would place metres in a foot-based
        // CRS and reproduce the offset this variable exists to describe (#155).
        writeStringAttr(ds, "model_crs", model_crs_.c_str());
    }

    int dummy = 0;
    checkpointWriteStatus(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &dummy));
    H5Dclose(ds);
    H5Sclose(space);
}

// ============================================================================
// prepareMesh() — called by SWMMEngine after 2D initialization
// ============================================================================

/**
 * @brief Write static SI mixed-cell mesh topology, coordinates and edge geometry.
 * @param[in] file_id Valid open writable HDF5 file, borrowed; names must not already exist.
 * @param[in] mesh Prepared mesh with complete SI vertex/cell/edge geometry and valid
 * connectivity.
 * @param[out] n_faces Replaced by mesh.n_triangles(), the generic mesh cell count.
 * @param[out] n_nodes Replaced by mesh.n_vertices().
 * @param[out] edge_stride Replaced by mesh.edge_stride(), 3 for all triangles or 4 for mixed
 * cells.
 * @param[in,out] writeStringAttrFn Borrowed callable (object,name,value) writing scalar string
 * attributes; invoked synchronously with local buffers whose lifetimes end after each call.
 * @param[in,out] writeStatusFn Borrowed callable receiving HDF5 write statuses for owner
 * tracking.
 * @note Writes CF/UGRID descriptor, SI coordinates, zero-based padded cell connectivity,
 * roughness and edge fields, closing local handles. Does not acquire mutex, validate source
 * shapes or roll back partial file writes. Caller serializes HDF5 and consumes status/errors;
 * callbacks/allocation can throw after output/count mutation. Mesh data is not retained or
 * modified.
 */
void writeMeshToHDF5(hid_t file_id, const MeshData& mesh, hsize_t& n_faces, hsize_t& n_nodes,
                     hsize_t& edge_stride, auto& writeStringAttrFn, auto& writeStatusFn) {
    n_faces = static_cast<hsize_t>(mesh.n_triangles());
    n_nodes = static_cast<hsize_t>(mesh.n_vertices());
    // UGRID mixed topology: [nFace, max_nv] with _FillValue in the padding.
    // All-triangle meshes keep the historical {n, 3} layout byte-for-byte.
    edge_stride = static_cast<hsize_t>(mesh.edge_stride());
    const int stride = static_cast<int>(edge_stride);

    // --- Mesh2 topology variable (UGRID convention) ---
    {
        hid_t space = H5Screate(H5S_SCALAR);
        hid_t ds = io::createHdf5Dataset(file_id, "Mesh2", H5T_NATIVE_INT, space, H5P_DEFAULT,
                                         H5P_DEFAULT, H5P_DEFAULT);

        writeStringAttrFn(ds, "cf_role", "mesh_topology");
        writeStringAttrFn(ds, "topology_dimension", "2");
        writeStringAttrFn(ds, "node_coordinates", "Mesh2_node_x Mesh2_node_y");
        writeStringAttrFn(ds, "face_node_connectivity", "Mesh2_face_nodes");
        writeStringAttrFn(ds, "face_coordinates", "Mesh2_face_x Mesh2_face_y");
        writeStringAttrFn(ds, "openswmm_crs", "crs");

        // Write a dummy value
        int dummy = 0;
        writeStatusFn(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &dummy));
        H5Dclose(ds);
        H5Sclose(space);
    }

    // --- Vertex coordinates ---
    {
        hsize_t dim = n_nodes;
        hid_t space = H5Screate_simple(1, &dim, nullptr);

        // Mesh2_node_x
        hid_t ds_x = io::createHdf5Dataset(file_id, "Mesh2_node_x", H5T_NATIVE_DOUBLE, space,
                                           H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        writeStatusFn(
            H5Dwrite(ds_x, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.vx.data()));
        writeStringAttrFn(ds_x, "standard_name", "projection_x_coordinate");
        writeStringAttrFn(ds_x, "units", "m");
        writeStringAttrFn(ds_x, "openswmm_crs", "crs");
        H5Dclose(ds_x);

        // Mesh2_node_y
        hid_t ds_y = io::createHdf5Dataset(file_id, "Mesh2_node_y", H5T_NATIVE_DOUBLE, space,
                                           H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        writeStatusFn(
            H5Dwrite(ds_y, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.vy.data()));
        writeStringAttrFn(ds_y, "standard_name", "projection_y_coordinate");
        writeStringAttrFn(ds_y, "units", "m");
        writeStringAttrFn(ds_y, "openswmm_crs", "crs");
        H5Dclose(ds_y);

        // Mesh2_node_z (elevation)
        hid_t ds_z = io::createHdf5Dataset(file_id, "Mesh2_node_z", H5T_NATIVE_DOUBLE, space,
                                           H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        writeStatusFn(
            H5Dwrite(ds_z, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.vz.data()));
        writeStringAttrFn(ds_z, "standard_name", "altitude");
        writeStringAttrFn(ds_z, "units", "m");
        writeStringAttrFn(ds_z, "positive", "up");
        H5Dclose(ds_z);

        H5Sclose(space);
    }

    // --- Face-node connectivity [nFace, 3 | 4] ---
    {
        hsize_t dims[2] = {n_faces, edge_stride};
        hid_t space = H5Screate_simple(2, dims, nullptr);
        const int fill = -1;
        hid_t dcpl = H5P_DEFAULT;
        if (stride > 3) {
            dcpl = H5Pcreate(H5P_DATASET_CREATE);
            H5Pset_fill_value(dcpl, H5T_NATIVE_INT, &fill);
        }
        hid_t ds = io::createHdf5Dataset(file_id, "Mesh2_face_nodes", H5T_NATIVE_INT, space,
                                         H5P_DEFAULT, dcpl, H5P_DEFAULT);
        if (dcpl != H5P_DEFAULT)
            H5Pclose(dcpl);

        // Interleave the cell vertices into [nFace, stride] row-major;
        // padding slots of a triangle row carry −1 (the UGRID fill value).
        std::vector<int> conn(n_faces * edge_stride);
        for (hsize_t i = 0; i < n_faces; ++i) {
            const int c = static_cast<int>(i);
            const int nv = mesh.cell_vertex_count(c);
            for (int k = 0; k < stride; ++k)
                conn[i * edge_stride + static_cast<hsize_t>(k)] =
                    (k < nv) ? mesh.cell_vertex(c, k) : -1;
        }
        writeStatusFn(H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, conn.data()));

        writeStringAttrFn(ds, "cf_role", "face_node_connectivity");
        writeStringAttrFn(ds, "start_index", "0");
        if (stride > 3) {
            // UGRID: the fill value is what marks a mixed-shape row's padding.
            hid_t aspace = H5Screate(H5S_SCALAR);
            hid_t attr =
                H5Acreate2(ds, "_FillValue", H5T_NATIVE_INT, aspace, H5P_DEFAULT, H5P_DEFAULT);
            writeStatusFn(H5Awrite(attr, H5T_NATIVE_INT, &fill));
            H5Aclose(attr);
            H5Sclose(aspace);
        }
        H5Dclose(ds);
        H5Sclose(space);
    }

    // --- Vertices per face [nFace] — only for mixed meshes (int8) ---
    if (stride > 3) {
        hsize_t dim = n_faces;
        hid_t space = H5Screate_simple(1, &dim, nullptr);
        hid_t ds = io::createHdf5Dataset(file_id, "Mesh2_face_nv", H5T_NATIVE_INT8, space,
                                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        std::vector<int8_t> nv(n_faces);
        for (hsize_t i = 0; i < n_faces; ++i)
            nv[i] = static_cast<int8_t>(mesh.cell_vertex_count(static_cast<int>(i)));
        writeStatusFn(H5Dwrite(ds, H5T_NATIVE_INT8, H5S_ALL, H5S_ALL, H5P_DEFAULT, nv.data()));
        writeStringAttrFn(ds, "long_name", "vertices per face (3 = triangle, 4 = quadrilateral)");
        writeStringAttrFn(ds, "mesh", "Mesh2");
        writeStringAttrFn(ds, "location", "face");
        H5Dclose(ds);
        H5Sclose(space);
    }

    // --- Face centroids ---
    {
        hsize_t dim = n_faces;
        hid_t space = H5Screate_simple(1, &dim, nullptr);

        hid_t ds_cx = io::createHdf5Dataset(file_id, "Mesh2_face_x", H5T_NATIVE_DOUBLE, space,
                                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        writeStatusFn(
            H5Dwrite(ds_cx, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.tri_cx.data()));
        writeStringAttrFn(ds_cx, "units", "m");
        writeStringAttrFn(ds_cx, "openswmm_crs", "crs");
        H5Dclose(ds_cx);

        hid_t ds_cy = io::createHdf5Dataset(file_id, "Mesh2_face_y", H5T_NATIVE_DOUBLE, space,
                                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        writeStatusFn(
            H5Dwrite(ds_cy, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.tri_cy.data()));
        writeStringAttrFn(ds_cy, "units", "m");
        writeStringAttrFn(ds_cy, "openswmm_crs", "crs");
        H5Dclose(ds_cy);

        hid_t ds_cz = io::createHdf5Dataset(file_id, "Mesh2_face_z", H5T_NATIVE_DOUBLE, space,
                                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        writeStatusFn(
            H5Dwrite(ds_cz, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.tri_cz.data()));
        writeStringAttrFn(ds_cz, "units", "m");
        H5Dclose(ds_cz);

        // Manning's n
        hid_t ds_n = io::createHdf5Dataset(file_id, "Mesh2_face_mannings_n", H5T_NATIVE_DOUBLE,
                                           space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        writeStatusFn(H5Dwrite(ds_n, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                               mesh.mannings_n.data()));
        writeStringAttrFn(ds_n, "long_name", "Manning roughness coefficient");
        writeStringAttrFn(ds_n, "units", "s m^(-1/3)");
        writeStringAttrFn(ds_n, "mesh", "Mesh2");
        writeStringAttrFn(ds_n, "location", "face");
        H5Dclose(ds_n);

        // Face area
        hid_t ds_a = io::createHdf5Dataset(file_id, "Mesh2_face_area", H5T_NATIVE_DOUBLE, space,
                                           H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        writeStatusFn(
            H5Dwrite(ds_a, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, mesh.tri_area.data()));
        writeStringAttrFn(ds_a, "long_name", "face planimetric area");
        writeStringAttrFn(ds_a, "units", "m2");
        writeStringAttrFn(ds_a, "mesh", "Mesh2");
        writeStringAttrFn(ds_a, "location", "face");
        H5Dclose(ds_a);

        H5Sclose(space);
    }

    // --- Edge geometry [nFace, 3] — time-invariant, indexed (tri, localEdge) ---
    // Paired with the time-varying /Mesh2_edge_flux dataset so post-run readers
    // can reconstruct cell-centred velocity via RT0 without re-deriving edge
    // length / outward normals from the vertex coordinates.
    {
        hsize_t dims[2] = {n_faces, edge_stride};
        hid_t space = H5Screate_simple(2, dims, nullptr);

        // Pack the internal kMaxCellVerts-stride slots to the public stride.
        auto packed = [&](const std::vector<double>& src) {
            std::vector<double> out(n_faces * edge_stride, 0.0);
            for (hsize_t i = 0; i < n_faces; ++i)
                for (int k = 0; k < stride; ++k)
                    out[i * edge_stride + static_cast<hsize_t>(k)] =
                        src[static_cast<std::size_t>(MeshData::slot(static_cast<int>(i), k))];
            return out;
        };
        const std::vector<double> len_p = packed(mesh.edge_length);
        const std::vector<double> nx_p = packed(mesh.edge_nx);
        const std::vector<double> ny_p = packed(mesh.edge_ny);

        hid_t ds_len = io::createHdf5Dataset(file_id, "Mesh2_edge_length", H5T_NATIVE_DOUBLE, space,
                                             H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        writeStatusFn(
            H5Dwrite(ds_len, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, len_p.data()));
        writeStringAttrFn(ds_len, "long_name", "edge length");
        writeStringAttrFn(ds_len, "units", "m");
        writeStringAttrFn(ds_len, "mesh", "Mesh2");
        writeStringAttrFn(ds_len, "location", "edge");
        H5Dclose(ds_len);

        hid_t ds_nx = io::createHdf5Dataset(file_id, "Mesh2_edge_nx", H5T_NATIVE_DOUBLE, space,
                                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        writeStatusFn(
            H5Dwrite(ds_nx, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, nx_p.data()));
        writeStringAttrFn(ds_nx, "long_name", "edge outward unit normal x component");
        writeStringAttrFn(ds_nx, "units", "1");
        writeStringAttrFn(ds_nx, "mesh", "Mesh2");
        writeStringAttrFn(ds_nx, "location", "edge");
        H5Dclose(ds_nx);

        hid_t ds_ny = io::createHdf5Dataset(file_id, "Mesh2_edge_ny", H5T_NATIVE_DOUBLE, space,
                                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        writeStatusFn(
            H5Dwrite(ds_ny, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, ny_p.data()));
        writeStringAttrFn(ds_ny, "long_name", "edge outward unit normal y component");
        writeStringAttrFn(ds_ny, "units", "1");
        writeStringAttrFn(ds_ny, "mesh", "Mesh2");
        writeStringAttrFn(ds_ny, "location", "edge");
        H5Dclose(ds_ny);

        H5Sclose(space);
    }
}

void Default2DOutputPlugin::prepareMeshAndDatasets(const MeshData& mesh) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    // Written first so the `openswmm_crs = "crs"` attributes the mesh
    // variables carry resolve against a variable that already exists.
    writeCrsVariable();
    writeDoubleAttr(file_id_, "coriolis_f_s-1", coriolis_f_);
    writeStringAttr(file_id_, "turbulence_model", mixing_enabled_ ? "SMAGORINSKY" : "NONE");
    if (mixing_enabled_) {
        writeDoubleAttr(file_id_, "smagorinsky_cs", mixing_cs_);
        writeStringAttr(file_id_, "smagorinsky_filter", "sqrt(cell area), SI metres");
        writeStringAttr(file_id_, "turbulent_boundary_traction", "zero");
    }
    mixing_mesh_ = &mesh;
    if (mixing_enabled_ && want(report2d::TURBULENCE)) {
        mixing_edges_ = std::make_unique<InertialEdges>();
        mixing_edges_->build(mesh);
        mixing_diagnostics_ = std::make_unique<MomentumPhysics2D>();
    }


    // Results-file size controls, recorded so readers and the parity tooling
    // can tell how the time-varying datasets were stored.
    writeStringAttr(file_id_, "precision",
                    storage_type_ == H5T_NATIVE_DOUBLE ? "float64" : "float32");
    {
        char cbuf[16];
        std::snprintf(cbuf, sizeof cbuf, "%d", compression_);
        writeStringAttr(file_id_, "compression", cbuf);
        writeStringAttr(file_id_, "report_2d_variables",
                        report2d::formatMask(report_vars_).c_str());
    }

    auto writeAttr = [this](hid_t loc, const char* name, const char* val) {
        writeStringAttr(loc, name, val);
    };
    auto writeStatus = [this](herr_t status) { checkpointWriteStatus(status); };
    writeMeshToHDF5(file_id_, mesh, n_faces_, n_nodes_, edge_stride_, writeAttr, writeStatus);

    // --- Create time-varying datasets with unlimited time dimension ---
    // Chunk size: 1 time step x all faces (or nodes)
    hsize_t face_chunk[2] = {1, n_faces_};
    hsize_t node_chunk[2] = {1, n_nodes_};
    hsize_t edge_chunk[3] = {1, n_faces_, edge_stride_};
    hsize_t time_chunk[1] = {64};

    hsize_t zero2[2] = {0, n_faces_};
    hsize_t zero2n[2] = {0, n_nodes_};
    hsize_t zero3[3] = {0, n_faces_, edge_stride_};
    hsize_t zero1[1] = {0};

    // Time coordinate — always float64: OUTPUT_PRECISION thins the state
    // fields, not the axis every reader keys its lookup on.
    ds_time_ = createUnlimitedDataset("time", 1, zero1, time_chunk, H5T_NATIVE_DOUBLE);
    writeStringAttr(ds_time_, "standard_name", "time");
    writeStringAttr(ds_time_, "units", "days since 1899-12-30 00:00:00");
    writeStringAttr(ds_time_, "calendar", "standard");

    // Helper lambda for face datasets
    auto createFaceDS = [&](const char* name, const char* long_name, const char* units,
                            const char* std_name = nullptr) -> hid_t {
        hid_t ds = createUnlimitedDataset(name, 2, zero2, face_chunk);
        writeStringAttr(ds, "long_name", long_name);
        writeStringAttr(ds, "units", units);
        writeStringAttr(ds, "mesh", "Mesh2");
        writeStringAttr(ds, "location", "face");
        if (std_name)
            writeStringAttr(ds, "standard_name", std_name);
        return ds;
    };

    // Every dataset below is created ONLY when its REPORT_2D_VARIABLES group is
    // selected (default: everything a user renders or plots; solver
    // diagnostics off). Handles stay H5I_INVALID_HID otherwise and update()
    // skips them. DEPTH is always on (the time axis every reader keys on).
    ds_face_depth_ = createFaceDS("Mesh2_face_depth", "overland flow depth", "m",
                                  "water_surface_height_above_reference_datum");
    ds_face_head_ = createFaceDS("Mesh2_face_head", "total hydraulic head", "m", "hydraulic_head");
    if (want(report2d::GRADIENTS)) {
        ds_face_grad_hx_ =
            createFaceDS("Mesh2_face_grad_hx", "head gradient dh/dx (unlimited)", "1");
        ds_face_grad_hy_ =
            createFaceDS("Mesh2_face_grad_hy", "head gradient dh/dy (unlimited)", "1");
        ds_face_grad_hx_lim_ =
            createFaceDS("Mesh2_face_grad_hx_lim", "head gradient dh/dx (slope limited)", "1");
        ds_face_grad_hy_lim_ =
            createFaceDS("Mesh2_face_grad_hy_lim", "head gradient dh/dy (slope limited)", "1");
    }
    if (want(report2d::RAINFALL)) {
        ds_face_rainfall_ =
            createFaceDS("Mesh2_face_rainfall", "rainfall intensity", "m s-1", "rainfall_rate");
        // Cumulative rainfall VOLUME per cell (m³) at this report time.
        // The final mass_balance_2d/rainfall_in can include a trailing
        // interval after the last scheduled report.
        ds_face_rain_cum_ = createFaceDS("Mesh2_face_rain_cum", "cumulative rainfall volume", "m3");
    }
    if (want(report2d::COUPLING)) {
        ds_face_coupling_flux_ =
            createFaceDS("Mesh2_face_coupling_flux", "coupling flux with SWMM node", "m s-1");
        ds_face_net_source_ =
            createFaceDS("Mesh2_face_net_source", "net volumetric source/sink", "m s-1");
    }
    if (want(report2d::INFILTRATION)) {
        // Per-cell infiltration (plan §5.5.6): the held INFIL_STEP rate and
        // the cumulative infiltrated depth (all-zero when no
        // [2D_INFILTRATION*] rows resolved).
        ds_face_infil_rate_ =
            createFaceDS("Mesh2_face_infil_rate", "infiltration loss rate", "m s-1");
        ds_face_infil_cum_ =
            createFaceDS("Mesh2_face_infil_cum", "cumulative infiltrated depth", "m");
    }
    if (mixing_enabled_ && want(report2d::TURBULENCE)) {
        ds_face_eddy_viscosity_ = createFaceDS("Mesh2_face_eddy_viscosity", "Smagorinsky horizontal eddy viscosity", "m2 s-1");
        ds_face_strain_rate_ = createFaceDS("Mesh2_face_strain_rate", "horizontal strain rate magnitude", "s-1");
    }
    if (want(report2d::VELOCITY)) {
        ds_face_vx_ = createFaceDS("Mesh2_face_vx", momentum_velocity_ ? "cell velocity X from conserved momentum" : "cell-centred velocity X (RT0)", "m s-1");
        ds_face_vy_ = createFaceDS("Mesh2_face_vy", momentum_velocity_ ? "cell velocity Y from conserved momentum" : "cell-centred velocity Y (RT0)", "m s-1");
    }
    if (want(report2d::CONTINUITY))
        ds_face_continuity_err_ =
            createFaceDS("Mesh2_face_continuity_err", "per-cell continuity residual", "m3 s-1");

    if (want(report2d::EDGE_FLUX)) {
        // Edge flux [nTime, nFace, stride]
        ds_edge_flux_ = createUnlimitedDataset("Mesh2_edge_flux", 3, zero3, edge_chunk);
        writeStringAttr(ds_edge_flux_, "long_name", "normal flux through cell edges");
        // Volumetric edge flux F_e = -ψ·q·n_e·L_e, as published by the explicit
        // marcher after conveyance and availability limiting. Matches the
        // m3 s-1 units of the continuity-residual datasets that sum these fluxes.
        writeStringAttr(ds_edge_flux_, "units", "m3 s-1");
        writeStringAttr(ds_edge_flux_, "mesh", "Mesh2");
        writeStringAttr(ds_edge_flux_, "location", "edge");
    }

    if (want(report2d::NODE_HEAD)) {
        // Node head [nTime, nNode]
        ds_node_head_ = createUnlimitedDataset("Mesh2_node_head", 2, zero2n, node_chunk);
        writeStringAttr(ds_node_head_, "long_name", "reconstructed vertex head");
        writeStringAttr(ds_node_head_, "units", "m");
        writeStringAttr(ds_node_head_, "mesh", "Mesh2");
        writeStringAttr(ds_node_head_, "location", "node");

        // Node SIGNED depth [nTime, nNode] — wet-masked render reconstruction
        // (eta_v - z_v; wetted-contact gated, so current engines emit > 0 or
        // the 0 no-data sentinel; files from older engines may carry
        // negatives). This is the field renderers/profilers should
        // interpolate; Mesh2_node_head is the solver field (dry-cell head =
        // bed elevation) kept for back-compat.
        ds_node_depth_ = createUnlimitedDataset("Mesh2_node_depth", 2, zero2n, node_chunk);
        writeStringAttr(ds_node_depth_, "long_name",
                        "signed vertex water depth (wet-masked render reconstruction)");
        writeStringAttr(ds_node_depth_, "units", "m");
        writeStringAttr(ds_node_depth_, "mesh", "Mesh2");
        writeStringAttr(ds_node_depth_, "location", "node");
    }

    // --- Cumulative rendering envelopes (fixed [nFace], overwritten in place) ---
    if (want(report2d::ENVELOPES)) {
        ds_face_max_depth_ =
            createFaceEnvelopeDataset("Mesh2_face_max_depth", "maximum overland flow depth", "m");
        ds_face_max_velocity_ = createFaceEnvelopeDataset(
            "Mesh2_face_max_velocity", "maximum cell velocity magnitude", "m s-1");
        if (want(report2d::CONTINUITY))
            ds_face_max_continuity_err_ = createFaceEnvelopeDataset(
                "Mesh2_face_max_continuity_err", "maximum absolute per-cell continuity residual",
                "m3 s-1");
    }
    if (checkpoint_fresh_ && !checkpoint_write_failed_) {
        try {
            checkpoint_header_ = fileBytes();
        } catch (const std::exception& e) {
            checkpoint_write_failed_ = true;
            state_ = PluginState::ERROR;
            last_error_ = e.what();
        }
    }
}

// ============================================================================
// update() — append one time step (called from IO thread)
// ============================================================================

void Default2DOutputPlugin::enqueueSourceHistory(
    std::shared_ptr<const SourceResultHistory> history) {
    if (!history)
        throw std::invalid_argument("Cannot enqueue an absent source history.");
    std::lock_guard<std::mutex> lock(source_mutex_);
    source_pending_.push_back(std::move(history));
}
/**
 * @brief Drain immutable source snapshots into the open results file in queue order.
 * @return Zero after writing all drained snapshots; one and ERROR/last_error_ for a refused
 * extension or write. The queue is swapped into local storage under source_mutex_; HDF5 writes
 * require the calling owner already hold the global result-HDF5 mutex and a valid open file.
 * @note Each successful history replaces source_written_. Failure loses the remaining locally
 * drained queue entries rather than requeueing them, and earlier histories may already be
 * written. No whole-drain rollback. Shared histories remain owned during use; helper
 * exceptions/allocations may propagate.
 */
int Default2DOutputPlugin::writePendingSourceHistory() {
    std::vector<std::shared_ptr<const SourceResultHistory>> pending;
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        pending.swap(source_pending_);
    }
    for (const auto& history : pending) {
        auto error = source_written_ ? history->extends(*source_written_) : std::string{};
        if (error.empty())
            error = history->writeIntoOpenResult(file_id_);
        if (!error.empty()) {
            last_error_ = error;
            state_ = PluginState::ERROR;
            return 1;
        }
        source_written_ = history;
    }
    return 0;
}

/**
 * @brief Append a due immutable engine snapshot and flush queued accepted-source histories.
 * @param[in] snap Borrowed snapshot with stable matching mesh/species/layer/bed/LID identity and
 * owned/shared buffers alive through this call. sim_time is absolute engine DateTime days.
 * Surface geometry/hydraulics use SI; species units are supplied per row, surface age hours and
 * groundwater age seconds. Active layer/boundary buffers passed directly must cover their full
 * stored products.
 * @return Zero for success, cadence skip or absent file/surface; one for source-history refusal,
 * checked changing thermal layout or fresh-checkpoint write failure. Sets ERROR text for
 * reported layout/source failures. A zero return does not guarantee every normal-mode HDF5 call
 * succeeded.
 * @note Holds the global result mutex. Drains history before cadence/file checks; first report
 * is due. Appends time, selected correctly shaped fields and lazy species/buildup/GW/heat/snow
 * datasets, updates envelopes and advances n_steps_ only after tracked checks pass. Optional
 * absent/mismatched ordinary arrays are skipped. Late failure can leave an extended partial row,
 * earlier source writes, lazy handles and cadence changes; no rollback. Allocations/exceptions
 * can propagate. Requires owner thread serialization and no model/layout change after dataset
 * creation.
 */
int Default2DOutputPlugin::update(const SimulationSnapshot& snap) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    if (writePendingSourceHistory() != 0)
        return 1;
    if (file_id_ < 0 || snap.surface_tri_count == 0)
        return 0;

    // REPORT_2D_STEP cadence: update() arrives every [OPTIONS] REPORT_STEP;
    // write only when the 2D instant is due (first call always writes).
    if (report_2d_step_days_ > 0.0) {
        if (next_due_days_ < 0.0)
            next_due_days_ = snap.sim_time;
        if (snap.sim_time + 1e-9 < next_due_days_)
            return 0;
        // Advance in whole 2D steps so a late call cannot double-write.
        while (next_due_days_ <= snap.sim_time + 1e-9)
            next_due_days_ += report_2d_step_days_;
    }

    // Write time value
    {
        hsize_t new_dim = n_steps_ + 1;
        checkpointWriteStatus(H5Dset_extent(ds_time_, &new_dim));
        hid_t fspace = H5Dget_space(ds_time_);
        hsize_t offset = n_steps_, count = 1;
        checkpointWriteStatus(
            H5Sselect_hyperslab(fspace, H5S_SELECT_SET, &offset, nullptr, &count, nullptr));
        hid_t mspace = H5Screate_simple(1, &count, nullptr);
        checkpointWriteStatus(
            H5Dwrite(ds_time_, H5T_NATIVE_DOUBLE, mspace, fspace, H5P_DEFAULT, &snap.sim_time));
        H5Sclose(mspace);
        H5Sclose(fspace);
    }

    // Per-face fields — each guarded by its dataset handle, which is
    // H5I_INVALID_HID for groups REPORT_2D_VARIABLES left out.
    auto face = [&](hid_t ds, const std::vector<double>& v) {
        if (ds != H5I_INVALID_HID && v.size() == static_cast<std::size_t>(n_faces_))
            extendAndWrite2D(ds, v.data(), n_faces_);
    };
    face(ds_face_depth_, snap.surface_depth);
    face(ds_face_head_, snap.surface_head);
    face(ds_face_grad_hx_, snap.surface_grad_hx);
    face(ds_face_grad_hy_, snap.surface_grad_hy);
    face(ds_face_grad_hx_lim_, snap.surface_grad_hx_lim);
    face(ds_face_grad_hy_lim_, snap.surface_grad_hy_lim);
    face(ds_face_rainfall_, snap.surface_rainfall);
    face(ds_face_coupling_flux_, snap.surface_coupling_flux);
    face(ds_face_net_source_, snap.surface_net_source);
    face(ds_face_infil_rate_, snap.surface_infil_rate);
    face(ds_face_infil_cum_, snap.surface_infil_cum);
    face(ds_face_rain_cum_, snap.surface_rain_cum);
    if (want(report2d::RAINFALL) && !snap.surface_snow[0].empty()) {
        createSnowDatasets(snap);
        for (std::size_t k = 0; k < ds_snow_.size(); ++k)
            face(ds_snow_[k], snap.surface_snow[k]);
    }

    if (mixing_diagnostics_ && mixing_edges_ && mixing_mesh_ &&
        snap.surface_depth.size() == static_cast<std::size_t>(n_faces_) &&
        snap.surface_face_vx.size() == snap.surface_depth.size() &&
        snap.surface_face_vy.size() == snap.surface_depth.size()) {
        mixing_edges_->refreshConveyance(*mixing_mesh_);
        auto qx = snap.surface_face_vx, qy = snap.surface_face_vy;
        for (std::size_t k = 0; k < qx.size(); ++k) {
            qx[k] *= snap.surface_depth[k]; qy[k] *= snap.surface_depth[k];
        }
        std::vector<double> rx, ry;
        mixing_diagnostics_->evaluate(*mixing_mesh_, *mixing_edges_, snap.surface_depth,
            snap.surface_head, qx, qy, mixing_dry_, mixing_cs_, rx, ry);
        face(ds_face_eddy_viscosity_, mixing_diagnostics_->viscosity);
        face(ds_face_strain_rate_, mixing_diagnostics_->strain);
    }
    face(ds_face_vx_, snap.surface_face_vx);
    face(ds_face_vy_, snap.surface_face_vy);
    face(ds_face_continuity_err_, snap.surface_continuity_err);

    // Per-edge fields [nFace, edge_stride] (the snapshot is already packed to
    // the public stride by SWMMEngine::fillSurfaceSnapshot).
    if (ds_edge_flux_ != H5I_INVALID_HID &&
        snap.surface_edge_flux.size() == static_cast<std::size_t>(n_faces_ * edge_stride_))
        extendAndWrite3D(ds_edge_flux_, snap.surface_edge_flux.data(), n_faces_, edge_stride_);

    // Overland transport S1: species concentration [nTime, nSpecies, nFace].
    // Created LAZILY on the first step that carries species, and only then:
    // a model with no 2D transport gets no variable, which is the correct
    // statement. REPORT_2D_SPECIES narrows the rows written; the
    // species_names attribute always lists exactly the rows in the file.
    if (want(report2d::SPECIES) && snap.surface_species_count > 0 &&
        snap.surface_species_conc.size() ==
            static_cast<std::size_t>(snap.surface_species_count) * n_faces_) {
        if (ds_face_species_conc_ == H5I_INVALID_HID) {
            // Resolve the row selection once, by name against the surface's
            // own row names (S4) or the pollutant list (pre-S4 fallback).
            const std::vector<std::string>* names_src =
                snap.surface_species_names ? snap.surface_species_names : snap.pollut_names;
            species_rows_.clear();
            const int n_in = snap.surface_species_count;
            if (species_filter_.empty() || !names_src) {
                for (int i = 0; i < n_in; ++i)
                    species_rows_.push_back(i);
            } else {
                for (int i = 0; i < n_in && i < static_cast<int>(names_src->size()); ++i) {
                    const std::string& nm = (*names_src)[static_cast<std::size_t>(i)];
                    for (const auto& want_nm : species_filter_) {
                        if (report2d::iequalsTok(want_nm, nm.c_str())) {
                            species_rows_.push_back(i);
                            break;
                        }
                    }
                }
            }
            if (species_rows_.empty()) {
                // Nothing selected matches — write nothing rather than an
                // empty block; the mask says SPECIES, the filter said none.
                n_species_ = 0;
            } else {
                n_species_ = static_cast<hsize_t>(species_rows_.size());
                hsize_t zero3s[3] = {0, n_species_, n_faces_};
                hsize_t chunk3[3] = {1, n_species_, std::min<hsize_t>(n_faces_, 4096)};
                ds_face_species_conc_ =
                    createUnlimitedDataset("Mesh2_face_species_conc", 3, zero3s, chunk3);
                writeStringAttr(ds_face_species_conc_, "long_name",
                                "surface species concentration");
                // Species units are per species; the dataset carries the
                // layout, a name list and (S4b) a parallel unit list so a
                // reader can join. "1" here means "see species_units".
                writeStringAttr(ds_face_species_conc_, "units", "1");
                writeStringAttr(ds_face_species_conc_, "mesh", "Mesh2");
                writeStringAttr(ds_face_species_conc_, "location", "face");
                writeStringAttr(ds_face_species_conc_, "layout",
                                "[time, species, face]; species order = "
                                "the species_names list (a REPORT_2D_SPECIES "
                                "subset of pollutants, MSX, __WATER_AGE__, "
                                "__TEMPERATURE__); dry cell reports 0");
                if (names_src) {
                    std::string names;
                    for (std::size_t k = 0; k < species_rows_.size(); ++k) {
                        const auto r = static_cast<std::size_t>(species_rows_[k]);
                        if (r >= names_src->size())
                            break;
                        if (k)
                            names += ",";
                        names += (*names_src)[r];
                    }
                    if (!names.empty())
                        writeStringAttr(ds_face_species_conc_, "species_names", names.c_str());
                }
                // S4b: species_units — one label per selected row, same
                // order as species_names; __WATER_AGE__ is reported in hours.
                if (snap.surface_species_units) {
                    std::string units;
                    for (std::size_t k = 0; k < species_rows_.size(); ++k) {
                        const auto r = static_cast<std::size_t>(species_rows_[k]);
                        if (r >= snap.surface_species_units->size())
                            break;
                        if (k)
                            units += ",";
                        units += (*snap.surface_species_units)[r];
                    }
                    if (!units.empty())
                        writeStringAttr(ds_face_species_conc_, "species_units", units.c_str());
                }
            }
        }
        if (ds_face_species_conc_ != H5I_INVALID_HID && n_species_ > 0)
            writeSpeciesRows(snap.surface_species_conc.data(),
                             static_cast<hsize_t>(snap.surface_species_count));
    }

    // S7: cell buildup per unit land area, one row per surface species (the
    // pollutant and MSX rows, unfiltered — the store is what the user
    // authored, and it is small). Absent when no [2D_COVERAGES] resolved.
    if (want(report2d::BUILDUP) && snap.surface_buildup_count > 0 &&
        snap.surface_buildup.size() ==
            static_cast<std::size_t>(snap.surface_buildup_count) * n_faces_) {
        if (ds_face_buildup_ == H5I_INVALID_HID) {
            n_buildup_ = static_cast<hsize_t>(snap.surface_buildup_count);
            hsize_t zero3b[3] = {0, n_buildup_, n_faces_};
            hsize_t chunk3b[3] = {1, n_buildup_, std::min<hsize_t>(n_faces_, 4096)};
            ds_face_buildup_ = createUnlimitedDataset("Mesh2_face_buildup", 3, zero3b, chunk3b);
            writeStringAttr(ds_face_buildup_, "long_name",
                            "surface pollutant buildup per unit land area");
            // The 1D washoff summary's unit: lbs/acre (US) or kg/ha (SI);
            // a reactions species declared in UG scales through its own
            // unit factor — see species_units for the concentration side.
            writeStringAttr(ds_face_buildup_, "units",
                            (snap.flow_units_code >= 3) ? "kg ha-1" : "lb acre-1");
            writeStringAttr(ds_face_buildup_, "mesh", "Mesh2");
            writeStringAttr(ds_face_buildup_, "location", "face");
            writeStringAttr(ds_face_buildup_, "layout",
                            "[time, species, face]; species order = the first "
                            "rows of species_names (pollutants, then MSX)");
            if (snap.surface_species_names) {
                std::string names;
                for (hsize_t k = 0; k < n_buildup_ && k < snap.surface_species_names->size(); ++k) {
                    if (k)
                        names += ",";
                    names += (*snap.surface_species_names)[static_cast<std::size_t>(k)];
                }
                if (!names.empty())
                    writeStringAttr(ds_face_buildup_, "species_names", names.c_str());
            }
        }
        if (ds_face_buildup_ != H5I_INVALID_HID &&
            n_buildup_ == static_cast<hsize_t>(snap.surface_buildup_count))
            extendAndWrite3D(ds_face_buildup_, snap.surface_buildup.data(), n_buildup_, n_faces_);
    }

    // G-O: the two-zone aquifer. Lazy like the species block: a model with no
    // [2D_AQUIFER] gets no groundwater variable at all.
    if (want(report2d::GROUNDWATER) && snap.gw2d_active &&
        snap.gw2d_hg.size() == static_cast<std::size_t>(n_faces_)) {
        if (!gw_created_)
            createGroundwaterDatasets(snap);
        const std::vector<double>* fields[kGwFaceFields] = {&snap.gw2d_table_elev,
                                                            &snap.gw2d_hg,
                                                            &snap.gw2d_hu,
                                                            &snap.gw2d_recharge,
                                                            &snap.gw2d_lateral,
                                                            &snap.gw2d_node_exchange,
                                                            &snap.gw2d_deep,
                                                            &snap.gw2d_et,
                                                            &snap.gw2d_dunne,
                                                            &snap.gw2d_infil_in,
                                                            &snap.gw2d_link_seepage,
                                                            &snap.gw2d_infil_capacity,
                                                            &snap.gw2d_infil_remaining,
                                                            &snap.gw2d_infil_refresh,
                                                            &snap.gw2d_infil_pending,
                                                            &snap.gw2d_reject,
                                                            &snap.gw2d_reject_cum,
                                                            &snap.gw2d_dunne_cum,
                                                            &snap.gw2d_et_pending,
                                                            &snap.gw2d_et_potential_cum,
                                                            &snap.gw2d_et_surface_cum,
                                                            &snap.gw2d_et_soil_cum,
                                                            &snap.gw2d_et_unused_cum,
                                                            &snap.gw2d_et_surface,
                                                            &snap.gw2d_et_potential,
                                                            &snap.gw2d_et_stress,
                                                            &snap.gw2d_et_refresh};
        for (int k = 0; k < kGwFaceFields; ++k)
            face(ds_gw_face_[k], *fields[k]);
        if (ds_gw_ledger_ != H5I_INVALID_HID && snap.gw2d_ledger.size() == kGwLedgerTerms)
            extendAndWrite2D(ds_gw_ledger_, snap.gw2d_ledger.data(), kGwLedgerTerms);
        // T7.5
        if (n_gw_species_ > 0) {
            const auto want =
                static_cast<std::size_t>(n_gw_species_) * static_cast<std::size_t>(n_faces_);
            if (ds_gw_sat_conc_ != H5I_INVALID_HID && snap.gw2d_sat_conc.size() == want)
                extendAndWrite3D(ds_gw_sat_conc_, snap.gw2d_sat_conc.data(), n_gw_species_,
                                 n_faces_);
            if (ds_gw_unsat_conc_ != H5I_INVALID_HID && snap.gw2d_unsat_conc.size() == want)
                extendAndWrite3D(ds_gw_unsat_conc_, snap.gw2d_unsat_conc.data(), n_gw_species_,
                                 n_faces_);
            if (ds_gw_species_ledger_ != H5I_INVALID_HID &&
                snap.gw2d_species_ledger.size() ==
                    static_cast<std::size_t>(n_gw_species_) * kGwSpeciesLedgerTerms)
                extendAndWrite3D(ds_gw_species_ledger_, snap.gw2d_species_ledger.data(),
                                 n_gw_species_, kGwSpeciesLedgerTerms);
        }
        if (ds_gw_layer_mass_ >= 0)
            extendAndWrite4D(ds_gw_layer_mass_, snap.gw2d_layer_mass.data(), n_gw_species_,
                             n_gw_transport_layers_, n_faces_);
        if (ds_gw_layer_conc_ >= 0)
            extendAndWrite4D(ds_gw_layer_conc_, snap.gw2d_layer_concentration.data(), n_gw_species_,
                             n_gw_transport_layers_, n_faces_);
        if (ds_gw_boundary_water_ >= 0)
            extendAndWrite3D(ds_gw_boundary_water_, snap.gw2d_boundary_water.data(),
                             n_gw_boundaries_, 4);
        if (ds_gw_boundary_species_ >= 0)
            extendAndWrite4D(ds_gw_boundary_species_, snap.gw2d_boundary_species.data(),
                             n_gw_species_, n_gw_boundaries_, 4);
        if (ds_gw_transport_ledger_ >= 0)
            extendAndWrite3D(ds_gw_transport_ledger_, snap.gw2d_transport_ledger.data(),
                             n_gw_species_, 20);
        if (ds_gw_bed_exchange_ != H5I_INVALID_HID &&
            snap.gw2d_bed_exchange_cum.size() == static_cast<std::size_t>(n_gw_beds_))
            extendAndWrite2D(ds_gw_bed_exchange_, snap.gw2d_bed_exchange_cum.data(), n_gw_beds_);
        if (ds_gw_theta_ != H5I_INVALID_HID &&
            snap.gw2d_theta_sigma.size() == static_cast<std::size_t>(n_gw_layers_ * n_faces_))
            extendAndWrite3D(ds_gw_theta_, snap.gw2d_theta_sigma.data(), n_gw_layers_, n_faces_);
    }

    // P9: optional thermal fields are created only for an active thermal
    // owner. Their absent-vs-NaN semantics distinguish an unavailable model
    // from an uncovered cell in a model that is active elsewhere.
    if (want(report2d::HEAT) && snap.thermal2d_active) {
        const auto n_lid = static_cast<std::size_t>(std::max(0, snap.thermal2d_lid_layer_count));
        const bool lid_shape_ok = snap.thermal2d_lid_layer_count >= 0 &&
                                  snap.thermal2d_lid_layer_node_index.size() == n_lid &&
                                  snap.thermal2d_lid_layer_face_index.size() == n_lid &&
                                  snap.thermal2d_lid_layer_physical_index.size() == n_lid &&
                                  snap.thermal2d_lid_layer_control_index.size() == n_lid &&
                                  snap.thermal2d_lid_layer_authored_index.size() == n_lid &&
                                  snap.thermal2d_lid_layer_node_id.size() == n_lid &&
                                  snap.thermal2d_lid_layer_top_m.size() == n_lid &&
                                  snap.thermal2d_lid_layer_bottom_m.size() == n_lid &&
                                  snap.thermal2d_lid_layer_area_m2.size() == n_lid &&
                                  snap.thermal2d_lid_layer_temperature.size() == n_lid &&
                                  snap.thermal2d_lid_layer_energy.size() == n_lid &&
                                  snap.thermal2d_lid_layer_capacity.size() == n_lid &&
                                  snap.thermal2d_lid_layer_ground_contact_j.size() == n_lid;
        if (!lid_shape_ok) {
            last_error_ =
                "Mesh LID thermal output snapshot has inconsistent layer mappings or state arrays.";
            state_ = PluginState::ERROR;
            return 1;
        }
        if (!heat_created_)
            createThermalDatasets(snap);
        if (static_cast<hsize_t>(n_lid) != n_heat_lid_layers_) {
            last_error_ =
                "Mesh LID thermal layer mapping changed after output datasets were created.";
            state_ = PluginState::ERROR;
            return 1;
        }
        auto writeThermalFace = [&](hid_t ds, const std::vector<double>& v) {
            if (ds != H5I_INVALID_HID && v.size() == static_cast<std::size_t>(n_faces_))
                extendAndWrite2D(ds, v.data(), n_faces_);
        };
        writeThermalFace(ds_heat_water_temp_, snap.thermal2d_water_temperature);
        writeThermalFace(ds_heat_bed_temp_, snap.thermal2d_bed_temperature);
        writeThermalFace(ds_heat_bed_energy_, snap.thermal2d_bed_energy);
        writeThermalFace(ds_heat_bed_initial_energy_, snap.thermal2d_bed_initial_energy);
        writeThermalFace(ds_heat_bed_capacity_, snap.thermal2d_bed_capacity);
        writeThermalFace(ds_heat_bed_pore_volume_, snap.thermal2d_bed_pore_volume);
        writeThermalFace(ds_heat_bed_age_generation_, snap.thermal2d_bed_age_generation);
        if (n_heat_bed_species_ > 0) {
            if (snap.thermal2d_bed_species_count != static_cast<int>(n_heat_bed_species_) ||
                snap.thermal2d_bed_species_concentration.size() !=
                    static_cast<std::size_t>(n_heat_bed_species_ * n_faces_) ||
                snap.thermal2d_bed_species_initial_amount.size() !=
                    static_cast<std::size_t>(n_heat_bed_species_ * n_faces_) ||
                snap.thermal2d_bed_species_exchange.size() !=
                    static_cast<std::size_t>(n_heat_bed_species_ * n_faces_)) {
                last_error_ =
                    "Local mesh bed species output layout changed after datasets were created.";
                state_ = PluginState::ERROR;
                return 1;
            }
            extendAndWrite3D(ds_heat_bed_species_conc_,
                             snap.thermal2d_bed_species_concentration.data(), n_heat_bed_species_,
                             n_faces_);
            extendAndWrite3D(ds_heat_bed_species_initial_amount_,
                             snap.thermal2d_bed_species_initial_amount.data(), n_heat_bed_species_,
                             n_faces_);
            extendAndWrite3D(ds_heat_bed_species_exchange_,
                             snap.thermal2d_bed_species_exchange.data(), n_heat_bed_species_,
                             n_faces_);
        }
        writeThermalFace(ds_heat_evaporation_, snap.thermal2d_evaporation_m3);
        writeThermalFace(ds_heat_ground_temp_, snap.thermal2d_ground_temperature);
        writeThermalFace(ds_heat_ground_energy_, snap.thermal2d_ground_energy);
        writeThermalFace(ds_heat_ground_capacity_, snap.thermal2d_ground_capacity);
        if (ds_heat_surface_budget_ != H5I_INVALID_HID && snap.thermal2d_surface_budget_terms > 0 &&
            snap.thermal2d_surface_budget.size() ==
                static_cast<std::size_t>(n_faces_) * snap.thermal2d_surface_budget_terms)
            extendAndWrite3D(ds_heat_surface_budget_, snap.thermal2d_surface_budget.data(),
                             n_faces_, static_cast<hsize_t>(snap.thermal2d_surface_budget_terms));
        if (n_heat_layers_ > 0) {
            const auto expected = static_cast<std::size_t>(n_heat_layers_ * n_faces_);
            if (ds_heat_layer_temp_ != H5I_INVALID_HID &&
                snap.thermal2d_ground_layer_temperature.size() == expected)
                extendAndWrite3D(ds_heat_layer_temp_,
                                 snap.thermal2d_ground_layer_temperature.data(), n_heat_layers_,
                                 n_faces_);
            if (ds_heat_layer_energy_ != H5I_INVALID_HID &&
                snap.thermal2d_ground_layer_energy.size() == expected)
                extendAndWrite3D(ds_heat_layer_energy_, snap.thermal2d_ground_layer_energy.data(),
                                 n_heat_layers_, n_faces_);
            if (ds_heat_layer_capacity_ != H5I_INVALID_HID &&
                snap.thermal2d_ground_layer_capacity.size() == expected)
                extendAndWrite3D(ds_heat_layer_capacity_,
                                 snap.thermal2d_ground_layer_capacity.data(), n_heat_layers_,
                                 n_faces_);
        }
        if (ds_heat_ground_budget_ != H5I_INVALID_HID && snap.thermal2d_ground_budget_terms > 0 &&
            snap.thermal2d_ground_budget.size() ==
                static_cast<std::size_t>(n_faces_) * snap.thermal2d_ground_budget_terms)
            extendAndWrite3D(ds_heat_ground_budget_, snap.thermal2d_ground_budget.data(), n_faces_,
                             static_cast<hsize_t>(snap.thermal2d_ground_budget_terms));
        if (n_heat_lid_layers_ > 0) {
            extendAndWrite2D(ds_heat_lid_temp_, snap.thermal2d_lid_layer_temperature.data(),
                             n_heat_lid_layers_);
            extendAndWrite2D(ds_heat_lid_energy_, snap.thermal2d_lid_layer_energy.data(),
                             n_heat_lid_layers_);
            extendAndWrite2D(ds_heat_lid_capacity_, snap.thermal2d_lid_layer_capacity.data(),
                             n_heat_lid_layers_);
            extendAndWrite2D(ds_heat_lid_ground_contact_,
                             snap.thermal2d_lid_layer_ground_contact_j.data(), n_heat_lid_layers_);
        }
    }

    // Per-node fields
    if (ds_node_head_ != H5I_INVALID_HID &&
        snap.surface_vert_head.size() == static_cast<std::size_t>(n_nodes_))
        extendAndWrite2D(ds_node_head_, snap.surface_vert_head.data(), n_nodes_);
    if (ds_node_depth_ != H5I_INVALID_HID &&
        static_cast<hsize_t>(snap.surface_vert_depth.size()) == n_nodes_)
        extendAndWrite2D(ds_node_depth_, snap.surface_vert_depth.data(), n_nodes_);

    // Overwrite cumulative envelopes in place (monotone; last write is final).
    if (snap.surface_stat_max_depth.size() == n_faces_)
        writeFaceEnvelope(ds_face_max_depth_, snap.surface_stat_max_depth.data());
    if (snap.surface_stat_max_velocity.size() == n_faces_)
        writeFaceEnvelope(ds_face_max_velocity_, snap.surface_stat_max_velocity.data());
    if (snap.surface_stat_max_cont_err.size() == n_faces_)
        writeFaceEnvelope(ds_face_max_continuity_err_, snap.surface_stat_max_cont_err.data());

    if (checkpoint_write_failed_)
        return 1;
    ++n_steps_;
    return 0;
}

/**
 * @brief Lazily create physical/local-bed heat, native bed porewater, fixed-depth soil and
 * physical LID mapping/state datasets.
 * @param[in] snap Stable first active snapshot with matching mesh counts and consistent parallel
 * metadata/layout arrays. All datasets retain that identity through later reports; pointers are
 * not retained. Units/layout are recorded per field and species, with SI geometry and native row
 * metadata.
 * @note Requires prepared file, caller-held global result mutex and no prior creation of the
 * same names. Sets its created flag before completing setup, so a failure may retain a partial
 * dataset set. Creates owned dataset handles and copies static metadata; zero-time fields are
 * populated by update(). Missing optional arrays omit corresponding fields; NaN in supported
 * output denotes unavailable cell ownership/quantity. Allocations or HDF5 setup can fail after
 * partial file/handle/count mutation; no rollback and not all setup statuses are promoted to
 * writer error. Does not change simulation state.
 */
void Default2DOutputPlugin::createThermalDatasets(const SimulationSnapshot& snap) {
    heat_created_ = true;
    const hsize_t zero2[2]{0, n_faces_}, face_chunk[2]{1, std::min<hsize_t>(n_faces_, 4096)};
    auto createFace = [&](const char* name, const char* label, const char* units) {
        hid_t ds = createUnlimitedDataset(name, 2, zero2, face_chunk);
        writeStringAttr(ds, "long_name", label);
        writeStringAttr(ds, "units", units);
        writeStringAttr(ds, "mesh", "Mesh2");
        writeStringAttr(ds, "location", "face");
        writeStringAttr(ds, "missing_values",
                        "NaN means this cell has no owner for the reported quantity");
        return ds;
    };
    auto createBudget = [&](const char* name, const char* label, int terms,
                            const char* names) -> hid_t {
        if (terms <= 0)
            return H5I_INVALID_HID;
        const hsize_t zero3[3]{0, n_faces_, static_cast<hsize_t>(terms)};
        const hsize_t chunk3[3]{1, std::min<hsize_t>(n_faces_, 1024), static_cast<hsize_t>(terms)};
        hid_t ds = createUnlimitedDataset(name, 3, zero3, chunk3);
        writeStringAttr(ds, "long_name", label);
        writeStringAttr(ds, "units", "J");
        writeStringAttr(ds, "mesh", "Mesh2");
        writeStringAttr(ds, "location", "face");
        writeStringAttr(ds, "layout", "[time, face, term]");
        writeStringAttr(ds, "terms", names);
        writeStringAttr(ds, "cell_methods", "time: sum (cumulative since initialization)");
        return ds;
    };
    if (snap.thermal2d_water_temperature.size() == static_cast<std::size_t>(n_faces_))
        ds_heat_water_temp_ = createFace("Mesh2_face_water_temperature",
                                         "overland water temperature", "degree_Celsius");
    if (snap.thermal2d_bed_temperature.size() == static_cast<std::size_t>(n_faces_)) {
        ds_heat_bed_temp_ =
            createFace("Mesh2_face_bed_temperature", "mesh bed temperature", "degree_Celsius");
        ds_heat_bed_energy_ = createFace("Mesh2_face_bed_energy",
                                         "mesh bed sensible energy relative to 0 Celsius", "J");
        ds_heat_bed_capacity_ =
            createFace("Mesh2_face_bed_heat_capacity", "mesh bed heat capacity", "J K-1");
    }
    if (snap.thermal2d_bed_initial_energy.size() == static_cast<std::size_t>(n_faces_)) {
        ds_heat_bed_initial_energy_ =
            createFace("Mesh2_face_bed_initial_energy",
                       "initial local mesh-bed sensible energy relative to 0 Celsius", "J");
        writeStringAttr(ds_heat_bed_initial_energy_, "cell_methods",
                        "time: point (constant initial condition)");
    }
    if (snap.thermal2d_bed_pore_volume.size() == static_cast<std::size_t>(n_faces_))
        ds_heat_bed_pore_volume_ = createFace("Mesh2_face_bed_porewater_volume",
                                              "local mesh bed fixed porewater volume", "m3");
    const auto bed_species_size =
        static_cast<std::size_t>(std::max(0, snap.thermal2d_bed_species_count));
    if (bed_species_size > 0 && snap.thermal2d_bed_species_names.size() == bed_species_size &&
        snap.thermal2d_bed_species_units.size() == bed_species_size &&
        snap.thermal2d_bed_species_concentration.size() == bed_species_size * n_faces_ &&
        snap.thermal2d_bed_species_initial_amount.size() == bed_species_size * n_faces_ &&
        snap.thermal2d_bed_species_exchange.size() == bed_species_size * n_faces_) {
        n_heat_bed_species_ = static_cast<hsize_t>(bed_species_size);
        const hsize_t zero3[3]{0, n_heat_bed_species_, n_faces_};
        const hsize_t chunk3[3]{1, n_heat_bed_species_, std::min<hsize_t>(n_faces_, 4096)};
        auto createBedSpecies = [&](const char* name, const char* label, const char* layout,
                                    bool amount) {
            hid_t ds = createUnlimitedDataset(name, 3, zero3, chunk3);
            writeStringAttr(ds, "long_name", label);
            writeStringAttr(ds, "units", "1");
            writeStringAttr(ds, "mesh", "Mesh2");
            writeStringAttr(ds, "location", "bed porewater");
            writeStringAttr(ds, "layout", layout);
            std::string names, units;
            for (std::size_t row = 0; row < bed_species_size; ++row) {
                if (row) {
                    names += ",";
                    units += ",";
                }
                names += snap.thermal2d_bed_species_names[row];
                units += snap.thermal2d_bed_species_units[row];
                if (amount)
                    units += " m3";
            }
            writeStringAttr(ds, "species_names", names.c_str());
            writeStringAttr(ds, "species_units", units.c_str());
            if (amount)
                writeStringAttr(ds, "cell_methods",
                                "time: point (constant initial-condition amount)");
            return ds;
        };
        ds_heat_bed_species_conc_ = createBedSpecies(
            "Mesh2_face_bed_species_conc", "local bed porewater species concentration",
            "[time, species, face]; concentration in bed porewater; age in hours", false);
        ds_heat_bed_species_initial_amount_ = createBedSpecies(
            "Mesh2_face_bed_species_initial_amount", "initial local bed porewater species amount",
            "[time, species, face]; initial concentration multiplied by fixed porewater volume; age in hour m3",
            true);
        ds_heat_bed_species_exchange_ = createBedSpecies(
            "Mesh2_face_bed_species_exchange",
            "cumulative signed surface/bed species exchange amount",
            "[time, species, face]; positive from surface water into bed; age in hour m3", true);
    }
    if (snap.thermal2d_bed_age_generation.size() == static_cast<std::size_t>(n_faces_)) {
        ds_heat_bed_age_generation_ =
            createFace("Mesh2_face_bed_age_generation",
                       "cumulative age generated in local bed porewater", "hour m3");
    }
    if (snap.thermal2d_surface_budget.size() ==
        static_cast<std::size_t>(n_faces_) * snap.thermal2d_surface_budget_terms) {
        ds_heat_surface_budget_ = createBudget(
            "mesh_heat_budget", "Mesh surface thermal receipts",
            snap.thermal2d_surface_budget_terms,
            "radiation_j,sensible_j,latent_j,deep_boundary_j,water_to_bed_j,water_thermal_increment_j");
        writeStringAttr(
            ds_heat_surface_budget_, "sign_convention",
            "positive into the named external store; water_to_bed_j is positive water to bed and is internal to the combined system");
    }
    if (snap.thermal2d_evaporation_m3.size() == static_cast<std::size_t>(n_faces_))
        ds_heat_evaporation_ = createFace("Mesh2_face_accepted_evaporation_cum",
                                          "cumulative accepted surface evaporation", "m3");
    if (snap.thermal2d_ground_temperature.size() == static_cast<std::size_t>(n_faces_)) {
        ds_heat_ground_temp_ = createFace("Mesh2_face_soil_temperature",
                                          "capacity-weighted soil temperature", "degree_Celsius");
        ds_heat_ground_energy_ =
            createFace("Mesh2_face_soil_energy", "soil sensible energy relative to 0 Celsius", "J");
        ds_heat_ground_capacity_ =
            createFace("Mesh2_face_soil_heat_capacity", "soil heat capacity", "J K-1");
    }
    n_heat_layers_ = static_cast<hsize_t>(std::max(0, snap.thermal2d_layer_count));
    const auto layer_size = static_cast<std::size_t>(n_heat_layers_ * n_faces_);
    if (n_heat_layers_ > 0 && snap.thermal2d_ground_layer_temperature.size() == layer_size &&
        snap.thermal2d_ground_layer_energy.size() == layer_size &&
        snap.thermal2d_ground_layer_capacity.size() == layer_size) {
        const hsize_t zero3[3]{0, n_heat_layers_, n_faces_};
        const hsize_t chunk3[3]{1, n_heat_layers_, std::min<hsize_t>(n_faces_, 2048)};
        auto createLayer = [&](const char* name, const char* label, const char* units) {
            hid_t ds = createUnlimitedDataset(name, 3, zero3, chunk3);
            writeStringAttr(ds, "long_name", label);
            writeStringAttr(ds, "units", units);
            writeStringAttr(ds, "mesh", "Mesh2");
            writeStringAttr(ds, "location", "face");
            writeStringAttr(
                ds, "layout",
                "[time, layer, face]; layers are fixed-depth thermal stores, top layer first");
            writeStringAttr(ds, "missing_values",
                            "NaN means this cell has no thermal layer at that index");
            return ds;
        };
        ds_heat_layer_temp_ = createLayer("Mesh2_face_soil_layer_temperature",
                                          "fixed-grid soil layer temperature", "degree_Celsius");
        ds_heat_layer_energy_ =
            createLayer("Mesh2_face_soil_layer_energy",
                        "fixed-grid soil layer sensible energy relative to 0 Celsius", "J");
        ds_heat_layer_capacity_ = createLayer("Mesh2_face_soil_layer_heat_capacity",
                                              "fixed-grid soil layer heat capacity", "J K-1");
        if (snap.thermal2d_ground_layer_center_m.size() == layer_size) {
            const hsize_t dims[2]{n_heat_layers_, n_faces_};
            hid_t space = H5Screate_simple(2, dims, nullptr);
            hid_t ds =
                io::createHdf5Dataset(file_id_, "Mesh2_face_soil_layer_center_depth", storage_type_,
                                      space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
            checkpointWriteStatus(H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                                           snap.thermal2d_ground_layer_center_m.data()));
            writeStringAttr(ds, "long_name",
                            "fixed-grid soil layer center depth below ground surface");
            writeStringAttr(ds, "units", "m");
            writeStringAttr(ds, "mesh", "Mesh2");
            writeStringAttr(ds, "location", "face");
            writeStringAttr(ds, "layout",
                            "[layer, face]; depth positive downward; NaN means no layer");
            H5Dclose(ds);
            H5Sclose(space);
        }
    }
    if (snap.thermal2d_ground_budget.size() ==
        static_cast<std::size_t>(n_faces_) * snap.thermal2d_ground_budget_terms) {
        ds_heat_ground_budget_ = createBudget(
            "ground_heat_budget", "Ground thermal state and cumulative receipts",
            snap.thermal2d_ground_budget_terms,
            "initial_j,advective_j,source_j,surface_boundary_j,deep_boundary_j,latent_j,lateral_net_j,water_contact_j,channel_contact_j,residual_j,pending_lateral_j,lid_contact_j");
        writeStringAttr(
            ds_heat_ground_budget_, "sign_convention",
            "cumulative signed receipts; contact terms are internal and equal-opposite on paired stores; residual is stored energy minus initial and receipts");
    }
    n_heat_lid_layers_ = static_cast<hsize_t>(std::max(0, snap.thermal2d_lid_layer_count));
    const auto n_lid = static_cast<std::size_t>(n_heat_lid_layers_);
    if (n_lid > 0) {
        const hsize_t dims[1]{n_heat_lid_layers_};
        auto createStatic = [&](const char* name, const char* label, const char* units,
                                hid_t file_type, hid_t memory_type, const void* values) {
            hid_t space = H5Screate_simple(1, dims, nullptr);
            hid_t ds = io::createHdf5Dataset(file_id_, name, file_type, space, H5P_DEFAULT,
                                             H5P_DEFAULT, H5P_DEFAULT);
            checkpointWriteStatus(H5Dwrite(ds, memory_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, values));
            writeStringAttr(ds, "long_name", label);
            writeStringAttr(ds, "units", units);
            writeStringAttr(ds, "mesh", "Mesh2");
            writeStringAttr(ds, "location", "LID layer");
            writeStringAttr(
                ds, "layout",
                "[lid_layer]; zero-based indices map each thermal store to its owning node, mesh face, and physical LID cell");
            H5Dclose(ds);
            H5Sclose(space);
        };
        createStatic("Mesh2_lid_layer_node_index",
                     "network node index for each mesh-connected LID thermal layer", "1",
                     H5T_NATIVE_INT, H5T_NATIVE_INT, snap.thermal2d_lid_layer_node_index.data());
        createStatic("Mesh2_lid_layer_face_index",
                     "mesh face index for each mesh-connected LID thermal layer", "1",
                     H5T_NATIVE_INT, H5T_NATIVE_INT, snap.thermal2d_lid_layer_face_index.data());
        createStatic("Mesh2_lid_layer_physical_cell_index",
                     "physical LID cell index within its owning node", "1", H5T_NATIVE_INT,
                     H5T_NATIVE_INT, snap.thermal2d_lid_layer_physical_index.data());
        createStatic("Mesh2_lid_layer_control_index",
                     "LID control index for each mesh-connected LID thermal layer", "1",
                     H5T_NATIVE_INT, H5T_NATIVE_INT, snap.thermal2d_lid_layer_control_index.data());
        createStatic("Mesh2_lid_layer_authored_index",
                     "authored LID layer index for each physical thermal cell", "1", H5T_NATIVE_INT,
                     H5T_NATIVE_INT, snap.thermal2d_lid_layer_authored_index.data());
        createStatic("Mesh2_lid_layer_top_elevation",
                     "LID physical-cell top elevation relative to node invert", "m",
                     H5T_NATIVE_DOUBLE, H5T_NATIVE_DOUBLE, snap.thermal2d_lid_layer_top_m.data());
        createStatic("Mesh2_lid_layer_bottom_elevation",
                     "LID physical-cell bottom elevation relative to node invert", "m",
                     H5T_NATIVE_DOUBLE, H5T_NATIVE_DOUBLE,
                     snap.thermal2d_lid_layer_bottom_m.data());
        createStatic("Mesh2_lid_layer_area", "LID physical-cell cross-sectional area", "m2",
                     H5T_NATIVE_DOUBLE, H5T_NATIVE_DOUBLE, snap.thermal2d_lid_layer_area_m2.data());

        hid_t string_type = H5Tcopy(H5T_C_S1);
        H5Tset_size(string_type, H5T_VARIABLE);
        H5Tset_cset(string_type, H5T_CSET_UTF8);
        hid_t string_space = H5Screate_simple(1, dims, nullptr);
        hid_t node_names =
            io::createHdf5Dataset(file_id_, "Mesh2_lid_layer_node_id", string_type, string_space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        std::vector<const char*> node_name_values;
        node_name_values.reserve(n_lid);
        for (const auto& name : snap.thermal2d_lid_layer_node_id)
            node_name_values.push_back(name.c_str());
        checkpointWriteStatus(H5Dwrite(node_names, string_type, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                                       node_name_values.data()));
        writeStringAttr(node_names, "long_name",
                        "network node ID for each mesh-connected LID thermal layer");
        writeStringAttr(node_names, "mesh", "Mesh2");
        writeStringAttr(node_names, "location", "LID layer");
        writeStringAttr(node_names, "layout", "[lid_layer]");
        H5Dclose(node_names);
        H5Sclose(string_space);
        H5Tclose(string_type);

        const hsize_t zero2[2]{0, n_heat_lid_layers_};
        const hsize_t chunk2[2]{1, std::min<hsize_t>(n_heat_lid_layers_, 4096)};
        auto createLidState = [&](const char* name, const char* label, const char* units,
                                  const char* missing) {
            hid_t ds = createUnlimitedDataset(name, 2, zero2, chunk2);
            writeStringAttr(ds, "long_name", label);
            writeStringAttr(ds, "units", units);
            writeStringAttr(ds, "mesh", "Mesh2");
            writeStringAttr(ds, "location", "LID layer");
            writeStringAttr(
                ds, "layout",
                "[time, lid_layer]; static Mesh2_lid_layer_* datasets identify the owning node, face and physical LID cell");
            writeStringAttr(ds, "missing_values", missing);
            return ds;
        };
        ds_heat_lid_temp_ = createLidState(
            "Mesh2_lid_layer_temperature", "mesh-connected LID physical-layer temperature",
            "degree_Celsius", "NaN means the physical cell currently has zero heat capacity");
        ds_heat_lid_energy_ = createLidState(
            "Mesh2_lid_layer_energy",
            "mesh-connected LID physical-layer sensible energy relative to 0 Celsius", "J",
            "Zero for a physical cell with zero heat capacity");
        ds_heat_lid_capacity_ = createLidState(
            "Mesh2_lid_layer_heat_capacity", "mesh-connected LID physical-layer heat capacity",
            "J K-1", "Zero for a physical cell with zero heat capacity");
        ds_heat_lid_ground_contact_ = createLidState(
            "Mesh2_lid_layer_ground_contact_energy",
            "cumulative signed ground-contact energy received by each mesh-connected LID physical layer",
            "J", "Signed cumulative receipt; positive into the LID layer");
    }
    writeStringAttr(file_id_, "thermal_result_schema_version", "6");
}

// Snow fields follow the rainfall report selection. They carry their actual
// accepted timestamp because local source clocks need not equal report time.
/**
 * @brief Lazily create accepted per-cell snow state and its actual completed-source timestamp
 * datasets.
 * @param[in] snap Stable first active snapshot with matching mesh counts and consistent parallel
 * metadata/layout arrays. All datasets retain that identity through later reports; pointers are
 * not retained. Units/layout are recorded per field and species, with SI geometry and native row
 * metadata.
 * @note Requires prepared file, caller-held global result mutex and no prior creation of the
 * same names. Snow selection returns for disabled RAINFALL, absent initial field or an existing
 * first snow dataset. Creates owned dataset handles and copies static metadata; zero-time fields
 * are populated by update(). Missing optional arrays omit corresponding fields; NaN in supported
 * output denotes unavailable cell ownership/quantity. Allocations or HDF5 setup can fail after
 * partial file/handle/count mutation; no rollback and not all setup statuses are promoted to
 * writer error. Does not change simulation state.
 */
void Default2DOutputPlugin::createSnowDatasets(const SimulationSnapshot& snap) {
    if (!want(report2d::RAINFALL) || snap.surface_snow[0].empty() || ds_snow_[0] >= 0)
        return;
    const hsize_t zero[2]{0, n_faces_}, chunk[2]{1, n_faces_};
    for (std::size_t k = 0; k < meshSnowFields.size(); ++k) {
        const auto& field = meshSnowFields[k];
        auto& id = ds_snow_[k];
        id = createUnlimitedDataset(field.name, 2, zero, chunk, H5T_NATIVE_DOUBLE);
        writeStringAttr(id, "long_name", field.label);
        writeStringAttr(id, "units", field.units);
        writeStringAttr(id, "mesh", "Mesh2");
        writeStringAttr(id, "location", "face");
        writeStringAttr(id, "state_time", "Mesh2_face_snow_completed");
        writeStringAttr(
            id, "missing_values",
            "NaN for unassigned cells or quantities unavailable for the selected model");
    }
}

// G-O: the aquifer's datasets, created on the first update() with a live kernel.
/**
 * @brief Lazily create aquifer hydraulic, native species/layers, boundary receipts and
 * cumulative balance datasets.
 * @param[in] snap Stable first active snapshot with matching mesh counts and consistent parallel
 * metadata/layout arrays. All datasets retain that identity through later reports; pointers are
 * not retained. Units/layout are recorded per field and species, with SI geometry and native row
 * metadata.
 * @note Requires prepared file, caller-held global result mutex and no prior creation of the
 * same names. Sets its created flag before completing setup, so a failure may retain a partial
 * dataset set. Creates owned dataset handles and copies static metadata; zero-time fields are
 * populated by update(). Missing optional arrays omit corresponding fields; NaN in supported
 * output denotes unavailable cell ownership/quantity. Allocations or HDF5 setup can fail after
 * partial file/handle/count mutation; no rollback and not all setup statuses are promoted to
 * writer error. Does not change simulation state.
 */
void Default2DOutputPlugin::createGroundwaterDatasets(const SimulationSnapshot& snap) {
    gw_created_ = true;
    hsize_t zero2[2] = {0, n_faces_};
    hsize_t face_chunk[2] = {1, std::min<hsize_t>(n_faces_, 4096)};
    struct F {
        const char* name;
        const char* long_name;
        const char* units;
    };
    // Same order as the `fields[]` table in update().
    static const F kFields[kGwFaceFields] = {
        {"Mesh2_face_gw_table_elev", "water-table elevation (aquifer bottom + saturated thickness)",
         "m"},
        {"Mesh2_face_gw_hg", "saturated thickness", "m"},
        {"Mesh2_face_gw_hu",
         "closure A unsaturated storage (equivalent depth of water; 0 under closure B)", "m"},
        {"Mesh2_face_gw_recharge",
         "recharge from the unsaturated to the saturated zone, positive down, negative = capillary rise (last firing, held)",
         "m s-1"},
        {"Mesh2_face_gw_lateral", "net lateral Darcy flow into the cell (last firing, held)",
         "m3 s-1"},
        {"Mesh2_face_gw_node_exchange",
         "exchange with the 1D node bed, positive out of the aquifer into the pipe (last firing, held)",
         "m3 s-1"},
        {"Mesh2_face_gw_deep", "deep percolation loss (last firing, held)", "m s-1"},
        {"Mesh2_face_gw_et", "subsurface evapotranspiration (last firing, held)", "m s-1"},
        {"Mesh2_face_gw_dunne", "saturation excess returned to the surface (last firing, held)",
         "m3 s-1"},
        {"Mesh2_face_gw_infil_in", "infiltration delivered from the surface (last firing, held)",
         "m s-1"},
        {"Mesh2_face_gw_link_seepage",
         "conduit seepage delivered into the cell, length-weighted over the conduits crossing it (last firing, held)",
         "m3 s-1"}, // G-X3
        {"Mesh2_face_gw_infil_capacity", "Aquifer top-interface receiving capacity", "m s-1"},
        {"Mesh2_face_gw_infil_remaining", "Remaining receiving allowance after pending receipts",
         "m3"},
        {"Mesh2_face_gw_infil_refresh", "Capacity refresh time since simulation start", "s"},
        {"Mesh2_face_gw_infil_pending", "Reserved surface water pending aquifer firing", "m3"},
        {"Mesh2_face_gw_reject", "Rejected top-interface delivery returned to surface", "m3 s-1"},
        {"Mesh2_face_gw_reject_cum", "Cumulative rejected top-interface delivery", "m3"},
        {"Mesh2_face_gw_dunne_cum", "Cumulative physical saturation excess", "m3"},
        {"Mesh2_face_et_pending", "Atmospheric demand awaiting the aquifer firing", "m3"},
        {"Mesh2_face_et_potential_cum", "Cumulative single atmospheric demand", "m3"},
        {"Mesh2_face_et_surface_cum", "Cumulative actual surface evaporation", "m3"},
        {"Mesh2_face_et_soil_cum", "Cumulative actual soil ET", "m3"},
        {"Mesh2_face_et_unused_cum", "Cumulative unused expired atmospheric demand", "m3"},
        {"Mesh2_face_et_surface", "Actual surface evaporation last surface interval", "m s-1"},
        {"Mesh2_face_et_potential", "Potential atmospheric loss last surface interval", "m s-1"},
        {"Mesh2_face_et_stress", "Single stress factor at the soil ET extraction location", "1"},
        {"Mesh2_face_et_refresh", "End of the last integrated surface ET interval", "s"},
    };
    for (int k = 0; k < kGwFaceFields; ++k) {
        hid_t ds = createUnlimitedDataset(kFields[k].name, 2, zero2, face_chunk);
        writeStringAttr(ds, "long_name", kFields[k].long_name);
        writeStringAttr(ds, "units", kFields[k].units);
        writeStringAttr(ds, "mesh", "Mesh2");
        writeStringAttr(ds, "location", "face");
        ds_gw_face_[k] = ds;
    }
    // Static descriptors, written once: the aquifer bottom and the resolved
    // closure per cell (0 closed form, 1 enslaved, 2 sigma) — the mask a
    // reader needs for Mesh2_face_gw_theta_sigma and _hu.
    if (snap.gw2d_bed_elev.size() == static_cast<std::size_t>(n_faces_)) {
        hid_t space = H5Screate_simple(1, &n_faces_, nullptr);
        hid_t ds = io::createHdf5Dataset(file_id_, "Mesh2_face_gw_bed_elev", H5T_NATIVE_DOUBLE,
                                         space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        checkpointWriteStatus(H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                                       snap.gw2d_bed_elev.data()));
        writeStringAttr(ds, "long_name",
                        "aquifer bottom elevation (cell bed minus soil column thickness)");
        writeStringAttr(ds, "units", "m");
        writeStringAttr(ds, "mesh", "Mesh2");
        writeStringAttr(ds, "location", "face");
        H5Dclose(ds);
        H5Sclose(space);
    }
    if (snap.gw2d_closure.size() == static_cast<std::size_t>(n_faces_)) {
        hid_t space = H5Screate_simple(1, &n_faces_, nullptr);
        hid_t ds = io::createHdf5Dataset(file_id_, "Mesh2_face_gw_closure", H5T_NATIVE_INT, space,
                                         H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        checkpointWriteStatus(
            H5Dwrite(ds, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, snap.gw2d_closure.data()));
        writeStringAttr(ds, "long_name", "resolved unsaturated-zone closure per cell");
        writeStringAttr(ds, "flag_values", "0 1 2");
        writeStringAttr(ds, "flag_meanings", "closed_form enslaved sigma");
        writeStringAttr(ds, "mesh", "Mesh2");
        writeStringAttr(ds, "location", "face");
        H5Dclose(ds);
        H5Sclose(space);
    }
    // T7.5: the transported tuple. `[time, species, face]` concentrations
    // for each zone, and a per-species ledger — the species twin of the
    // water's `groundwater_ledger`, written to the same cadence.
    n_gw_species_ = static_cast<hsize_t>(std::max(0, snap.gw2d_species_count));
    if (n_gw_species_ > 0) {
        hsize_t zero3[3] = {0, n_gw_species_, n_faces_};
        hsize_t chunk3[3] = {1, n_gw_species_, std::min<hsize_t>(n_faces_, 4096)};
        struct S {
            const char* name;
            const char* what;
            hid_t* ds;
        };
        const S kZones[2] = {
            {"Mesh2_face_gw_sat_conc", "saturated zone", &ds_gw_sat_conc_},
            {"Mesh2_face_gw_unsat_conc", "unsaturated column", &ds_gw_unsat_conc_},
        };
        for (const auto& z : kZones) {
            *z.ds = createUnlimitedDataset(z.name, 3, zero3, chunk3);
            const std::string ln =
                std::string("groundwater species concentration in the ") + z.what;
            writeStringAttr(*z.ds, "long_name", ln.c_str());
            writeStringAttr(
                *z.ds, "concentration_basis",
                snap.gw2d_transport_layers > 0
                    ? "dissolved equivalent: total inventory / (aqueous plus equilibrium-sorbed capacity)"
                    : "legacy BULK: total inventory / water volume");
            writeStringAttr(*z.ds, "units", "1"); // per species; see species_units
            writeStringAttr(*z.ds, "mesh", "Mesh2");
            writeStringAttr(*z.ds, "location", "face");
            writeStringAttr(*z.ds, "layout",
                            "[time, species, face]; species order = the "
                            "species_names list (pollutants, MSX, "
                            "__WATER_AGE__, __TEMPERATURE__); a zone with no "
                            "water reports 0");
            if (snap.gw2d_species_names) {
                std::string names;
                for (hsize_t k = 0; k < n_gw_species_ && k < snap.gw2d_species_names->size(); ++k) {
                    if (k)
                        names += ",";
                    names += (*snap.gw2d_species_names)[static_cast<std::size_t>(k)];
                }
                if (!names.empty())
                    writeStringAttr(*z.ds, "species_names", names.c_str());
                std::string units;
                for (hsize_t k = 0; k < n_gw_species_; ++k) {
                    if (k)
                        units += ",";
                    if (k < snap.gw2d_species_names->size()) {
                        const auto unit = native_species_units_.find(
                            (*snap.gw2d_species_names)[static_cast<std::size_t>(k)]);
                        if (unit != native_species_units_.end())
                            units += unit->second;
                    }
                }
                // Empty positions deliberately remain unresolved for readers.
                writeStringAttr(*z.ds, "species_units", units.c_str());
            }
        }
        hsize_t zerol[3] = {0, n_gw_species_, kGwSpeciesLedgerTerms};
        hsize_t chunkl[3] = {64, n_gw_species_, kGwSpeciesLedgerTerms};
        ds_gw_species_ledger_ = createUnlimitedDataset("groundwater_species_ledger", 3, zerol,
                                                       chunkl, H5T_NATIVE_DOUBLE);
        writeStringAttr(ds_gw_species_ledger_, "long_name",
                        "two-zone aquifer species ledger, cumulative");
        writeStringAttr(ds_gw_species_ledger_, "units", "1");
        writeStringAttr(ds_gw_species_ledger_, "terms",
                        "init,storage,infil_in,node_in,link_in,lateral_net,"
                        "deep_out,node_out,link_out,dunne_out,et_out,"
                        "reaction_out,residual,source_in,source_out");
        writeStringAttr(ds_gw_species_ledger_, "layout",
                        "[time, species, term]; mass units are the species' "
                        "own (concentration x m3); residual = storage + out "
                        "- in - init and is zero for a conserving kernel");
    }

    // Domain ledger series [nTime, 12], cumulative m³ — the SWMM_GW2D_LED_*
    // order plus the continuity residual last, so a plot of the residual
    // over time is one column read. (G-X3 appended `link` at LED index 10;
    // the residual moved to 11 — read the `terms` attribute, not a constant.)
    {
        hsize_t zero[2] = {0, kGwLedgerTerms}, chunk[2] = {64, kGwLedgerTerms};
        ds_gw_ledger_ =
            createUnlimitedDataset("groundwater_ledger", 2, zero, chunk, H5T_NATIVE_DOUBLE);
        writeStringAttr(ds_gw_ledger_, "long_name", "two-zone aquifer domain ledger, cumulative");
        writeStringAttr(ds_gw_ledger_, "units", "m3");
        writeStringAttr(
            ds_gw_ledger_, "terms",
            "recharge,lateral,deep,node,dunne,caprise,et,infil_in,"
            "init_storage,storage,link,continuity_residual,source_in,source_out,reject");
        writeStringAttr(ds_gw_ledger_, "layout",
                        "[time, term]; storage includes water in flight in the side "
                        "accumulators; continuity_residual = ledgered storage − init − (in − out)");
    }
    // Per-bed cumulative node exchange [nTime, nBeds], + out of the aquifer.
    n_gw_beds_ = static_cast<hsize_t>(snap.gw2d_bed_exchange_cum.size());
    if (n_gw_beds_ > 0) {
        hsize_t zero[2] = {0, n_gw_beds_}, chunk[2] = {64, n_gw_beds_};
        ds_gw_bed_exchange_ = createUnlimitedDataset("groundwater_node_exchange_cum", 2, zero,
                                                     chunk, H5T_NATIVE_DOUBLE);
        writeStringAttr(
            ds_gw_bed_exchange_, "long_name",
            "cumulative aquifer <-> 1D node exchange per [2D_AQUIFER_NODE] bed, positive out of the aquifer");
        writeStringAttr(ds_gw_bed_exchange_, "units", "m3");
        if (snap.gw2d_node_names) {
            std::string names;
            for (hsize_t k = 0; k < n_gw_beds_ && k < snap.gw2d_node_names->size(); ++k) {
                if (k)
                    names += ",";
                names += (*snap.gw2d_node_names)[static_cast<std::size_t>(k)];
            }
            if (!names.empty())
                writeStringAttr(ds_gw_bed_exchange_, "node_names", names.c_str());
        }
    }
    n_gw_transport_layers_ = snap.gw2d_transport_layers;
    n_gw_boundaries_ = snap.gw2d_boundary_count;
    if (n_gw_transport_layers_ > 0 && n_gw_species_ > 0) {
        hsize_t zero[4] = {0, n_gw_species_, n_gw_transport_layers_, n_faces_};
        hsize_t chunk[4] = {1, 1, n_gw_transport_layers_, std::min<hsize_t>(n_faces_, 4096)};
        ds_gw_layer_mass_ = createUnlimitedDataset("Mesh2_face_gw_layer_mass", 4, zero, chunk);
        ds_gw_layer_conc_ = createUnlimitedDataset("Mesh2_face_gw_layer_conc", 4, zero, chunk);
        for (hid_t id : {ds_gw_layer_mass_, ds_gw_layer_conc_}) {
            writeStringAttr(id, "mesh", "Mesh2");
            writeStringAttr(id, "location", "face");
            writeStringAttr(id, "layout",
                            "[time,species,layer,face]; zero-based SIGMA layer, surface downward");
            if (snap.gw2d_species_names) {
                std::string names, units;
                for (hsize_t k = 0; k < n_gw_species_; ++k) {
                    if (k) {
                        names += ",";
                        units += ",";
                    }
                    const auto& name = (*snap.gw2d_species_names)[static_cast<std::size_t>(k)];
                    names += name;
                    const auto unit = native_species_units_.find(name);
                    if (unit != native_species_units_.end()) {
                        units += unit->second;
                        if (id == ds_gw_layer_mass_)
                            units += " x m3";
                    }
                }
                writeStringAttr(id, "species_names", names.c_str());
                writeStringAttr(id, "species_units", units.c_str());
            }
        }
        writeStringAttr(ds_gw_layer_mass_, "quantity",
                        "aqueous plus sorbed inventory; dry retained mass remains visible");
        writeStringAttr(ds_gw_layer_mass_, "units", "native concentration x m3");
        writeStringAttr(ds_gw_layer_conc_, "quantity",
                        "dissolved concentration; zero capacity reports zero");
        writeStringAttr(ds_gw_layer_conc_, "units", "native concentration");
    }
    if (n_gw_boundaries_ > 0) {
        hsize_t zero[3] = {0, n_gw_boundaries_, 4}, chunk[3] = {1, n_gw_boundaries_, 4};
        ds_gw_boundary_water_ =
            createUnlimitedDataset("groundwater_boundary_water", 3, zero, chunk);
        writeStringAttr(ds_gw_boundary_water_, "terms",
                        "requested_in,requested_out,accepted_in,accepted_out");
        writeStringAttr(ds_gw_boundary_water_, "layout",
                        "[time,boundary,term]; gross cumulative m3");
        std::string edges;
        for (std::size_t i = 0; i < snap.gw2d_boundary_cell_edge.size(); i += 2) {
            if (i)
                edges += ",";
            edges += std::to_string(snap.gw2d_boundary_cell_edge[i]) + ":" +
                     std::to_string(snap.gw2d_boundary_cell_edge[i + 1]);
        }
        writeStringAttr(ds_gw_boundary_water_, "cell_local_edge", edges.c_str());
        if (n_gw_species_ > 0) {
            hsize_t zero4[4] = {0, n_gw_species_, n_gw_boundaries_, 4},
                    chunk4[4] = {1, 1, n_gw_boundaries_, 4};
            ds_gw_boundary_species_ =
                createUnlimitedDataset("groundwater_boundary_species", 4, zero4, chunk4);
            writeStringAttr(ds_gw_boundary_species_, "terms",
                            "advective_in,advective_out,imposed_in,imposed_out");
            writeStringAttr(
                ds_gw_boundary_species_, "layout",
                "[time,species,boundary,term]; gross cumulative native concentration x m3");
        }
    }
    if (!snap.gw2d_transport_ledger.empty() && n_gw_species_ > 0) {
        hsize_t zero[3] = {0, n_gw_species_, 20}, chunk[3] = {64, n_gw_species_, 20};
        ds_gw_transport_ledger_ =
            createUnlimitedDataset("groundwater_transport_ledger", 3, zero, chunk);
        writeStringAttr(
            ds_gw_transport_ledger_, "terms",
            "init,storage,infil_in,node_in,link_in,lateral_net,deep_out,node_out,link_out,dunne_out,et_out,reaction_out,residual,source_in,source_out,boundary_in,boundary_out,boundary_flux_in,boundary_flux_out,age_generated");
        writeStringAttr(
            ds_gw_transport_ledger_, "layout",
            "[time,species,term]; append-only SWMM_GW2D_SPL order; native concentration x m3");
    }
    // GW_DETAILED: the σ columns — m × nFace per step, the one large variable.
    n_gw_layers_ = static_cast<hsize_t>(std::max(0, snap.gw2d_m_layers));
    if (want(report2d::GW_DETAILED) && n_gw_layers_ > 0 &&
        snap.gw2d_theta_sigma.size() == static_cast<std::size_t>(n_gw_layers_ * n_faces_)) {
        hsize_t zero3[3] = {0, n_gw_layers_, n_faces_};
        hsize_t chunk3[3] = {1, n_gw_layers_, std::min<hsize_t>(n_faces_, 4096)};
        ds_gw_theta_ = createUnlimitedDataset("Mesh2_face_gw_theta_sigma", 3, zero3, chunk3);
        writeStringAttr(ds_gw_theta_, "long_name", "closure B water content per sigma layer");
        writeStringAttr(ds_gw_theta_, "units", "1");
        writeStringAttr(ds_gw_theta_, "mesh", "Mesh2");
        writeStringAttr(ds_gw_theta_, "location", "face");
        writeStringAttr(ds_gw_theta_, "layout",
                        "[time, layer, face]; layer 0 at the ground surface; a cell whose "
                        "Mesh2_face_gw_closure is not 2 carries 0 in every layer");
    }
}

void Default2DOutputPlugin::writeSpeciesRows(const double* all, hsize_t n_species_in) {
    // Fast path: every row selected in order — write the block directly.
    bool identity = (n_species_ == n_species_in);
    if (identity)
        for (std::size_t k = 0; k < species_rows_.size(); ++k)
            if (species_rows_[k] != static_cast<int>(k)) {
                identity = false;
                break;
            }
    if (identity) {
        extendAndWrite3D(ds_face_species_conc_, all, n_species_, n_faces_);
        return;
    }
    // Gather the selected rows into a compact [n_sel × n_faces] block.
    std::vector<double> sel(static_cast<std::size_t>(n_species_ * n_faces_));
    for (std::size_t k = 0; k < species_rows_.size(); ++k) {
        const auto r = static_cast<std::size_t>(species_rows_[k]);
        if (r >= static_cast<std::size_t>(n_species_in))
            continue;
        std::copy(all + r * n_faces_, all + (r + 1) * n_faces_,
                  sel.begin() + static_cast<std::ptrdiff_t>(k * n_faces_));
    }
    extendAndWrite3D(ds_face_species_conc_, sel.data(), n_species_, n_faces_);
}

// ============================================================================
// finalize()
// ============================================================================

int Default2DOutputPlugin::finalize(const SimulationContext& ctx) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    const bool prior_error = state_ == PluginState::ERROR;
    const int source_error = writePendingSourceHistory() != 0 || prior_error ? 1 : 0;
    // Write the global 2D mass balance once, now that all terms are final.
    if (file_id_ != H5I_INVALID_HID && ctx.mass_balance_2d.active) {
        const auto& mb = ctx.mass_balance_2d;
        hid_t grp = io::createHdf5Group(file_id_, "/mass_balance_2d", H5P_DEFAULT, H5P_DEFAULT,
                                        H5P_DEFAULT);
        checkpointWriteStatus(grp >= 0 ? 0 : -1);
        if (grp >= 0) {
            auto writeScalar = [this](hid_t loc, const char* name, double v) {
                hid_t space = H5Screate(H5S_SCALAR);
                hid_t ds = io::createHdf5Dataset(loc, name, H5T_NATIVE_DOUBLE, space, H5P_DEFAULT,
                                                 H5P_DEFAULT, H5P_DEFAULT);
                checkpointWriteStatus(
                    H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, &v));
                writeStringAttr(ds, "units", "m3");
                H5Dclose(ds);
                H5Sclose(space);
            };
            writeScalar(grp, "init_storage", mb.init_storage);
            writeScalar(grp, "final_storage", mb.final_storage);
            writeScalar(grp, "rainfall_in", mb.rainfall_in);
            writeScalar(grp, "coupling_1d_to_2d_in", mb.coupling_1d_to_2d_in);
            writeScalar(grp, "coupling_2d_to_1d_out", mb.coupling_2d_to_1d_out);
            writeScalar(grp, "outfall_in", mb.outfall_in);
            writeScalar(grp, "outfall_out", mb.outfall_out);
            writeScalar(grp, "boundary_in", mb.boundary_in);
            writeScalar(grp, "boundary_out", mb.boundary_out);
            // Both loss channels (plan §5.5.4): evap_out was previously
            // omitted here, so writing infil_out alone would leave the
            // sidecar's budget silently non-closing.
            writeScalar(grp, "evap_out", mb.evap_out);
            writeScalar(grp, "infil_out", mb.infil_out);
            writeDoubleAttr(grp, "continuity_error", mb.error());
            H5Gclose(grp);
        }
    }
    // G-O: the aquifer's own ledger, once, beside the surface's — the same
    // terms the .rpt "2D Aquifer Continuity" block prints.
    if (file_id_ != H5I_INVALID_HID && ctx.twod_io.aquifer_state &&
        ctx.twod_io.aquifer_state->active) {
        const auto& g = *ctx.twod_io.aquifer_state;
        hid_t grp =
            io::createHdf5Group(file_id_, "/groundwater_2d", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        checkpointWriteStatus(grp >= 0 ? 0 : -1);
        if (grp >= 0) {
            auto writeScalar = [this](hid_t loc, const char* name, double v) {
                hid_t space = H5Screate(H5S_SCALAR);
                hid_t ds = io::createHdf5Dataset(loc, name, H5T_NATIVE_DOUBLE, space, H5P_DEFAULT,
                                                 H5P_DEFAULT, H5P_DEFAULT);
                checkpointWriteStatus(
                    H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, &v));
                writeStringAttr(ds, "units", "m3");
                H5Dclose(ds);
                H5Sclose(space);
            };
            writeScalar(grp, "init_storage", g.led_init_storage);
            writeScalar(grp, "final_storage", g.liveStorage());
            writeScalar(grp, "infil_in", g.led_infil_in);
            writeScalar(grp, "source_in", g.led_source_in);
            writeScalar(grp, "source_out", g.led_source_out);
            writeScalar(grp, "link_in", g.led_link); // G-X3: conduit seepage
            writeScalar(grp, "lateral_in", g.led_lateral);
            writeScalar(grp, "recharge", g.led_recharge);
            writeScalar(grp, "capillary_rise", g.led_caprise);
            writeScalar(grp, "deep_out", g.led_deep);
            writeScalar(grp, "node_out", g.led_node);
            writeScalar(grp, "et_out", g.led_et);
            writeScalar(grp, "dunne_out", g.led_dunne);
            writeDoubleAttr(grp, "continuity_error", g.continuityResidual());
            H5Gclose(grp);
        }
    }

    closeDatasets();

    if (file_id_ != H5I_INVALID_HID) {
        checkpointWriteStatus(H5Fclose(file_id_));
        file_id_ = H5I_INVALID_HID;
    }

    state_ = source_error || checkpoint_write_failed_ ? PluginState::ERROR : PluginState::FINALIZED;
    return source_error || checkpoint_write_failed_ ? 1 : 0;
}

/**
 * @brief Record a negative HDF5 status only while checkpoint-fresh output tracking is enabled.
 * @param[in] status HDF5 herr_t return value; nonnegative is ignored.
 * @note On tracked failure sets checkpoint_write_failed_, ERROR and a diagnostic. Ordinary
 * output mode ignores this status. Does not throw explicitly, close handles or undo prior
 * writes; assigning error text can allocate. Caller holds serialized writer access.
 */
void Default2DOutputPlugin::checkpointWriteStatus(herr_t status) {
    if (checkpoint_fresh_ && status < 0) {
        checkpoint_write_failed_ = true;
        state_ = PluginState::ERROR;
        last_error_ = "Mesh checkpoint output write failed.";
    }
}

/**
 * @brief Enumerate addresses of all owned scalar/array dataset-handle slots for checkpoint work.
 * @return Owned vector of borrowed hid_t pointers, including invalid optional slots, in fixed
 * owner order plus groundwater/snow arrays. Addresses remain valid only while this object and
 * arrays remain stable; do not close or mutate them concurrently. Does not duplicate HDF5
 * handles or transfer ownership.
 * @note Allocates and can throw. Caller holds global result mutex and serializes
 * checkpoint/lifecycle.
 */
std::vector<hid_t*> Default2DOutputPlugin::checkpointDatasets() {
    std::vector<hid_t*> ids{&ds_gw_layer_mass_,
                            &ds_gw_layer_conc_,
                            &ds_gw_boundary_water_,
                            &ds_gw_boundary_species_,
                            &ds_gw_transport_ledger_,
                            &ds_time_,
                            &ds_face_depth_,
                            &ds_face_head_,
                            &ds_face_grad_hx_,
                            &ds_face_grad_hy_,
                            &ds_face_grad_hx_lim_,
                            &ds_face_grad_hy_lim_,
                            &ds_face_rainfall_,
                            &ds_face_coupling_flux_,
                            &ds_face_net_source_,
                            &ds_face_infil_rate_,
                            &ds_face_infil_cum_,
                            &ds_face_rain_cum_,
                            &ds_face_species_conc_,
                            &ds_face_buildup_,
                            &ds_gw_sat_conc_,
                            &ds_gw_unsat_conc_,
                            &ds_gw_species_ledger_,
                            &ds_gw_ledger_,
                            &ds_gw_bed_exchange_,
                            &ds_gw_theta_,
                            &ds_heat_water_temp_,
                            &ds_heat_bed_temp_,
                            &ds_heat_bed_energy_,
                            &ds_heat_bed_initial_energy_,
                            &ds_heat_bed_capacity_,
                            &ds_heat_bed_pore_volume_,
                            &ds_heat_bed_species_conc_,
                            &ds_heat_bed_species_initial_amount_,
                            &ds_heat_bed_species_exchange_,
                            &ds_heat_bed_age_generation_,
                            &ds_heat_surface_budget_,
                            &ds_heat_evaporation_,
                            &ds_heat_ground_temp_,
                            &ds_heat_ground_energy_,
                            &ds_heat_ground_capacity_,
                            &ds_heat_layer_temp_,
                            &ds_heat_layer_energy_,
                            &ds_heat_layer_capacity_,
                            &ds_heat_ground_budget_,
                            &ds_heat_lid_temp_,
                            &ds_heat_lid_energy_,
                            &ds_heat_lid_capacity_,
                            &ds_heat_lid_ground_contact_,
                            &ds_face_eddy_viscosity_,
                            &ds_face_strain_rate_,
                            &ds_face_vx_,
                            &ds_face_vy_,
                            &ds_face_continuity_err_,
                            &ds_edge_flux_,
                            &ds_node_head_,
                            &ds_node_depth_,
                            &ds_face_max_depth_,
                            &ds_face_max_velocity_,
                            &ds_face_max_continuity_err_};
    for (auto& id : ds_gw_face_)
        ids.push_back(&id);
    for (auto& id : ds_snow_)
        ids.push_back(&id);
    return ids;
}
/**
 * @brief Close every retained result dataset handle and mark its slot invalid.
 * @note Does not close file_id_, flush histories, reset counts/created flags or report HDF5
 * close failures. Caller holds global result mutex and stops concurrent I/O; destructor/finalize
 * provide it. No allocation or file rollback occurs; the object cannot resume updates without
 * complete reinitialization.
 */
void Default2DOutputPlugin::closeDatasets() noexcept {
    // Close all datasets
    auto closeDS = [](hid_t& ds) {
        if (ds != H5I_INVALID_HID) {
            H5Dclose(ds);
            ds = H5I_INVALID_HID;
        }
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
    closeDS(ds_face_infil_rate_);
    closeDS(ds_face_infil_cum_);
    closeDS(ds_face_rain_cum_);
    for (auto& id : ds_snow_)
        closeDS(id);
    closeDS(ds_face_species_conc_);
    closeDS(ds_face_buildup_);
    for (int k = 0; k < kGwFaceFields; ++k)
        closeDS(ds_gw_face_[k]); // G-O
    closeDS(ds_gw_sat_conc_);
    closeDS(ds_gw_unsat_conc_); // T7.5
    closeDS(ds_gw_species_ledger_);
    closeDS(ds_gw_ledger_);
    closeDS(ds_gw_bed_exchange_);
    closeDS(ds_gw_theta_);
    closeDS(ds_gw_layer_mass_);
    closeDS(ds_gw_layer_conc_);
    closeDS(ds_gw_boundary_water_);
    closeDS(ds_gw_boundary_species_);
    closeDS(ds_gw_transport_ledger_);
    closeDS(ds_heat_water_temp_);
    closeDS(ds_heat_bed_temp_);
    closeDS(ds_heat_bed_energy_);
    closeDS(ds_heat_bed_initial_energy_);
    closeDS(ds_heat_bed_capacity_);
    closeDS(ds_heat_bed_pore_volume_);
    closeDS(ds_heat_bed_species_conc_);
    closeDS(ds_heat_bed_species_initial_amount_);
    closeDS(ds_heat_bed_species_exchange_);
    closeDS(ds_heat_bed_age_generation_);
    closeDS(ds_heat_surface_budget_);
    closeDS(ds_heat_evaporation_);
    closeDS(ds_heat_ground_temp_);
    closeDS(ds_heat_ground_energy_);
    closeDS(ds_heat_ground_capacity_);
    closeDS(ds_heat_layer_temp_);
    closeDS(ds_heat_layer_energy_);
    closeDS(ds_heat_layer_capacity_);
    closeDS(ds_heat_ground_budget_);
    closeDS(ds_heat_lid_temp_);
    closeDS(ds_heat_lid_energy_);
    closeDS(ds_heat_lid_capacity_);
    closeDS(ds_heat_lid_ground_contact_);
    closeDS(ds_face_eddy_viscosity_);
    closeDS(ds_face_strain_rate_);
    mixing_diagnostics_.reset();
    mixing_edges_.reset();
    mixing_mesh_ = nullptr;
    closeDS(ds_face_vx_);
    closeDS(ds_face_vy_);
    closeDS(ds_face_continuity_err_);
    closeDS(ds_edge_flux_);
    closeDS(ds_node_head_);
    closeDS(ds_node_depth_);
    closeDS(ds_face_max_depth_);
    closeDS(ds_face_max_velocity_);
    closeDS(ds_face_max_continuity_err_);
}

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
