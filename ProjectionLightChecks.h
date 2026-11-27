#pragma once
#include <cmath>

enum class ProjectionLights { Unknown, NoShadow, Shadow };

struct ProjectionLightSphere {
    float x{},y{},z{},radius{};
};

inline bool ValidProjectionLightSphere(const ProjectionLightSphere& sphere){
    return std::isfinite(sphere.x)&&std::isfinite(sphere.y)&&std::isfinite(sphere.z)&&
           std::isfinite(sphere.radius)&&sphere.radius>0.f;
}

inline bool ShadowLightMayReach(const ProjectionLightSphere& light,const ProjectionLightSphere& bottle){
    if(!ValidProjectionLightSphere(light)||!ValidProjectionLightSphere(bottle))return true;
    const double x=double(light.x)-bottle.x,y=double(light.y)-bottle.y,z=double(light.z)-bottle.z;
    const double range=double(light.radius)+bottle.radius;
    return x*x+y*y+z*z<=range*range;
}
