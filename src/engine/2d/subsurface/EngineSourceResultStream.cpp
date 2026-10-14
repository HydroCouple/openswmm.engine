// SPDX-License-Identifier: Apache-2.0
#include "EngineSourceResultStream.hpp"
#include "EngineSourceResultHistory.hpp"
#include "CommonSourceStage.hpp"
#include "../../core/SWMMEngine.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace openswmm::twoD {
namespace {
using Archive=detail::CompletedSourceArchive;
using Stream=EngineSourceResultStream;
constexpr uint64_t maxFrame=64*1024*1024,marker=0x31454d4152465353ULL,seed=14695981039346656037ULL;
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
uint64_t hash(uint64_t h,const char* p,std::size_t n) {for(std::size_t i=0;i<n;++i){h^=static_cast<unsigned char>(p[i]);h*=1099511628211ULL;}return h;}
uint64_t hash(uint64_t h,const std::string& s) {return hash(h,s.data(),s.size());}
struct File { FILE* f=nullptr; explicit File(const std::string& path,const char* mode):f(std::fopen(path.c_str(),mode)) {require(f,"Cannot open source stream.");std::setvbuf(f,nullptr,_IONBF,0);} ~File(){if(f)std::fclose(f);} };
void read(FILE* f,void* p,std::size_t n) {require(std::fread(p,1,n,f)==n,"Source stream prefix is truncated or unreadable.");}
void write(FILE* f,const std::string& s) {require(std::fwrite(s.data(),1,s.size(),f)==s.size(),"Cannot write source stream frame.");}
void sync(FILE* f) {
    require(std::fflush(f)==0,"Cannot flush source stream.");
#ifdef _WIN32
    require(_commit(_fileno(f))==0,"Cannot synchronize source stream.");
#else
    require(fsync(fileno(f))==0,"Cannot synchronize source stream.");
#endif
}
void seek(FILE* f,uint64_t n) {
#ifdef _WIN32
    require(n<=INT64_MAX && _fseeki64(f,static_cast<int64_t>(n),SEEK_SET)==0,"Cannot seek source stream.");
#else
    require(n<=INT64_MAX && fseeko(f,static_cast<off_t>(n),SEEK_SET)==0,"Cannot seek source stream.");
#endif
}
void truncate(FILE* f,uint64_t n) {
#ifdef _WIN32
    require(_chsize_s(_fileno(f),n)==0,"Cannot roll back uncommitted source frame.");
#else
    require(n<=INT64_MAX && ftruncate(fileno(f),static_cast<off_t>(n))==0,"Cannot roll back uncommitted source frame.");
#endif
}
struct Identity {uint64_t device,inode,size;};
Identity identity(const std::string& path,bool writable=false) {
    auto status=std::filesystem::symlink_status(path);
    require(std::filesystem::is_regular_file(status),"Source stream must be a regular file, not a symlink.");
    if(writable)require((status.permissions()&(std::filesystem::perms::owner_write|std::filesystem::perms::group_write|std::filesystem::perms::others_write))!=std::filesystem::perms::none,"Source stream is not writable.");
#ifdef _WIN32
    struct _stat64 s{};require(_wstat64(std::filesystem::path(path).c_str(),&s)==0,"Cannot inspect source stream.");
#else
    struct stat s{};require(stat(path.c_str(),&s)==0,"Cannot inspect source stream.");
#endif
    return {uint64_t(s.st_dev),uint64_t(s.st_ino),uint64_t(s.st_size)};
}
void tables(Archive& a,std::map<std::string,SourceResultTable>& ts) {
    uint64_t size=ts.size();a(size);require(size<=64,"Too many source stream tables.");
    if(a.reading())ts.clear();
    if(a.reading())for(uint64_t i=0;i<size;++i) {
        std::string name;SourceResultTable t;a(name,t.fields,t.units,t.columns);
        require(ts.emplace(std::move(name),std::move(t)).second,"Duplicate source stream table.");
    } else for(auto& [name,t]:ts) {auto key=name;a(key,t.fields,t.units,t.columns);}
}
void shape(const Stream::Frame& f) {
    require(uint32_t(f.kind)<=uint32_t(Stream::Kind::Checkpoint) && !f.tables.empty(),"Invalid source stream capture kind or tables.");
    require(std::isfinite(f.engine_start)&&std::isfinite(f.engine_end)&&std::isfinite(f.source_start)&&std::isfinite(f.source_end)&&
        f.engine_start>=0&&f.engine_end>=f.engine_start&&f.source_start>=0&&f.source_end>=f.source_start,"Invalid source stream clocks.");
    std::size_t values=0;
    for(const auto& [name,t]:f.tables) {
        require(!name.empty()&&name.size()<128&&t.fields.size()==t.units.size()&&t.fields.size()==t.columns.size()&&!t.columns.empty(),"Invalid source stream table schema.");
        for(std::size_t c=0;c<t.columns.size();++c) {
            require(!t.fields[c].empty()&&t.fields[c].size()<128&&t.units[c].size()<128&&t.columns[c].size()==t.rows()&&
                t.rows()<=8*1024*1024-values,"Invalid or oversized source stream column.");values+=t.rows();
            for(double v:t.columns[c])require(std::isfinite(v),"Nonfinite source stream value.");
        }
    }
}
std::string encode(Stream::Frame& f) {
    shape(f);Archive a;a(f.kind,f.sequence,f.source_intervals,f.boundary_fingerprint,f.engine_start,f.engine_end,f.source_start,f.source_end);tables(a,f.tables);
    require(a.bytes().size()<=maxFrame,"Source stream frame exceeds its size limit.");return a.bytes();
}
Stream::Frame decode(std::string bytes) {
    Archive a(std::move(bytes));Stream::Frame f;a(f.kind,f.sequence,f.source_intervals,f.boundary_fingerprint,f.engine_start,f.engine_end,f.source_start,f.source_end);tables(a,f.tables);a.finish();shape(f);return f;
}
std::string header(const std::string& id,const std::string& metadata) {
    Archive a;std::string magic="OpenSWMM source stream",identity=id,model=metadata;uint32_t version=1,endian=0x01020304;a(magic,version,endian,identity,model);
    uint64_t size=a.bytes().size();require(size<=maxFrame,"Source stream metadata exceeds its size limit.");Archive out;out(size);return out.bytes()+a.bytes();
}
struct Header {std::string identity,metadata;uint64_t bytes=0,fingerprint=0;};
Header readHeader(FILE* f) {
    uint64_t size;read(f,&size,sizeof(size));require(size>0&&size<=maxFrame,"Invalid source stream header size.");
    std::string bytes(size,'\0');read(f,bytes.data(),bytes.size());Archive a(bytes);Header h;std::string magic;uint32_t version,endian;a(magic,version,endian,h.identity,h.metadata);a.finish();
    require(magic=="OpenSWMM source stream"&&version==1&&endian==0x01020304&&h.identity.size()==32&&!h.metadata.empty(),"Source stream format or identity differs.");
    h.bytes=sizeof(size)+size;h.fingerprint=hash(hash(seed,reinterpret_cast<const char*>(&size),sizeof(size)),bytes);return h;
}
struct Progress {
    uint64_t frames=0,intervals=0;double engine_end=0,source_end=0;bool detailed=false;
    std::map<std::string,SourceResultTable> schemas;
    void accept(const Stream::Frame& f) {
        require(f.sequence==frames && frames<9007199254740991ULL && f.source_intervals<=9007199254740991ULL,"Source stream sequence is not contiguous or exceeds exact table precision.");
        if(!frames)for(const auto& [name,t]:f.tables) {SourceResultTable schema;schema.fields=t.fields;schema.units=t.units;schema.columns.resize(t.columns.size());schemas.emplace(name,std::move(schema));}
        else if(!schemas.empty()) {
            std::size_t expected=0;
            const bool source=f.kind==Stream::Kind::SourceInterval;
            for(const auto& [name,t]:schemas) {
                if(source?(!name.starts_with("source/")||name=="source/footprints"||name=="source/lid_usages"):!name.starts_with("engine/"))continue;
                ++expected;const auto found=f.tables.find(name);require(found!=f.tables.end()&&found->second.fields==t.fields&&found->second.units==t.units,"Source stream table fields or units changed.");
            }
            require(f.tables.size()==expected,"Source stream frame omits or adds tables.");
        }
        if(!frames) {
            require(f.kind==Stream::Kind::Initial&&f.engine_start==0&&f.engine_end==0&&f.source_start==0&&f.source_end==0&&f.source_intervals==0&&f.boundary_fingerprint!=0,"Invalid initial source stream frame.");
        } else if(f.kind==Stream::Kind::SourceInterval) {
            require(detailed&&f.engine_start==engine_end&&f.engine_end==engine_end&&f.source_start==source_end&&f.source_end>source_end&&
                f.source_intervals==intervals+1&&f.boundary_fingerprint==0,"Invalid streamed source interval progression.");
            intervals=f.source_intervals;source_end=f.source_end;
        } else {
            require(f.kind!=Stream::Kind::Initial&&f.engine_start==engine_end&&f.boundary_fingerprint!=0&&
                (f.kind==Stream::Kind::EngineStep?f.engine_end>engine_end:f.engine_end==engine_end)&&f.source_end<=f.engine_end+1e-12*std::max(1.,f.engine_end),"Invalid streamed engine step or barrier.");
            require(detailed?f.source_intervals==intervals&&f.source_end==source_end:
                f.source_intervals>=intervals&&f.source_end>=source_end&&((f.source_intervals==intervals)==(f.source_end==source_end)),"Streamed source/engine coverage differs.");
            intervals=f.source_intervals;source_end=f.source_end;engine_end=f.engine_end;
        }
        const bool source=f.kind==Stream::Kind::SourceInterval;
        const auto found=f.tables.find(source?"source/boundaries":"engine/boundaries");require(found!=f.tables.end()&&found->second.rows()==1,"Source stream boundary table is missing.");
        const auto& b=found->second.columns;
        require(b.size()==(source?7:10),"Source stream boundary schema differs.");
        require(b[0][0]==double(source?f.source_intervals:f.sequence),"Source stream table sequence differs.");
        if(source)require(b.size()==7&&b[1][0]==double(f.source_intervals)&&b[2][0]==f.source_start&&b[3][0]==f.source_end,"Source stream interval table clocks differ.");
        else require(b.size()==10&&b[1][0]==f.engine_start&&b[2][0]==f.engine_end&&b[3][0]==double(f.source_intervals)&&b[4][0]==f.source_end,"Source stream engine table clocks differ.");
        if(!source&&f.kind!=Stream::Kind::EngineStep) {
            const auto& n=f.tables.at("engine/node_delivery").columns;require(n.size()==11,"Source stream node delivery schema differs.");
            for(std::size_t r=0;r<n[0].size();++r)require(n[6][r]==0&&n[7][r]==0&&n[8][r]==f.engine_end&&n[9][r]==f.engine_end&&n[10][r]==0,"Barrier replays an earlier routing receipt.");
        }
        ++frames;
    }
};
struct Scan {Header header;Stream::Frame latest;std::size_t largest=0;Progress progress;};
Scan scan(const std::string& path,const Stream::Cursor& cursor,const std::function<void(const Stream::Frame&)>& visit={}) {
    require(!cursor.identity.empty()&&cursor.frames>0&&identity(path).size>=cursor.bytes,"Source stream saved prefix is missing or truncated.");
    File file(path,"rb");Scan out;out.header=readHeader(file.f);
    require(out.header.identity==cursor.identity&&out.header.bytes<cursor.bytes,"Source stream identity differs.");
    {Archive a(out.header.metadata);std::string model;std::vector<std::string> sources,nodes;a(model,sources,nodes,out.progress.detailed);a.finish();}
    uint64_t bytes=out.header.bytes,digest=out.header.fingerprint;
    while(bytes<cursor.bytes) {
        require(cursor.bytes-bytes>=32,"Uncommitted source stream frame.");uint64_t size,previous,stored,end;
        read(file.f,&size,8);read(file.f,&previous,8);require(size>0&&size<=maxFrame&&size<=cursor.bytes-bytes-32&&previous==digest,"Invalid source stream frame length or prefix fingerprint.");
        std::string payload(size,'\0');read(file.f,payload.data(),payload.size());read(file.f,&stored,8);read(file.f,&end,8);
        digest=hash(hash(digest,reinterpret_cast<const char*>(&size),8),payload);
        require(end==marker&&stored==digest,"Source stream frame checksum or commit marker differs.");
        auto frame=decode(std::move(payload));out.progress.accept(frame);if(visit)visit(frame);out.latest=std::move(frame);out.largest=std::max(out.largest,std::size_t(size));bytes+=size+32;
    }
    require(bytes==cursor.bytes&&out.progress.frames==cursor.frames&&digest==cursor.fingerprint,"Source stream committed cursor differs.");return out;
}
void merge(std::map<std::string,SourceResultTable>& dest,std::map<std::string,SourceResultTable> source,const char* prefix) {
    for(auto& [name,t]:source)dest.emplace(std::string(prefix)+name,std::move(t));
}
}
uint64_t EngineSourceResultStream::fingerprint(const std::string& bytes) {return hash(seed,bytes);}
std::string EngineSourceResultStream::encodeCursor(const Cursor& c) {auto copy=c;Archive a;a(copy.identity,copy.bytes,copy.frames,copy.fingerprint);return a.bytes();}
EngineSourceResultStream::Cursor EngineSourceResultStream::decodeCursor(std::string bytes) {Archive a(std::move(bytes));Cursor c;a(c.identity,c.bytes,c.frames,c.fingerprint);a.finish();return c;}
std::string EngineSourceResultStream::visit(const std::string& path,const Cursor& cursor,const std::function<void(const Frame&)>& visitor) {
    try {scan(path,cursor,visitor);return {};}catch(const std::exception& e){return e.what();}
}
void EngineSourceResultStream::preflight() const {
    const auto now=identity(path_,true);require(now.device==device_&&now.inode==inode_&&now.size==cursor_.bytes,"Source stream destination changed, was truncated, or has an uncommitted tail.");
    if(cursor_.frames) {File f(path_,"rb");seek(f.f,cursor_.bytes-16);uint64_t digest,end;read(f.f,&digest,8);read(f.f,&end,8);require(digest==cursor_.fingerprint&&end==marker,"Source stream final commit marker changed.");}
}
void EngineSourceResultStream::append(Frame frame) {
    preflight();Progress progress{cursor_.frames,source_intervals_,engine_end_,source_end_,detailed_};progress.accept(frame);
    const auto payload=encode(frame);uint64_t size=payload.size(),previous=cursor_.fingerprint;
    const auto digest=hash(hash(previous,reinterpret_cast<const char*>(&size),8),payload);uint64_t end=marker;Archive a;a(size,previous);std::string record=a.bytes()+payload;Archive footer;auto saved=digest;footer(saved,end);record+=footer.bytes();
    require(record.size()<=UINT64_MAX-cursor_.bytes,"Source stream byte cursor overflow.");
    Cursor next=cursor_;next.bytes+=record.size();++next.frames;next.fingerprint=digest; // allocate before writing
    File f(path_,"rb+");seek(f.f,cursor_.bytes);
    try {write(f.f,record);sync(f.f);}catch(...) {std::clearerr(f.f);try {std::fflush(f.f);truncate(f.f,cursor_.bytes);sync(f.f);}catch(...) {}throw;}
    cursor_=std::move(next);latest_=std::move(frame);engine_end_=progress.engine_end;source_end_=progress.source_end;source_intervals_=progress.intervals;largest_frame_=std::max(largest_frame_,payload.size());
}
std::string EngineSourceResultStream::initialize(const SWMMEngine& e,const std::string& path,bool detailed) {
    bool created=false;
    try {
        require(!engine_&&path_.empty()&&e.ctx_.current_time==0&&e.surface_router_.commonSourceStage()&&(e.source_delivery_||e.surface_router_.subsurface().lidReceiptsEnabled()),"Attach an empty stream at the initial spatial-source checkpoint.");
        std::vector<std::string> sources,nodes;for(int sc=0;sc<e.ctx_.n_subcatches();++sc)sources.push_back(e.ctx_.subcatch_names.name_of(sc));for(int n=0;n<e.ctx_.n_nodes();++n)nodes.push_back(e.ctx_.node_names.name_of(n));
        Archive meta;auto model=e.checkpoint_model_;meta(model,sources,nodes,detailed);metadata_=meta.bytes();
        std::random_device random;const char* hex="0123456789abcdef";std::string id;id.reserve(32);for(int i=0;i<32;++i)id+=hex[random()&15];
        auto bytes=header(id,metadata_);path_=path;{File f(path,"wbx");created=true;write(f.f,bytes);sync(f.f);}
        auto named=identity(path);device_=named.device;inode_=named.inode;cursor_={id,uint64_t(bytes.size()),0,fingerprint(bytes)};engine_=&e;detailed_=detailed;
        const auto error=capture(e,Kind::Initial,0);require(error.empty(),error.c_str());
        // The fresh destination contains one complete initial frame, bounded by model size.
        File f(path,"rb");fresh_header_.resize(cursor_.bytes);read(f.f,fresh_header_.data(),fresh_header_.size());return {};
    }catch(const std::exception& e) {
        if(created){std::error_code ec;std::filesystem::remove(path,ec);}path_.clear();metadata_.clear();fresh_header_.clear();cursor_={};latest_={};engine_=nullptr;return e.what();
    }
}
std::string EngineSourceResultStream::capture(const SWMMEngine& e,Kind kind,double start,uint64_t anchor) {
    try {
        require(engine_==&e&&e.surface_router_.commonSourceStage()&&(e.source_delivery_||e.surface_router_.subsurface().lidReceiptsEnabled()),"Source stream belongs to a different engine.");
        if(!anchor){std::string bytes;const auto error=e.checkpointBytes(bytes,false);require(error.empty(),error.c_str());anchor=fingerprint(bytes);}
        pending_capture_=true;pending_kind_=kind;pending_start_=start;
        Frame frame;frame.kind=kind;frame.sequence=cursor_.frames;frame.engine_start=start;frame.engine_end=e.ctx_.current_time;frame.source_start=source_end_;frame.source_end=e.surface_router_.commonSourceStage()->completedEnd();frame.source_intervals=e.surface_router_.commonSourceStage()->intervals();frame.boundary_fingerprint=anchor;
        auto projected=EngineSourceResultHistory::projection(e,frame.sequence,start);
        if(kind!=Kind::EngineStep) {auto& n=projected.tables_.at("node_delivery").columns;for(std::size_t r=0;r<n[0].size();++r){n[6][r]=n[7][r]=n[10][r]=0;n[8][r]=n[9][r]=frame.engine_end;}}
        merge(frame.tables,std::move(projected.tables_),"engine/");
        if(kind==Kind::Initial) {auto source=SourceResultHistory::projection(*e.surface_router_.commonSourceStage(),0,0);merge(frame.tables,std::move(source.tables_),"source/");}
        append(std::move(frame));pending_capture_=false;error_.clear();return {};
    }catch(const std::exception& e){error_=e.what();return error_;}
}
std::string EngineSourceResultStream::captureSourceInterval(const CommonSourceStage& stage) {
    try {
        require(detailed_&&engine_&&engine_->surface_router_.commonSourceStage()==&stage,"Source interval requires its attached detailed stream.");
        Frame frame;frame.kind=Kind::SourceInterval;frame.sequence=cursor_.frames;frame.engine_start=frame.engine_end=engine_end_;frame.source_start=source_end_;frame.source_end=stage.completedEnd();frame.source_intervals=stage.intervals();
        auto source=SourceResultHistory::projection(stage,frame.source_intervals,frame.source_start);source.tables_.erase("footprints");source.tables_.erase("lid_usages");merge(frame.tables,std::move(source.tables_),"source/");append(std::move(frame));return {};
    }catch(const std::exception& e){error_=e.what();return error_;}
}
std::string EngineSourceResultStream::checkpointContract(uint64_t physical,uint64_t whole) const {
    require(latest_.kind!=Kind::SourceInterval&&latest_.boundary_fingerprint==physical,"Stream does not match the accepted physical checkpoint; settle recording first.");preflight();scan(path_,cursor_);
    Archive a;auto path=path_,cursor=encodeCursor(cursor_);a(path,cursor,physical,whole);return a.bytes();
}
EngineSourceResultStream::PreparedRestore::~PreparedRestore() {if(owns_staging){std::error_code ec;std::filesystem::remove(publication.staging,ec);}}
std::unique_ptr<EngineSourceResultStream::PreparedRestore> EngineSourceResultStream::prepareRestore(const std::string& contract,uint64_t whole_checkpoint) const {
    require(engine_&&engine_end_==0&&cursor_.frames==1,"Stream restore requires its fresh initial destination.");preflight();
    Archive a(contract);std::string source,saved;uint64_t anchor,whole;a(source,saved,anchor,whole);a.finish();
    require(whole==whole_checkpoint,"Stream saved prefix is bound to a different whole-engine/output checkpoint.");const auto cursor=decodeCursor(std::move(saved));auto verified=scan(source,cursor);
    require(verified.header.metadata==metadata_&&verified.latest.kind!=Kind::SourceInterval&&verified.latest.boundary_fingerprint==anchor,"Stream identity/model/profile or physical checkpoint binding differs.");
    require(std::filesystem::weakly_canonical(source)!=std::filesystem::weakly_canonical(path_)&&!std::filesystem::equivalent(source,path_),"Stream restore source aliases its destination.");
    auto prepared=std::make_unique<PreparedRestore>();prepared->publication={path_+".stream-part-"+cursor_.identity+"-"+std::to_string(++preparation_sequence_),path_,fresh_header_,-1};
    {File in(source,"rb"),out(prepared->publication.staging.string(),"wbx");prepared->owns_staging=true;std::string buffer(64*1024,'\0');uint64_t left=cursor.bytes;while(left){auto size=std::min(left,uint64_t(buffer.size()));read(in.f,buffer.data(),size);require(std::fwrite(buffer.data(),1,size,out.f)==size,"Cannot stage source stream prefix.");left-=size;}sync(out.f);}
    // Validate the copied bytes as well, detecting changes during preparation.
    auto copied=scan(prepared->publication.staging.string(),cursor);auto named=identity(prepared->publication.staging.string());
    prepared->cursor=cursor;prepared->latest=std::move(copied.latest);prepared->device=named.device;prepared->inode=named.inode;prepared->largest_frame=copied.largest;return prepared;
}
void EngineSourceResultStream::adopt(PreparedRestore& p) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<Cursor> && std::is_nothrow_move_assignable_v<Frame>);
    p.owns_staging=false;
    cursor_=std::move(p.cursor);latest_=std::move(p.latest);device_=p.device;inode_=p.inode;largest_frame_=p.largest_frame;
    source_intervals_=latest_.source_intervals;engine_end_=latest_.engine_end;source_end_=latest_.source_end;pending_capture_=false;error_.clear();
}
}
