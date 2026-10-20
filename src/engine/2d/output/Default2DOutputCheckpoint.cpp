// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#ifdef OPENSWMM_HAS_2D
#include "Default2DOutputPlugin.hpp"
#include "../subsurface/SourceResultHistory.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include "../../core/DateTime.hpp"
#include "../../core/FileIO.hpp"
#include "../../io/Hdf5Mutex.hpp"
#include "../../core/SimulationContext.hpp"
#include "../data/Report2DVars.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>

namespace openswmm::twoD {
namespace {
constexpr std::size_t maxBytes = 64 * 1024 * 1024;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Handle {
    hid_t id; herr_t (*close)(hid_t);
    ~Handle() { if (id >= 0) close(id); }
};
std::string readFile(const std::filesystem::path& path) {
    std::error_code ec; const auto size = std::filesystem::file_size(path, ec);
    require(!ec && size > 0 && size <= maxBytes, "Mesh result prefix is missing or exceeds its checkpoint limit.");
    std::ifstream in(path, std::ios::binary); std::string bytes(size, '\0');
    in.read(bytes.data(), bytes.size());
    require(bool(in) && in.peek() == std::char_traits<char>::eof(), "Cannot read mesh result prefix.");
    return bytes;
}
hid_t openImage(const std::string& bytes) {
    Handle props{H5Pcreate(H5P_FILE_ACCESS), H5Pclose};
    require(props.id >= 0 && H5Pset_fapl_core(props.id, 4096, false) >= 0 &&
            H5Pset_file_image(props.id, const_cast<char*>(bytes.data()), bytes.size()) >= 0,
            "Cannot prepare mesh schema image.");
    const auto file = H5Fopen("OpenSWMM mesh checkpoint schema", H5F_ACC_RDWR, props.id);
    require(file >= 0, "Cannot open mesh schema image."); return file;
}
std::set<std::string> links(hid_t file) {
    H5G_info_t info{}; require(H5Gget_info(file, &info) >= 0 && info.nlinks <= 256, "Invalid mesh result schema.");
    std::set<std::string> names;
    for (hsize_t k = 0; k < info.nlinks; ++k) {
        const auto len = H5Lget_name_by_idx(file, ".", H5_INDEX_NAME, H5_ITER_INC, k, nullptr, 0, H5P_DEFAULT);
        require(len > 0 && len < 256, "Invalid mesh result name.");
        std::string name(len + 1, '\0');
        require(H5Lget_name_by_idx(file, ".", H5_INDEX_NAME, H5_ITER_INC, k, name.data(), name.size(), H5P_DEFAULT) == len,
                "Cannot read mesh result name."); name.resize(len);
        H5L_info_t link{}; require(H5Lget_info(file, name.c_str(), &link, H5P_DEFAULT) >= 0 && link.type == H5L_TYPE_HARD,
                                  "Mesh result links must be local hard links.");
        names.insert(std::move(name));
    }
    return names;
}
std::vector<hsize_t> shape(hid_t dataset, bool maximum = false) {
    Handle space{H5Dget_space(dataset), H5Sclose}; const int rank = H5Sget_simple_extent_ndims(space.id);
    require(rank >= 0 && rank <= 4, "Invalid mesh dataset rank.");
    std::vector<hsize_t> dims(rank), max(rank);
    require(H5Sget_simple_extent_dims(space.id, dims.data(), max.data()) >= 0, "Cannot inspect mesh dimensions.");
    return maximum ? max : dims;
}
std::string raw(hid_t object, bool attribute) {
    Handle type{attribute ? H5Aget_type(object) : H5Dget_type(object), H5Tclose};
    Handle space{attribute ? H5Aget_space(object) : H5Dget_space(object), H5Sclose};
    const auto n = H5Sget_simple_extent_npoints(space.id); const auto width = H5Tget_size(type.id);
    const auto kind = H5Tget_class(type.id);
    if (!attribute && kind == H5T_STRING && H5Tis_variable_str(type.id) > 0) {
        require(n >= 0 && std::size_t(n) <= maxBytes / sizeof(char*),
                "Oversized mesh string result mapping.");
        std::vector<char*> values(static_cast<std::size_t>(n), nullptr);
        struct ReclaimStrings {
            hid_t type;
            hid_t space;
            void* values;
            ~ReclaimStrings() { H5Dvlen_reclaim(type, space, H5P_DEFAULT, values); }
        } reclaim{type.id, space.id, values.data()};
        require(H5Dread(object, type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, values.data()) >= 0,
                "Cannot read mesh string result mapping.");
        std::string result;
        for (const auto* value : values) {
            const std::size_t length = value ? std::strlen(value) : 0;
            const auto prefix = std::to_string(length);
            require(result.size() <= maxBytes && prefix.size() < maxBytes - result.size() &&
                    length < maxBytes - result.size() - prefix.size(),
                    "Oversized mesh string result value.");
            result += prefix; result.push_back(':');
            if (length) result.append(value, length);
            result.push_back('\0');
        }
        return result;
    }
    require(n >= 0 && width > 0 && std::size_t(n) <= maxBytes / width &&
            (kind == H5T_FLOAT || kind == H5T_INTEGER || (attribute && kind == H5T_STRING && !H5Tis_variable_str(type.id))),
            "Unsupported or oversized mesh result value.");
    std::string value(std::size_t(n) * width, '\0');
    require((attribute ? H5Aread(object, type.id, value.data()) :
            H5Dread(object, type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, value.data())) >= 0,
            "Cannot read mesh result value."); return value;
}
void attributes(hid_t expected, hid_t actual) {
    const int count = H5Aget_num_attrs(expected);
    require(count >= 0 && count == H5Aget_num_attrs(actual), "Mesh result attributes differ.");
    for (int k = 0; k < count; ++k) {
        Handle a{H5Aopen_by_idx(expected, ".", H5_INDEX_NAME, H5_ITER_INC, k, H5P_DEFAULT, H5P_DEFAULT), H5Aclose};
        const auto len = H5Aget_name(a.id, 0, nullptr); require(len > 0 && len < 256, "Invalid mesh attribute name.");
        std::string name(len + 1, '\0'); H5Aget_name(a.id, name.size(), name.data()); name.resize(len);
        Handle b{H5Aopen(actual, name.c_str(), H5P_DEFAULT), H5Aclose};
        Handle at{H5Aget_type(a.id), H5Tclose}, bt{b.id >= 0 ? H5Aget_type(b.id) : -1, H5Tclose};
        require(b.id >= 0 && H5Tequal(at.id, bt.id) > 0 && raw(a.id, true) == raw(b.id, true),
                "Mesh result attribute identity differs.");
    }
}
bool storage(hid_t a, hid_t b) {
    const auto layout=H5Pget_layout(a);
    if(layout!=H5Pget_layout(b) || layout==H5D_VIRTUAL || H5Pget_external_count(b)!=0)return false;
    if(layout==H5D_CHUNKED) {
        hsize_t ac[4]{},bc[4]{};const auto rank=H5Pget_chunk(a,4,ac);
        if(rank<1 || rank!=H5Pget_chunk(b,4,bc) || !std::equal(ac,ac+rank,bc))return false;
    }
    const auto count=H5Pget_nfilters(a);if(count<0 || count!=H5Pget_nfilters(b))return false;
    for(int k=0;k<count;++k) {
        unsigned af=0,bf=0,av[32]{},bv[32]{};size_t an=32,bn=32;
        const auto filter=H5Pget_filter2(a,k,&af,&an,av,0,nullptr,nullptr);
        if(filter<0 || filter!=H5Pget_filter2(b,k,&bf,&bn,bv,0,nullptr,nullptr) ||
           af!=bf || an>32 || an!=bn || !std::equal(av,av+an,bv))return false;
    }
    return true;
}

// Rectangular hyperslabs bound series reads even when one row is very wide.
void visitNumbers(hid_t dataset,const std::function<void(double)>& visit={},const std::vector<unsigned char>& missing={}) {
    auto dims=shape(dataset);if(std::any_of(dims.begin(),dims.end(),[](auto n){return n==0;}))return;
    std::vector<hsize_t> at(dims.size(),0),tile(dims.size(),1);hsize_t capacity=65536;
    for(int d=int(dims.size())-1;d>=0;--d){tile[d]=std::min(dims[d],capacity);capacity=std::max<hsize_t>(1,capacity/tile[d]);}
    // HDF5 also allocates workspace per selected chunk. Bound the reporting
    // axis as well as numeric values when histories use one chunk per report.
    const auto maximum=shape(dataset,true);
    const bool series=!maximum.empty()&&maximum[0]==H5S_UNLIMITED;
    if(series)tile[0]=std::min<hsize_t>(tile[0],256);
    Handle space{H5Dget_space(dataset),H5Sclose};bool done=false;
    while(!done){auto count=tile;hsize_t n=1;for(std::size_t d=0;d<dims.size();++d){count[d]=std::min(tile[d],dims[d]-at[d]);n*=count[d];}
        if(!dims.empty())require(H5Sselect_hyperslab(space.id,H5S_SELECT_SET,at.data(),nullptr,count.data(),nullptr)>=0,"Cannot select mesh checkpoint block.");
        Handle memory{H5Screate_simple(1,&n,nullptr),H5Sclose};std::vector<double> values(n);
        require(H5Dread(dataset,H5T_NATIVE_DOUBLE,memory.id,space.id,H5P_DEFAULT,values.data())>=0,"Cannot read mesh checkpoint block.");
        for(std::size_t j=0;j<values.size();++j) {
            const double v=values[j];
            std::size_t remainder=j,cell=0;
            std::vector<hsize_t> coordinate(dims.size());
            for(std::size_t d=dims.size();d-->0;){coordinate[d]=at[d]+remainder%count[d];remainder/=count[d];}
            const std::size_t begin=series?1:0;
            for(std::size_t d=begin;d<dims.size();++d)cell=cell*static_cast<std::size_t>(dims[d])+static_cast<std::size_t>(coordinate[d]);
            const bool unavailable=cell<missing.size()&&missing[cell];
            require(std::isfinite(v)||(unavailable&&std::isnan(v)),"Nonfinite mesh checkpoint result.");if(visit)visit(v);
        }
        if(dims.empty())break;
        for(int d=int(dims.size())-1;d>=0;--d){at[d]+=tile[d];if(at[d]<dims[d])break;at[d]=0;if(d==0)done=true;}
    }
}
std::string datasetName(hid_t id) {
    const auto len = H5Iget_name(id, nullptr, 0); require(len > 0 && len < 256, "Invalid mesh dataset handle.");
    std::string name(len + 1, '\0'); H5Iget_name(id, name.data(), name.size()); name.resize(len); return name;
}
void localHistory(hid_t group, int depth=0) {
    require(depth<16,"Mesh source result group nesting exceeds its limit.");
    for(const auto& name:links(group)) {
        Handle object{H5Oopen(group,name.c_str(),H5P_DEFAULT),H5Oclose};
        require(object.id>=0,"Cannot inspect mesh source result object.");
        if(H5Iget_type(object.id)==H5I_GROUP)localHistory(object.id,depth+1);
        else {
            require(H5Iget_type(object.id)==H5I_DATASET,"Unsupported mesh source result object.");
            Handle props{H5Dget_create_plist(object.id),H5Pclose};
            require(H5Pget_external_count(props.id)==0 && H5Pget_layout(props.id)!=H5D_VIRTUAL,
                    "Mesh source results must be stored in their checkpoint file.");
        }
    }
}
}

struct Default2DOutputPlugin::PreparedCheckpoint {
    std::filesystem::path staging;
    hid_t file = H5I_INVALID_HID;
    std::vector<std::pair<hid_t*, hid_t>> datasets;
    hsize_t steps = 0, beds = 0, layers = 0;
    double next_due = -1;
    bool groundwater = false;
    bool heat = false;
    hsize_t heat_layers = 0, heat_lid_layers = 0;
    std::shared_ptr<const SourceResultHistory> history;
    std::shared_ptr<io::OwnedCheckpointFile> file_copy;
    ~PreparedCheckpoint() {
        std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
        for (auto& [target, id] : datasets) if (id >= 0) H5Dclose(id);
        if (file >= 0) H5Fclose(file);
        if (!file_copy && !staging.empty()) { std::error_code ec; std::filesystem::remove(staging, ec); }
    }
};

std::string Default2DOutputPlugin::fileBytes() const {
    require(!checkpoint_write_failed_ && file_id_ >= 0 && H5Fflush(file_id_, H5F_SCOPE_GLOBAL) >= 0, "Cannot flush mesh results.");
    return readFile(io::utf8_path(h5_path_));
}

void Default2DOutputPlugin::validateCheckpoint(hid_t file, const SimulationContext& ctx,
    double next_report_ms, const SimulationSnapshot& snap, hsize_t steps, double next_due, bool groundwater) const {
    require(checkpoint_fresh_ && !checkpoint_header_.empty() && n_species_ == 0 && n_buildup_ == 0 && n_gw_species_ == 0,
            "Mesh checkpoint supports its prepared water-only writer.");
    Default2DOutputPlugin reference(""); reference.file_id_ = openImage(checkpoint_header_);
    reference.n_faces_ = n_faces_; reference.report_vars_ = report_vars_;
    reference.storage_type_ = storage_type_; reference.compression_ = compression_;
    require(groundwater == (steps > 0 && want(report2d::GROUNDWATER) && snap.gw2d_active),
            "Mesh checkpoint groundwater presence differs.");
    if (groundwater) reference.createGroundwaterDatasets(snap);
    if (steps>0) reference.createSnowDatasets(snap);
    if (steps > 0 && reference.want(report2d::HEAT) && snap.thermal2d_active)
        reference.createThermalDatasets(snap);
    auto expected = links(reference.file_id_), actual = links(file);
    actual.erase("source_history"); require(actual == expected, "Mesh checkpoint dataset selection differs.");
    attributes(reference.file_id_, file);
    for (const auto& name : expected) {
        Handle a{H5Dopen2(reference.file_id_, name.c_str(), H5P_DEFAULT), H5Dclose};
        Handle b{H5Dopen2(file, name.c_str(), H5P_DEFAULT), H5Dclose};
        require(a.id >= 0 && b.id >= 0, "Mesh checkpoint object is not a dataset.");
        Handle at{H5Dget_type(a.id), H5Tclose}, bt{H5Dget_type(b.id), H5Tclose};
        Handle ap{H5Dget_create_plist(a.id), H5Pclose}, bp{H5Dget_create_plist(b.id), H5Pclose};
        require(H5Tequal(at.id, bt.id) > 0 && storage(ap.id,bp.id),
                ("Mesh checkpoint dataset type or storage differs: "+name).c_str());
        attributes(a.id, b.id);
        auto dims = shape(a.id), maximum = shape(a.id, true);
        const bool series = !maximum.empty() && maximum[0] == H5S_UNLIMITED;
        if (series) dims[0] = steps;
        require(dims == shape(b.id) && maximum == shape(b.id, true), "Mesh checkpoint dataset shape differs.");
        std::vector<unsigned char> missing;
        for(std::size_t field=0;field<meshSnowFields.size();++field)if(name==meshSnowFields[field].name) {
            require(dims.size()==2&&dims[1]==snap.surface_snow[0].size(),"Mesh snow checkpoint identity differs.");
            missing.resize(dims[1]);
            for(std::size_t cell=0;cell<missing.size();++cell) {
                const int model=int(snap.surface_snow[0][cell]);
                missing[cell]=field!=0&&(model<0||field==10||
                    (model==0&&(field==7||field==8||field==9||field==11||field>=19)));
            }
        }
        const auto markMissing = [&](const std::vector<double>& values) {
            if (values.empty()) return;
            missing.resize(values.size());
            for (std::size_t i = 0; i < values.size(); ++i) missing[i] = std::isnan(values[i]);
        };
        if (name == "Mesh2_face_water_temperature") markMissing(snap.thermal2d_water_temperature);
        else if (name == "Mesh2_face_bed_temperature") markMissing(snap.thermal2d_bed_temperature);
        else if (name == "Mesh2_face_bed_initial_energy") markMissing(snap.thermal2d_bed_initial_energy);
        else if (name == "Mesh2_face_bed_porewater_volume") markMissing(snap.thermal2d_bed_pore_volume);
        else if (name == "Mesh2_face_bed_age_generation") markMissing(snap.thermal2d_bed_age_generation);
        else if (name == "Mesh2_face_bed_species_conc") markMissing(snap.thermal2d_bed_species_concentration);
        else if (name == "Mesh2_face_bed_species_initial_amount") markMissing(snap.thermal2d_bed_species_initial_amount);
        else if (name == "Mesh2_face_bed_species_exchange") markMissing(snap.thermal2d_bed_species_exchange);
        else if (name == "Mesh2_face_soil_temperature") markMissing(snap.thermal2d_ground_temperature);
        else if (name == "Mesh2_face_soil_energy") markMissing(snap.thermal2d_ground_energy);
        else if (name == "Mesh2_face_soil_heat_capacity") markMissing(snap.thermal2d_ground_capacity);
        else if (name == "Mesh2_face_soil_layer_center_depth") markMissing(snap.thermal2d_ground_layer_center_m);
        else if (name == "Mesh2_face_soil_layer_temperature") markMissing(snap.thermal2d_ground_layer_temperature);
        else if (name == "Mesh2_face_soil_layer_energy") markMissing(snap.thermal2d_ground_layer_energy);
        else if (name == "Mesh2_face_soil_layer_heat_capacity") markMissing(snap.thermal2d_ground_layer_capacity);
        else if (name == "ground_heat_budget") markMissing(snap.thermal2d_ground_budget);
        else if (name == "Mesh2_lid_layer_temperature") markMissing(snap.thermal2d_lid_layer_temperature);
        if (H5Tget_class(bt.id) == H5T_FLOAT || H5Tget_class(bt.id) == H5T_INTEGER)
            visitNumbers(b.id,{},missing);
        else
            require(H5Tget_class(bt.id) == H5T_STRING && !series,
                    "Unsupported mesh checkpoint dataset type.");
        if (!series && name.find("Mesh2_face_max_") != 0)
            require(raw(a.id, false) == raw(b.id, false), "Mesh checkpoint static geometry or aquifer identity differs.");
    }
    const double step_ms = 1000 * ctx.options.report_step, grid = std::round(next_report_ms / step_ms);
    require(std::isfinite(grid) && grid >= 1 && grid <= std::numeric_limits<int>::max() && grid * step_ms == next_report_ms,
            "Mesh checkpoint hydraulic reporting clock differs.");
    const int completed = static_cast<int>(grid) - 1;
    const auto date = [&](int k) { return datetime::addSeconds(ctx.options.start_date, (k * step_ms + 1) / 1000); };
    int first = 1, upper = completed + 1;
    while (first < upper) { const int mid = first + (upper - first) / 2;
        if (date(mid) < ctx.options.report_start) first = mid + 1; else upper = mid; }
    const double ratio = report_2d_step_days_ > 0 ? report_2d_step_days_ * 86400 / ctx.options.report_step : 1;
    require(std::isfinite(ratio) && ratio >= 1 && ratio <= std::numeric_limits<int>::max(), "Invalid mesh reporting interval.");
    const int stride = static_cast<int>(std::round(ratio));
    require(steps == (first > completed ? 0 : 1 + (completed - first) / stride), "Mesh report count differs.");
    Handle times{H5Dopen2(file, "time", H5P_DEFAULT), H5Dclose};double due=-1;std::size_t k=0;
    visitNumbers(times.id,[&](double value){
        require(value==date(first+static_cast<int>(k)*stride),"Mesh checkpoint report date differs.");++k;
        if(report_2d_step_days_>0){if(due<0)due=value;while(due<=value+1e-9)due+=report_2d_step_days_;}
    });
    require(std::isfinite(next_due) && next_due == due, "Mesh checkpoint cadence cursor differs.");
}

std::string Default2DOutputPlugin::checkpointBytes(const SimulationContext& ctx, double next_report_ms,
                                                  const SimulationSnapshot& snap) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    require(writePendingSourceHistory() == 0 && state_ != PluginState::ERROR, "Cannot checkpoint failed mesh source history.");
    auto prefix = fileBytes(); validateCheckpoint(file_id_, ctx, next_report_ms, snap, n_steps_, next_due_days_, gw_created_);
    detail::CompletedSourceArchive out; uint32_t version = 1;
    auto step = report_2d_step_days_; out(version, step, n_steps_, next_due_days_, gw_created_, prefix); return out.bytes();
}

