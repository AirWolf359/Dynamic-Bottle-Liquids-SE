// Test50: numerical sections of the real profile/polygon cavity, never the
// preview ellipsoid. Shared edge geometry; per-instance fill/orientation cache.
static Vec Cross(Vec a,Vec b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
void BuildCavityEdges(Settings& s){
    s.cavityEdges.clear();s.cavitySections.clear();constexpr int sides=128;
    auto point=[&](int ring,int angle){float a=angle*6.283185307f/sides;auto r=s.base.rings[ring];float radius=r.y*AngularScale(s.base,a);return Vec{radius*std::cos(a),radius*std::sin(a),r.x};};
    auto edge=[&](Vec a,Vec b){s.cavityEdges.push_back(a);s.cavityEdges.push_back(b);};
    for(int r=1;r<s.base.ringCount;++r){
        CavitySection section;section.begin=s.cavityEdges.size();section.low={1e30f,1e30f,1e30f};section.high={-1e30f,-1e30f,-1e30f};
        for(int a=0;a<sides;++a){auto p=point(r-1,a),q=point(r-1,a+1),u=point(r,a),v=point(r,a+1);edge(p,q);edge(p,u);edge(p,v);edge(u,v);}
        section.end=s.cavityEdges.size();
        for(size_t i=section.begin;i<section.end;++i){auto p=s.cavityEdges[i];
            section.low={std::min(section.low.x,p.x),std::min(section.low.y,p.y),std::min(section.low.z,p.z)};
            section.high={std::max(section.high.x,p.x),std::max(section.high.y,p.y),std::max(section.high.z,p.z)};
        }
        s.cavitySections.push_back(section);
    }
}
// Conservative bounds of the actual cached endpoints, not an assumed circle.
// Keep contact/near-contact sections; only skip a provably separated box.
static bool SectionSeparated(const CavitySection& box,Vec n,float h){
    Vec a{n.x>=0?box.low.x:box.high.x,n.y>=0?box.low.y:box.high.y,n.z>=0?box.low.z:box.high.z};
    Vec b{n.x>=0?box.high.x:box.low.x,n.y>=0?box.high.y:box.low.y,n.z>=0?box.high.z:box.low.z};
    float magnitude=std::abs(n.x)*std::max(std::abs(box.low.x),std::abs(box.high.x))+
        std::abs(n.y)*std::max(std::abs(box.low.y),std::abs(box.high.y))+
        std::abs(n.z)*std::max(std::abs(box.low.z),std::abs(box.high.z))+std::abs(h);
    float margin=16*std::numeric_limits<float>::epsilon()*std::max(magnitude,1.f);
    return h<Dot(a,n)-margin||h>Dot(b,n)+margin;
}
#ifdef DBL_SURFACE_REFERENCE
bool ValidateSectionBounds(const Settings& s){
    size_t end=0;
    for(const auto& box:s.cavitySections){if(box.begin!=end||box.end>s.cavityEdges.size()||box.end<box.begin)return false;end=box.end;}
    if(end!=s.cavityEdges.size())return false;
    for(int i=0;i<256;++i){float a=i*.137f;Vec n=Normalize({std::sin(a),std::cos(a*.73f),std::cos(a)});
        if(i%6==0)n={0,0,1};if(i%6==1)n={0,0,-1};if(i%6==2)n={1,0,0};
        for(const auto& box:s.cavitySections){float low=1e30f,high=-1e30f;
            for(size_t k=box.begin;k<box.end;++k){float h=Dot(s.cavityEdges[k],n);low=std::min(low,h);high=std::max(high,h);}
            for(float h:{low,high,(low+high)*.5f,std::nextafter(low,-INFINITY),std::nextafter(high,INFINITY),low-.001f,high+.001f,low-100.f,high+100.f}){
                if(!SectionSeparated(box,n,h))continue;
                bool negative=true,positive=true;
                for(size_t k=box.begin;k<box.end;++k){float d=Dot(s.cavityEdges[k],n)-h;negative&=d<0;positive&=d>0;}
                if(!negative&&!positive)return false;
            }
        }
    }
    return true;
}
#endif
SurfaceSection MeasureSurface(const Settings& s,Vec normal,float fill,Vec previousU){
    SurfaceSection out;out.normal=Normalize(normal);out.u=previousU-out.normal*Dot(previousU,out.normal);
    if(Length(out.u)<.05f){Vec axis=std::abs(out.normal.x)<.8f?Vec{1,0,0}:Vec{0,1,0};out.u=axis-out.normal*Dot(axis,out.normal);}
    out.u=Normalize(out.u);out.v=Normalize(Cross(out.normal,out.u));
    std::vector<std::pair<float,float>> heights;heights.reserve(s.samples.size());double total=0;
    float low=1e30f,high=-1e30f;
    for(auto p:s.samples){float height=Dot({p.x,p.y,p.z},out.normal);heights.emplace_back(height,p.w);total+=p.w;low=std::min(low,height);high=std::max(high,height);}
    double target=total*std::clamp(fill,.001f,.999f),volume=0;float h=0;
    auto selectedEnd=heights.end();float width=(high-low)/256;
    const double roundoff=4*std::numeric_limits<double>::epsilon()*heights.size()*total;
    if(width>0){
        std::array<double,256> weights{};
        auto binOf=[&](float height){return std::clamp(int((height-low)/width),0,255);};
        for(auto [height,w]:heights)weights[binOf(height)]+=w;
        int bin=0;for(;bin<255&&volume+weights[bin]<target;++bin)volume+=weights[bin];
        if(std::abs(volume-target)<=roundoff||std::abs(volume+weights[bin]-target)<=roundoff)volume=0;
        else selectedEnd=std::partition(heights.begin(),heights.end(),[&](auto p){return binOf(p.first)==bin;});
    }
    // Preserve the exact weighted order statistic, including equal-height ties.
    // Only the selected bin needs sorting; lower bins contribute fixed volume.
    std::sort(heights.begin(),selectedEnd);
    bool ambiguous=false;
    for(auto it=heights.begin();it!=selectedEnd;++it){auto [height,w]=*it;h=height;volume+=w;ambiguous|=std::abs(volume-target)<=roundoff;if(volume>=target)break;}
    // Ambiguous weighted thresholds retain the original full summation order.
    if(selectedEnd!=heights.end()&&ambiguous){
        std::sort(heights.begin(),heights.end());volume=0;
        for(auto [height,w]:heights){h=height;volume+=w;if(volume>=target)break;}
    }
    float x0=1e30f,x1=-1e30f,y0=x0,y1=x1;
    auto scan=[&](size_t begin,size_t end){
    for(size_t i=begin;i+1<end;i+=2){auto a=s.cavityEdges[i],b=s.cavityEdges[i+1];float da=Dot(a,out.normal)-h,db=Dot(b,out.normal)-h;
        if((da<0&&db<0)||(da>0&&db>0)||std::abs(da-db)<1e-8f)continue;
        auto p=a+(b-a)*std::clamp(da/(da-db),0.f,1.f);float x=Dot(p,out.u),y=Dot(p,out.v);x0=std::min(x0,x);x1=std::max(x1,x);y0=std::min(y0,y);y1=std::max(y1,y);}
    };
    if(s.cavitySections.empty())scan(0,s.cavityEdges.size());
    else for(const auto& section:s.cavitySections)if(!SectionSeparated(section,out.normal,h))scan(section.begin,section.end);

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
static void SectionFactors(const SurfaceSection& g,const SurfaceSection& ref,float kRadius,float dx,float dy,float& rate,float& decay){
    float radius=1/std::sqrt(dx*dx/(g.rx*g.rx)+dy*dy/(g.ry*g.ry));
    float reference=1/std::sqrt(dx*dx/(ref.rx*ref.rx)+dy*dy/(ref.ry*ref.ry));
    float width=std::max(radius/reference,.001f),depth=std::max(g.depth/ref.depth,.001f),k=kRadius/radius,k0=kRadius/reference;
    rate=std::sqrt(k*std::tanh(k*g.depth)/std::max(k0*std::tanh(k0*ref.depth),1e-8f));
    decay=std::clamp(std::pow(width,1.25f)*std::sqrt(depth/width),.1f,5.f);
}
static void UpdateTunedSection(const Settings& s,Motion& m,Vec normal,float dt){
    m.surfaceAge+=dt;normal=Normalize(normal);
    bool rebuild=!m.tunedSamples||m.tunedSamples->sampleData!=s.samples.data()||m.tunedSamples->wallData!=s.wetSamples.data()||m.tunedSamples->revision!=s.sampleRevision||m.sectionFill<0||std::abs(m.sectionFill-s.fill)>.0001f||
        (Length(normal-m.sectionNormal)>.015f&&(m.surfaceAge>=.1f||Length(normal-m.sectionNormal)>.25f));
    if(!rebuild){++m.sectionHits;return;}
    m.section=MeasureSurface(s,normal,s.fill,m.section.u);m.sectionNormal=normal;m.sectionFill=s.fill;m.surfaceAge=0;++m.sectionBuilds;
    const float ks[]={2.6f/.8f,4.498f/.6f,6.11f/4};const float directions[3][3]={{.8f,.6f,0},{-.48f,.64f,.6f},{.36f,-.48f,.8f}};
    float r=std::sqrt(m.section.area/3.14159265f);
    for(int i=0;i<3;++i){float xy=std::hypot(directions[i][0],directions[i][1]);SectionFactors(m.section,s.referenceSurface,ks[i],directions[i][0]/xy,directions[i][1]/xy,m.modeRate[i],m.modeDecay[i]);
        Vec k=m.section.u*(directions[i][0]*ks[i]/m.section.rx)+m.section.v*(directions[i][1]*ks[i]/m.section.ry)+normal*(directions[i][2]*ks[i]/std::max(r,.01f));m.modeVectors[i]={k.x,k.y,k.z,0};}
    SectionFactors(m.section,s.referenceSurface,1.841f,.8669063f,-.4982839f,m.sloshRate,m.sloshDecay);
    m.heightGain=std::min(1.f,std::sqrt(m.section.area/s.referenceSurface.area))*std::min(1.f,m.section.depth/s.referenceSurface.depth);
    auto& cache=s.tunedSampleCache;
    std::erase_if(cache,[](const auto& entry){return entry.expired();});
    for(const auto& entry:cache)if(auto data=entry.lock()){
        if(data->sampleData==s.samples.data()&&data->wallData==s.wetSamples.data()&&data->revision==s.sampleRevision&&
            std::memcmp(data->modes.data(),m.modeVectors,sizeof(m.modeVectors))==0){m.tunedSamples=std::move(data);return;}
    }
    // Reuse capacity for a moving bottle when nobody else shares its old basis.
    if(!m.tunedSamples||m.tunedSamples.use_count()!=1)m.tunedSamples=std::make_shared<TunedSampleBasis>();
    auto& data=*m.tunedSamples;data.sampleData=s.samples.data();data.wallData=s.wetSamples.data();data.revision=s.sampleRevision;
    std::copy_n(m.modeVectors,3,data.modes.begin());
    data.projectionNormal=normal;data.volumeDots.clear();data.wallDots.clear();data.volumeDots.reserve(s.samples.size());data.wallDots.reserve(s.wetSamples.size());
    data.primarySecondary.clear();data.medium.clear();data.wallPhases.clear();
    data.primarySecondary.reserve(s.samples.size());data.medium.reserve(s.samples.size());data.wallPhases.reserve(s.wetSamples.size());
    for(auto p:s.samples){float q[3];for(int i=0;i<3;++i){auto k=m.modeVectors[i];q[i]=p.x*k.x+p.y*k.y+p.z*k.z;}
        data.volumeDots.push_back(Dot({p.x,p.y,p.z},normal));
        data.primarySecondary.push_back({std::sin(q[0]),std::cos(q[0]),std::sin(q[1]),std::cos(q[1])});data.medium.push_back({std::sin(q[2]),std::cos(q[2]),0,0});}
    for(auto p:s.wetSamples){float q[3];for(int i=0;i<3;++i){auto k=m.modeVectors[i];q[i]=p.x*k.x+p.y*k.y+p.z*k.z;}
        data.wallDots.push_back(Dot({p.x,p.y,p.z},normal));
        data.wallPhases.push_back({q[0],q[1],q[2],0});}
    bool listed=false;for(const auto& entry:cache)if(entry.lock()==m.tunedSamples){listed=true;break;}
    // Weak entries retain no geometry after the owning bottles disappear.
    if(!listed&&cache.size()<16)cache.push_back(m.tunedSamples);

}
float TunedWave(const Constants& c,Vec p,float a,float b,float medium){
    float v[3];for(int i=0;i<3;++i){auto q=c.tunedModes[i];v[i]=std::sin(p.x*q.x+p.y*q.y+p.z*q.z+q.w);}
    float result=a*v[0]+b*v[1]+medium*v[2];if(c.tunedInfo.y>0)result+=.35f*(std::abs(a)+std::abs(b))*v[0]*v[1];return result;
}
