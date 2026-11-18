#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dblconfig {
struct ProfileDocuments {
    nlohmann::json values=nlohmann::json::array();
    std::map<std::string,std::filesystem::path> source;
    std::vector<std::filesystem::path> files;
    size_t overrides=0;
};
inline std::string Lower(std::string value){
    for(auto& c:value)c=char(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
// Startup-only merge. Names match the case-sensitive NIF tags used by the renderer.
// Later files override only explicitly supplied fields. Arrays/objects are atomic.
inline ProfileDocuments ReadProfiles(const std::filesystem::path& base){
    ProfileDocuments result;result.files.push_back(base);
    const auto folder=base.parent_path()/"profiles.d";
    std::vector<std::filesystem::path> patches;
    if(std::filesystem::exists(folder))
        for(const auto& entry:std::filesystem::directory_iterator(folder))
            if(entry.is_regular_file()&&Lower(entry.path().extension().string())==".json")patches.push_back(entry.path());
    std::sort(patches.begin(),patches.end(),[](const auto& a,const auto& b){
        const auto x=Lower(a.filename().string()),y=Lower(b.filename().string());
        return x==y?a.filename().string()<b.filename().string():x<y;
    });
    result.files.insert(result.files.end(),patches.begin(),patches.end());
    std::map<std::string,size_t> indices;
    for(const auto& path:result.files){
        try{
            std::ifstream input(path);if(!input)throw std::runtime_error("Cannot open file");
            auto rows=nlohmann::json::parse(input);
            if(!rows.is_array())throw std::runtime_error("Expected an array of named profile objects");
            std::set<std::string> seen;
            for(const auto& row:rows){
                if(!row.is_object()||!row.contains("name")||!row.at("name").is_string())throw std::runtime_error("Profile requires a string name");
                auto name=row.at("name").get<std::string>();
                if(!name.starts_with("DBL_SSE_")||name.size()==8)throw std::runtime_error("Invalid runtime profile tag: "+name);
                if(!seen.insert(name).second)throw std::runtime_error("Duplicate profile in one file: "+name);
                for(auto it=row.begin();it!=row.end();++it)if(it.value().is_null())throw std::runtime_error(name+": null is not a setting override ("+it.key()+")");
                if(auto found=indices.find(name);found!=indices.end()){
                    result.values[found->second].update(row);++result.overrides;
                }else{indices.emplace(name,result.values.size());result.values.push_back(row);}
                result.source[name]=path;
            }
        }catch(const std::exception& e){throw std::runtime_error(path.string()+": "+e.what());}
    }
    return result;
}
}