std::shared_ptr<Default2DOutputPlugin::PreparedCheckpoint> Default2DOutputPlugin::prepareCheckpoint(
    const SimulationContext& ctx, double next_report_ms, const SimulationSnapshot& snap,
    const SourceResultHistory* initial_history, std::string bytes) {
    std::lock_guard<std::recursive_mutex> hdf_lock(io::resultHdf5Mutex());
    { std::lock_guard<std::mutex> lock(source_mutex_);
      require(source_pending_.empty() && !source_written_, "Mesh restore requires an unbound fresh source writer."); }
    require(n_steps_ == 0 && fileBytes() == checkpoint_header_, "Mesh restore requires its fresh result header.");
    detail::CompletedSourceArchive in(std::move(bytes)); uint32_t version = 0; double step = 0; std::string prefix;
    auto result = std::make_shared<PreparedCheckpoint>();
    in(version, step, result->steps, result->next_due, result->groundwater, prefix); in.finish();
    require(version == 1 && step == report_2d_step_days_ && !prefix.empty() && prefix.size() <= maxBytes,
            "Mesh checkpoint format or reporting interval differs.");
    const auto path = io::utf8_path(h5_path_); static std::atomic<unsigned long long> serial{0};
    FILE* stream = nullptr;
    for (int k = 0; k < 32 && !stream; ++k) {
        result->staging = path; result->staging += ".mesh-part-" + std::to_string(++serial);
        stream = io::fopen_path(result->staging, "wbx"); if (!stream) result->staging.clear();
    }
    require(stream, "Cannot create staged mesh result prefix.");
    const bool wrote = std::fwrite(prefix.data(), 1, prefix.size(), stream) == prefix.size(); const bool closed = std::fclose(stream) == 0;
    require(wrote && closed, "Cannot stage mesh result prefix.");
    result->file = H5Fopen(result->staging.string().c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
    require(result->file >= 0, "Cannot reopen staged mesh result prefix.");
    validateCheckpoint(result->file, ctx, next_report_ms, snap, result->steps, result->next_due, result->groundwater);
    resolveCheckpointDatasets(*result,snap);
    if(H5Lexists(result->file,"source_history",H5P_DEFAULT)>0) {
        Handle group{H5Gopen2(result->file,"source_history",H5P_DEFAULT),H5Gclose};
        require(group.id>=0,"Invalid mesh source result group.");localHistory(group.id);
    }
    SourceResultHistory history; bool available = false;
    const auto error = SourceResultHistory::read(result->staging.string(), history, available);
    require(error.empty(), error.c_str());
    if (available) {
        require(initial_history && history.pollutantNames().empty() &&
                history.tables().at("boundaries").columns[3].back()<=snap.sim_time,
                "Mesh source result ownership or coverage differs.");
        const auto differs=history.extends(*initial_history);require(differs.empty(),differs.c_str());
        result->history = std::make_shared<SourceResultHistory>(std::move(history));
    }
    return result;
}

void Default2DOutputPlugin::resolveCheckpointDatasets(PreparedCheckpoint& result,const SimulationSnapshot& snap) {
    // Resolve all IDs while failure still leaves the live engine and writer untouched.
    Default2DOutputPlugin reference(""); reference.file_id_ = openImage(checkpoint_header_);
    reference.n_faces_ = n_faces_; reference.report_vars_ = report_vars_;
    reference.storage_type_ = storage_type_; reference.compression_ = compression_;
    if (result.groundwater) reference.createGroundwaterDatasets(snap);
    if (result.steps>0) reference.createSnowDatasets(snap);
    if (result.steps > 0 && reference.want(report2d::HEAT) && snap.thermal2d_active)
        reference.createThermalDatasets(snap);
    result.heat = reference.heat_created_;
    result.heat_layers = reference.n_heat_layers_;
    result.heat_lid_layers = reference.n_heat_lid_layers_;
    const auto targets = checkpointDatasets(), gw = reference.checkpointDatasets();
    result.datasets.reserve(targets.size());
    for (std::size_t k = 0; k < targets.size(); ++k) {
        const hid_t known = *targets[k] >= 0 ? *targets[k] : *gw[k];
        if (known < 0) continue;
        const auto name = datasetName(known); const auto id = H5Dopen2(result.file, name.c_str(), H5P_DEFAULT);
        require(id >= 0, "Cannot reopen mesh checkpoint dataset."); result.datasets.emplace_back(targets[k], id);
    }
    result.beds = reference.n_gw_beds_; result.layers = reference.n_gw_layers_;
 }

std::string Default2DOutputPlugin::captureFileCheckpoint(const SimulationContext& ctx,double next,const SimulationSnapshot& snap,
    const std::filesystem::path& base,std::shared_ptr<io::OwnedCheckpointFile>& artifact) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    {std::lock_guard<std::mutex> source_lock(source_mutex_);require(source_pending_.empty()&&!source_written_,"File checkpoint uses its separate full source stream.");}
    require(!checkpoint_write_failed_&&file_id_>=0&&H5Lexists(file_id_,"source_history",H5P_DEFAULT)==0&&H5Fflush(file_id_,H5F_SCOPE_GLOBAL)>=0,"Cannot settle mesh file checkpoint or embedded source history.");
    validateCheckpoint(file_id_,ctx,next,snap,n_steps_,next_due_days_,gw_created_);
    const auto path=io::utf8_path(h5_path_);auto copy=io::copyCheckpointFile(path,io::checkpointFileSize(path),base);
    detail::CompletedSourceArchive a;uint32_t version=1;auto step=report_2d_step_days_;auto descriptor=copy->file.encode();a(version,step,n_steps_,next_due_days_,gw_created_,descriptor);artifact=std::move(copy);return a.bytes();
}
std::shared_ptr<Default2DOutputPlugin::PreparedCheckpoint> Default2DOutputPlugin::prepareFileCheckpoint(const SimulationContext& ctx,
    double next,const SimulationSnapshot& snap,std::string bytes) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    {std::lock_guard<std::mutex> source_lock(source_mutex_);require(source_pending_.empty()&&!source_written_,"File restore requires its separate source stream.");}
    require(n_steps_==0&&fileBytes()==checkpoint_header_,"Mesh file restore requires its fresh header.");
    detail::CompletedSourceArchive a(std::move(bytes));uint32_t version;double step;std::string descriptor;auto result=std::make_shared<PreparedCheckpoint>();
    a(version,step,result->steps,result->next_due,result->groundwater,descriptor);a.finish();require(version==1&&step==report_2d_step_days_,"Mesh file checkpoint version or cadence differs.");
    auto saved=io::CheckpointFile::decode(std::move(descriptor));saved.verify();result->file_copy=io::copyCheckpointFile(saved.path,saved.bytes,io::utf8_path(h5_path_));
    require(result->file_copy->file.fingerprint==saved.fingerprint,"Mesh companion changed during copying.");result->staging=result->file_copy->file.path;
    result->file=H5Fopen(result->staging.string().c_str(),H5F_ACC_RDWR,H5P_DEFAULT);require(result->file>=0&&H5Lexists(result->file,"source_history",H5P_DEFAULT)==0,"Mesh file checkpoint contains unsupported embedded history.");
    validateCheckpoint(result->file,ctx,next,snap,result->steps,result->next_due,result->groundwater);resolveCheckpointDatasets(*result,snap);return result;
}

