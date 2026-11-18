#pragma once
#include <cstring>
#include <functional>
#include <string_view>
struct ProfileNameHash {
    using is_transparent=void;
    size_t operator()(std::string_view name)const noexcept{return std::hash<std::string_view>{}(name);}
};
template<class Map> auto FindBottleProfile(Map& profiles,const char* name)->typename Map::mapped_type* {
    // Supported runtime meshes use this exact tag prefix. Most engine draws
    // reject on their first character without allocation, length scan or hash.
    if(!name||name[0]!='D'||std::strncmp(name,"DBL_SSE_",8)!=0)return nullptr;
    auto it=profiles.find(std::string_view{name});return it==profiles.end()?nullptr:&it->second;
}
