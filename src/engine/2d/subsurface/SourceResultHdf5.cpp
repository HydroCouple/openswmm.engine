// SPDX-License-Identifier: Apache-2.0
#include "SourceResultHistory.hpp"
#ifdef OPENSWMM_HAS_HDF5_MODEL
#include "../../io/hdf5/Hdf5Util.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace openswmm::twoD {
namespace {
using h5io::Handle;
constexpr std::size_t maxValues=8*1024*1024, nameBytes=512, maxNames=65536;
const std::string artifactKind="OpenSWMM internal source result history";
const std::string semantics="accepted source intervals; frame 0 initial; receiver and queues at capture; cumulative ledgers; not hydraulic report time";
const std::string outfallSemantics="water-only accepted hydraulic credits; separate completed hydraulic cursor; queued availability may exceed source end; pending snapshots not additive; no per-outfall or adoption events";
const std::map<std::string,std::string> metadata={
    {"schema_version","1"},{"temporal_meaning",semantics},
    {"sign_convention","nonnegative amounts; signed residual; -1 unavailable identity/adoption/refresh"},
    {"donor_kinds","0 mesh; 1 non-LID; 2 LID bottom"},
    {"return_kinds","0 numerical refund; 1 physical Dunne; 2 column excess"},
    {"et_owners","0 mesh; 1 pervious; 2 impervious; 3 LID"},
    {"refresh_convention","-1 unpublished; otherwise elapsed seconds at receiver refresh"},
    {"mass_convention","native concentration times m3; per-species mass units; no kg/lb conversion; no age/heat/MSX"},
    {"pool_convention","type -1: IMPERV0/IMPERV1/PERV (fourth unused); otherwise LID surface/pavement/soil/storage; usage lookup maps type/unit"},
    {"footprint_convention","reviewed uniform weather footprints; cell -1 outside mesh; owner follows et_owners"}
};
struct QuietErrors {
    H5E_auto2_t function=nullptr;void* client=nullptr;
    QuietErrors(){H5Eget_auto2(H5E_DEFAULT,&function,&client);H5Eset_auto2(H5E_DEFAULT,nullptr,nullptr);}
    ~QuietErrors(){H5Eset_auto2(H5E_DEFAULT,function,client);}
};
void require(bool valid,const char* error){if(!valid)throw std::runtime_error(error);}
struct PublicationFile {
    hid_t id=-1;
    explicit PublicationFile(const std::string& path):id(H5Fcreate(path.c_str(),H5F_ACC_EXCL,H5P_DEFAULT,H5P_DEFAULT)){}
    ~PublicationFile(){if(id>=0)H5Fclose(id);}
    void close(){require(H5Fclose(id)>=0,"Cannot close source result publication file.");id=-1;}
};
Handle stringType() {
    Handle t(H5Tcopy(H5T_C_S1),Handle::Kind::Type);
    require(t.ok()&&H5Tset_size(t.get(),nameBytes)>=0&&H5Tset_cset(t.get(),H5T_CSET_UTF8)>=0&&
        H5Tset_strpad(t.get(),H5T_STR_NULLPAD)>=0,"Cannot create source result string type.");return t;
}
void attribute(hid_t loc,const char* name,const std::string& value) {
    auto type=stringType();Handle space(H5Screate(H5S_SCALAR),Handle::Kind::Space);
    Handle a(H5Acreate2(loc,name,type.get(),space.get(),H5P_DEFAULT,H5P_DEFAULT),Handle::Kind::Attr);
    char bytes[nameBytes]={};require(value.size()<nameBytes,"Source result attribute too long.");
    std::memcpy(bytes,value.data(),value.size());require(a.ok()&&H5Awrite(a.get(),type.get(),bytes)>=0,"Cannot write source result attribute.");
}
std::string attribute(hid_t loc,const char* name) {
    Handle a(H5Aopen(loc,name,H5P_DEFAULT),Handle::Kind::Attr);require(a.ok(),"Missing source result attribute.");
    Handle type(H5Aget_type(a.get()),Handle::Kind::Type);Handle space(H5Aget_space(a.get()),Handle::Kind::Space);
    require(type.ok()&&space.ok()&&H5Sget_simple_extent_type(space.get())==H5S_SCALAR&&
        H5Tget_class(type.get())==H5T_STRING&&H5Tis_variable_str(type.get())==0&&H5Tget_size(type.get())==nameBytes&&H5Tget_cset(type.get())==H5T_CSET_UTF8,
        "Invalid source result attribute shape or type.");
    char bytes[nameBytes]={};require(H5Aread(a.get(),type.get(),bytes)>=0&&std::memchr(bytes,0,nameBytes),"Cannot read source result attribute.");
    return std::string(bytes);
}
std::size_t length(hid_t dataset) {
    Handle space(H5Dget_space(dataset),Handle::Kind::Space);hsize_t dims[1]={};
    require(space.ok()&&H5Sget_simple_extent_ndims(space.get())==1&&H5Sget_simple_extent_dims(space.get(),dims,nullptr)>=0&&dims[0]<=maxValues,
        "Invalid source result column rank or size.");return std::size_t(dims[0]);
}
void names(hid_t group,const char* name,const std::vector<std::string>& values) {
    require(values.size()<=maxNames,"Too many source result names.");auto type=stringType();
    hsize_t dim=values.size();Handle space(H5Screate_simple(1,&dim,nullptr),Handle::Kind::Space);
    Handle d(H5Dcreate2(group,name,type.get(),space.get(),H5P_DEFAULT,H5P_DEFAULT,H5P_DEFAULT),Handle::Kind::Dataset);
    std::vector<char> bytes(values.size()*nameBytes,0);
    for(std::size_t i=0;i<values.size();++i) {
        require(values[i].size()<nameBytes,"Source result name too long.");std::memcpy(bytes.data()+i*nameBytes,values[i].data(),values[i].size());
    }
    require(d.ok()&&(values.empty()||H5Dwrite(d.get(),type.get(),H5S_ALL,H5S_ALL,H5P_DEFAULT,bytes.data())>=0),"Cannot write source result names.");
}
std::vector<std::string> names(hid_t group,const char* name) {
    Handle d(H5Dopen2(group,name,H5P_DEFAULT),Handle::Kind::Dataset);require(d.ok(),"Missing source result names.");
    const auto count=length(d.get());Handle type(H5Dget_type(d.get()),Handle::Kind::Type);
    require(count<=maxNames&&type.ok()&&H5Tget_class(type.get())==H5T_STRING&&H5Tis_variable_str(type.get())==0&&
        H5Tget_size(type.get())==nameBytes&&H5Tget_cset(type.get())==H5T_CSET_UTF8,"Invalid source result name type or count.");
    std::vector<char> bytes(count*nameBytes,0);
    require(count==0||H5Dread(d.get(),type.get(),H5S_ALL,H5S_ALL,H5P_DEFAULT,bytes.data())>=0,"Cannot read source result names.");
    std::vector<std::string> out;out.reserve(count);
    for(std::size_t i=0;i<count;++i) {
        const auto* p=bytes.data()+i*nameBytes;require(std::memchr(p,0,nameBytes)!=nullptr,"Unterminated source result name.");out.emplace_back(p);
    }
    return out;
}
void writeTable(hid_t root,const std::string& name,const SourceResultTable& t) {
    auto group=h5io::make_group(root,name);require(group.ok(),"Cannot create source result table.");
    for(std::size_t c=0;c<t.columns.size();++c) {
        hsize_t dim=t.columns[c].size();Handle space(H5Screate_simple(1,&dim,nullptr),Handle::Kind::Space);
        Handle d(H5Dcreate2(group.get(),t.fields[c].c_str(),H5T_IEEE_F64LE,space.get(),H5P_DEFAULT,H5P_DEFAULT,H5P_DEFAULT),Handle::Kind::Dataset);
        require(d.ok()&&(dim==0||H5Dwrite(d.get(),H5T_NATIVE_DOUBLE,H5S_ALL,H5S_ALL,H5P_DEFAULT,t.columns[c].data())>=0),"Cannot write source result column.");
        attribute(d.get(),"units",t.units[c]);
    }
}
void readTable(hid_t root,const std::string& name,SourceResultTable& t,std::size_t& total) {
    auto group=h5io::open_group(root,name);require(group.ok(),"Missing source result table.");
    std::size_t rows=0;
    for(std::size_t c=0;c<t.columns.size();++c) {
        Handle d(H5Dopen2(group.get(),t.fields[c].c_str(),H5P_DEFAULT),Handle::Kind::Dataset);require(d.ok(),"Missing source result column.");
        const auto count=length(d.get());if(c==0)rows=count;
        Handle type(H5Dget_type(d.get()),Handle::Kind::Type);
        require(count==rows&&count<=maxValues-total&&type.ok()&&H5Tequal(type.get(),H5T_IEEE_F64LE)>0,"Source result column shape, type or size differs.");
        total+=count;require(attribute(d.get(),"units")==t.units[c],"Source result column units differ.");
        t.columns[c].resize(count);
        require(count==0||H5Dread(d.get(),H5T_NATIVE_DOUBLE,H5S_ALL,H5S_ALL,H5P_DEFAULT,t.columns[c].data())>=0,"Cannot read source result column.");
    }
}
void boundary(hid_t group,const std::string& bytes) {
    hsize_t dim=bytes.size();Handle space(H5Screate_simple(1,&dim,nullptr),Handle::Kind::Space);
    Handle d(H5Dcreate2(group,"checkpoint_match",H5T_STD_U8LE,space.get(),H5P_DEFAULT,H5P_DEFAULT,H5P_DEFAULT),Handle::Kind::Dataset);
    require(d.ok()&&H5Dwrite(d.get(),H5T_NATIVE_UCHAR,H5S_ALL,H5S_ALL,H5P_DEFAULT,bytes.data())>=0,"Cannot write source checkpoint match.");
}
std::string boundary(hid_t group) {
    Handle d(H5Dopen2(group,"checkpoint_match",H5P_DEFAULT),Handle::Kind::Dataset);require(d.ok(),"Missing source checkpoint match.");
    Handle type(H5Dget_type(d.get()),Handle::Kind::Type);Handle space(H5Dget_space(d.get()),Handle::Kind::Space);hsize_t dim=0;
    require(type.ok()&&H5Tequal(type.get(),H5T_STD_U8LE)>0&&space.ok()&&H5Sget_simple_extent_ndims(space.get())==1&&
        H5Sget_simple_extent_dims(space.get(),&dim,nullptr)>=0&&dim>0&&dim<=64*1024*1024,"Invalid source checkpoint match shape/type/size.");
    std::string bytes(std::size_t(dim),'\0');require(H5Dread(d.get(),H5T_NATIVE_UCHAR,H5S_ALL,H5S_ALL,H5P_DEFAULT,bytes.data())>=0,"Cannot read source checkpoint match.");return bytes;
}

}
struct SourceResultFile {
    static void writeGroup(hid_t group,const SourceResultHistory& history) {
        for(const auto& [key,value]:metadata)attribute(group,key.c_str(),key=="schema_version"?
            (history.outfall_returns_?"3":!history.boundary_.empty()?"2":"1"):value);
        if(history.outfall_returns_)attribute(group,"outfall_return_convention",outfallSemantics);
        names(group,"source_names",history.sources_);names(group,"pollutant_names",history.pollutants_);names(group,"pollutant_mass_units",history.mass_units_);
        for(const auto& [name,t]:history.tables_)writeTable(group,name,t);
        if(!history.boundary_.empty())boundary(group,history.boundary_);
    }
    // Publish the optional group inside the engine-owned open HDF5 file.
    // Other root datasets/time axes are untouched. This is process-failure
    // rollback, not crash/power-loss atomicity or the production streaming gate.
    static std::string embed(const SourceResultHistory& history,hid_t file) {
        const char* staged="__source_history_next",*old="__source_history_previous";
        bool made=false,moved=false,published=false;
        QuietErrors quiet;
        try {
            const auto invalid=history.validate();if(!invalid.empty())return invalid;
            require(file>=0&&H5Iis_valid(file)>0,"Source output file is not open.");
            require(!h5io::has_link(file,staged)&&!h5io::has_link(file,old),"Source output has an unfinished publication.");
            auto group=h5io::make_group(file,staged);require(group.ok(),"Cannot stage source result group.");made=true;
            writeGroup(group.get(),history);group.close();
            require(H5Fflush(file,H5F_SCOPE_GLOBAL)>=0,"Cannot flush staged source results.");
            if(h5io::has_link(file,"source_history")) {
                require(H5Lmove(file,"source_history",file,old,H5P_DEFAULT,H5P_DEFAULT)>=0,"Cannot retain previous source results.");moved=true;
            }
            require(H5Lmove(file,staged,file,"source_history",H5P_DEFAULT,H5P_DEFAULT)>=0,"Cannot publish source result group.");published=true;
            require(H5Fflush(file,H5F_SCOPE_GLOBAL)>=0,"Cannot flush published source results.");
            if(moved)require(H5Ldelete(file,old,H5P_DEFAULT)>=0,"Cannot release previous source result group.");
            return {};
        } catch(const std::exception& e) {
            if(published)H5Ldelete(file,"source_history",H5P_DEFAULT);
            if(moved)H5Lmove(file,old,file,"source_history",H5P_DEFAULT,H5P_DEFAULT);
            if(made&&!published)H5Ldelete(file,staged,H5P_DEFAULT);
            H5Fflush(file,H5F_SCOPE_GLOBAL);return e.what();
        }
    }