io::CheckpointOutputPublication::Output Default2DOutputPlugin::checkpointPublication(const PreparedCheckpoint& prepared) const {
    const auto properties=H5Fget_access_plist(file_id_);require(properties>=0,"Cannot inspect mesh output driver.");
    const bool sec2=H5Pget_driver(properties)==H5FD_SEC2;H5Pclose(properties);
    void* handle=nullptr;
    require(sec2 && H5Fget_vfd_handle(file_id_,H5P_DEFAULT,&handle)>=0 && handle,
            "Mesh checkpoint publication requires the native file descriptor driver.");
    return {prepared.staging,io::utf8_path(h5_path_),checkpoint_header_,*static_cast<int*>(handle)};
}
void Default2DOutputPlugin::installCheckpoint(PreparedCheckpoint& prepared) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    io::CheckpointOutputPublication publication({checkpointPublication(prepared)});
    publication.publish();adoptCheckpoint(prepared);
}
// Caller holds the result HDF5 mutex before publication and adoption.
void Default2DOutputPlugin::adoptCheckpoint(PreparedCheckpoint& prepared) noexcept {
    // All throwing checks and ID opens precede publication. The solver moves next.
    closeDatasets(); H5Fclose(file_id_); file_id_ = prepared.file; prepared.file = H5I_INVALID_HID;
    for (auto& [target, id] : prepared.datasets) { *target = id; id = H5I_INVALID_HID; }
    if(prepared.file_copy)prepared.file_copy->retain();
    prepared.staging.clear(); n_steps_ = prepared.steps; next_due_days_ = prepared.next_due;
    gw_created_ = prepared.groundwater; n_gw_beds_ = prepared.beds; n_gw_layers_ = prepared.layers;
    heat_created_ = prepared.heat; n_heat_layers_ = prepared.heat_layers;
    n_heat_lid_layers_ = prepared.heat_lid_layers;
    source_written_ = std::move(prepared.history);
    state_ = n_steps_ ? PluginState::UPDATING : PluginState::PREPARED;
}
} // namespace openswmm::twoD
#endif
