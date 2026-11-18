// Test70 reference, linked only into the offline CPU probe.
SurfaceSection MeasureSurfaceReference(const Settings& s,Vec normal,float fill,Vec previousU){
    SurfaceSection out;out.normal=Normalize(normal);out.u=previousU-out.normal*Dot(previousU,out.normal);
    if(Length(out.u)<.05f){Vec axis=std::abs(out.normal.x)<.8f?Vec{1,0,0}:Vec{0,1,0};out.u=axis-out.normal*Dot(axis,out.normal);}
    out.u=Normalize(out.u);out.v=Normalize(Cross(out.normal,out.u));
    std::vector<std::pair<float,float>> heights;heights.reserve(s.samples.size());double total=0;
    for(auto p:s.samples){heights.emplace_back(Dot({p.x,p.y,p.z},out.normal),p.w);total+=p.w;}
    std::sort(heights.begin(),heights.end());double target=total*std::clamp(fill,.001f,.999f),volume=0;float h=0;
    for(auto [height,w]:heights){h=height;volume+=w;if(volume>=target)break;}
    float x0=1e30f,x1=-1e30f,y0=x0,y1=x1;
    for(size_t i=0;i+1<s.cavityEdges.size();i+=2){auto a=s.cavityEdges[i],b=s.cavityEdges[i+1];float da=Dot(a,out.normal)-h,db=Dot(b,out.normal)-h;
        if((da<0&&db<0)||(da>0&&db>0)||std::abs(da-db)<1e-8f)continue;
        auto p=a+(b-a)*std::clamp(da/(da-db),0.f,1.f);float x=Dot(p,out.u),y=Dot(p,out.v);x0=std::min(x0,x);x1=std::max(x1,x);y0=std::min(y0,y);y1=std::max(y1,y);}
    if(x1<=x0||y1<=y0){out.center=out.normal*h;out.rx=out.ry=.01f;out.area=.0003f;out.depth=.01f;return out;}
    auto inside=[&](Vec p){return p.z>=s.base.rings[0].x&&p.z<=s.base.rings[s.base.ringCount-1].x&&CrossSection(s.base,p.x,p.y)<=Radius(s.base,p.z);};
    // Small 48x48 section quadrature, rebuilt only when the cached plane changes.
    constexpr int grid=48;float dx=(x1-x0)/grid,dy=(y1-y0)/grid;Vec sum{};int count=0;
    for(int y=0;y<grid;++y)for(int x=0;x<grid;++x){auto p=out.normal*h+out.u*(x0+(x+.5f)*dx)+out.v*(y0+(y+.5f)*dy);if(inside(p)){sum=sum+p;++count;}}
    out.center=count?sum*(1.f/count):out.normal*h+out.u*((x0+x1)*.5f)+out.v*((y0+y1)*.5f);
    out.rx=std::max((x1-x0)*.5f,.01f);out.ry=std::max((y1-y0)*.5f,.01f);out.area=std::max(count*dx*dy,.0003f);
    float extent=2*(s.base.rings[s.base.ringCount-1].x-s.base.rings[0].x+s.base.rings[0].z),lo=0,hi=0;
    for(int i=1;i<=128;++i){hi=extent*i/128;if(!inside(out.center-out.normal*hi))break;lo=hi;}
    for(int i=0;i<12;++i){float mid=(lo+hi)*.5f;if(inside(out.center-out.normal*mid))lo=mid;else hi=mid;}
    out.depth=std::max((lo+hi)*.5f,.01f);return out;
}
