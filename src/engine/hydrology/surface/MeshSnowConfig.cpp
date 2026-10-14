// SPDX-License-Identifier: Apache-2.0
#include "MeshSnowConfig.hpp"
#include "../../input/InputParseUtils.hpp"
#include "../../input/Tokenizer.hpp"
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
namespace openswmm::surface {
namespace {
std::string upper(const std::string& s){return input::Tokenizer::to_upper(s);}
bool number(const std::string& s,double& v){return input::parse_double_strict(s,v)&&std::isfinite(v);}
void overlay(MeshSnowRow& a,const MeshSnowRow& b) {
    a.pack=b.pack;
    if(b.forest)a.forest=b.forest;
    if(b.drift)a.drift=b.drift;
    if(b.ground_heat)a.ground_heat=b.ground_heat;
    if(b.extinction_depth)a.extinction_depth=b.extinction_depth;
    if(b.slope)a.slope=b.slope;
    if(b.aspect)a.aspect=b.aspect;
}
}
std::string setMeshSnowOption(MeshSnowConfig& config,const std::vector<std::string>& tokens) {
    if(tokens.size()!=2)return "Mesh snow option requires one value.";
    if(upper(tokens[0])=="SNOWMELT") {
        const auto value=upper(tokens[1]);
        if(value=="AUTO")config.mode=MeshSnowConfig::Mode::Auto;
        else if(value=="YES")config.mode=MeshSnowConfig::Mode::Yes;
        else if(value=="NO")config.mode=MeshSnowConfig::Mode::No;
        else return "Mesh SNOWMELT must be AUTO, YES or NO.";
    } else if(upper(tokens[0])=="SNOW_STEP") {
        if(upper(tokens[1])=="DEFAULT")config.step=0;
        else {
            SnowConfig parsed;
            if(auto error=setSnowTemperature(parsed,tokens);!error.empty())return error;
            config.step=parsed.step;
        }
    } else return "Unknown mesh snow option: "+tokens[0];
    return {};
}
std::string setMeshSnowRow(MeshSnowConfig& config,const std::vector<std::string>& tokens,bool cell) {
    std::size_t at=0;
    if(!tokens.empty()&&upper(tokens[0])==(cell?"CELL":"TAG"))++at;
    if(tokens.size()<at+3)return "Mesh snow row requires a cell/tag and SNOWPACK name.";
    int key=-1;const std::string tag=tokens[at];
    if(cell) {
        double value=0;
        if(!number(tokens[at],value)||value!=std::floor(value)||value<1||value>std::numeric_limits<int>::max())
            return "Mesh snow cell must be a positive integer.";
        key=static_cast<int>(value)-1;
    } else if(tag.empty()||tag.find_first_of("\r\n\"")!=std::string::npos)return "Invalid mesh snow tag.";
    if(upper(tokens[++at])!="SNOWPACK")return "Expected SNOWPACK in mesh snow assignment.";
    MeshSnowRow row;row.pack=tokens[++at];++at;
    if(row.pack.empty()||row.pack.find_first_of("\r\n\"")!=std::string::npos)return "Invalid mesh snowpack name.";
    if(upper(row.pack)=="NONE")row.pack="NONE";
    std::set<std::string> seen;
    while(at<tokens.size()) {
        const auto name=upper(tokens[at++]);double value=0;
        if(at>=tokens.size()||!number(tokens[at++],value)||!seen.insert(name).second)
            return "Mesh snow overrides require unique keys and finite values.";
        if(name=="FC"&&value>=0&&value<=1)row.forest=value;
        else if(name=="F"&&value>=0)row.drift=value;
        else if(name=="QG")row.ground_heat=value;
        else if(name=="AEP"&&value>0)row.extinction_depth=value;
        else if(name=="SLOPE"&&value>=0&&value<90)row.slope=value;
        else if(name=="ASPECT"&&value>=0&&value<=360)row.aspect=value;
        else return "Invalid mesh snow override: "+name;
    }
    if(row.slope.has_value()!=row.aspect.has_value())return "Supply both SLOPE and ASPECT, or use cell geometry.";
    if(cell)config.cells[key]=std::move(row);else config.defaults[tag]=std::move(row);return {};
}
std::vector<std::string> meshSnowOptionRows(const MeshSnowConfig& config) {
    std::vector<std::string> rows;
    if(config.mode!=MeshSnowConfig::Mode::Auto)rows.push_back(config.mode==MeshSnowConfig::Mode::Yes?"SNOWMELT YES":"SNOWMELT NO");
    if(config.step>0){std::ostringstream s;s<<"SNOW_STEP "<<std::setprecision(17)<<config.step;rows.push_back(s.str());}
    return rows;
}
std::vector<std::string> meshSnowRows(const MeshSnowConfig& config,bool cell) {
    std::vector<std::string> rows;
    const auto append=[&](const std::string& prefix,const MeshSnowRow& row) {
        std::ostringstream s;s<<std::setprecision(17)<<prefix;
        s<<" SNOWPACK \""<<row.pack<<'"';
        const auto field=[&](const char* name,const std::optional<double>& value){if(value)s<<' '<<name<<' '<<*value;};
        field("FC",row.forest);field("F",row.drift);field("QG",row.ground_heat);field("AEP",row.extinction_depth);
        field("SLOPE",row.slope);field("ASPECT",row.aspect);rows.push_back(s.str());
    };
    if(cell)for(const auto& [key,row]:config.cells)append("CELL "+std::to_string(key+1),row);
    else for(const auto& [key,row]:config.defaults)append("\""+key+"\"",row);
    return rows;
}
MeshSnowRow resolveMeshSnowRow(const MeshSnowConfig& config,int cell,const std::string& tag) {
    MeshSnowRow row;
    if(auto it=config.defaults.find("*");it!=config.defaults.end())overlay(row,it->second);
    if(auto it=config.defaults.find(tag);it!=config.defaults.end())overlay(row,it->second);
    if(auto it=config.cells.find(cell);it!=config.cells.end())overlay(row,it->second);
    return row;
}
} // namespace openswmm::surface