    static std::string save(const SourceResultHistory& history,const std::string& path) {
        std::string part;
        try {
            const auto invalid=history.validate();if(!invalid.empty())return invalid;
            QuietErrors quiet;
            if(std::filesystem::exists(path)) {
                auto existing=h5io::open_file_readonly(path);
                require(existing.ok()&&attribute(existing.get(),"artifact_kind")==artifactKind,"Destination is not a standalone source result artifact.");
            }
            static std::atomic<unsigned long> serial{0};part=path+".source-results-part-"+std::to_string(++serial);
            PublicationFile file(part);
            // Never delete someone else's publication file after an exclusive-create failure.
            if(file.id<0){part.clear();return "Cannot create source result publication file.";}
            attribute(file.id,"artifact_kind",artifactKind);
            auto group=h5io::make_group(file.id,"source_history");require(group.ok(),"Cannot create source result history group.");
            writeGroup(group.get(),history);
            require(H5Fflush(file.id,H5F_SCOPE_GLOBAL)>=0,"Cannot flush source result file.");
            group.close();file.close();
            std::error_code error;std::filesystem::rename(part,path,error);
            require(!error,"Cannot publish source result file.");part.clear();return {};
        } catch(const std::exception& e) {
            if(!part.empty()){std::error_code ignored;std::filesystem::remove(part,ignored);}return e.what();
        }
    }
    static std::string read(const std::string& path,SourceResultHistory& out,bool& available) {
        available=false;
        try {
            QuietErrors quiet;
            std::error_code error;const auto size=std::filesystem::file_size(path,error);
            require(!error&&size>0&&size<=256*1024*1024,"Source result file missing or exceeds size limit.");
            auto file=h5io::open_file_readonly(path);require(file.ok(),"Cannot open source result HDF5 file.");
            require(!h5io::has_link(file.get(),"__source_history_next")&&!h5io::has_link(file.get(),"__source_history_previous"),"Source output contains an unfinished publication.");
            if(!h5io::has_link(file.get(),"source_history"))return {};
            auto group=h5io::open_group(file.get(),"source_history");require(group.ok(),"Invalid source result group.");
            const auto version=attribute(group.get(),"schema_version");require(version=="1"||version=="2"||version=="3","Unsupported source result schema.");
            for(const auto& [key,value]:metadata)if(key!="schema_version")require(attribute(group.get(),key.c_str())==value,"Unsupported source result metadata or schema.");
            SourceResultHistory trial;trial.outfall_returns_=version=="3";
            if(trial.outfall_returns_)require(attribute(group.get(),"outfall_return_convention")==outfallSemantics,"Unsupported outfall source result metadata.");
            trial.tables_=SourceResultHistory::schema(trial.outfall_returns_);
            if(version!="1")trial.boundary_=boundary(group.get());
            trial.sources_=names(group.get(),"source_names");trial.pollutants_=names(group.get(),"pollutant_names");trial.mass_units_=names(group.get(),"pollutant_mass_units");
            std::size_t total=0;for(auto& [name,t]:trial.tables_)readTable(group.get(),name,t,total);
            const auto invalid=trial.validate();if(!invalid.empty())return invalid;
            out=std::move(trial);available=true;return {};
        } catch(const std::exception& e){return e.what();}
    }
};
std::string SourceResultHistory::writeIntoOpenResult(std::int64_t file) const {return SourceResultFile::embed(*this,file);}
std::string SourceResultHistory::save(const std::string& path) const {return SourceResultFile::save(*this,path);}
std::string SourceResultHistory::read(const std::string& path,SourceResultHistory& out,bool& available){return SourceResultFile::read(path,out,available);}
}
#else
namespace openswmm::twoD {
std::string SourceResultHistory::writeIntoOpenResult(std::int64_t) const {return "Source result HDF5 support unavailable in this build.";}
std::string SourceResultHistory::save(const std::string&) const {return "Source result HDF5 support unavailable in this build.";}
std::string SourceResultHistory::read(const std::string&,SourceResultHistory&,bool& available){available=false;return "Source result HDF5 support unavailable in this build.";}
}
#endif
