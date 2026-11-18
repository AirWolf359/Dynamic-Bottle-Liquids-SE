    if(!detail||s.foamStrength<=0||s.intensity<=0||s.fill<=0||s.fill>=1)m.surfaceBubbles.clear();
    // Pop expired cells before neighbour separation. Survivors use only the
    // original surface flow, rim migration and contact relaxation below.
    if(dt>0)std::erase_if(m.surfaceBubbles,[&](const auto& p){return p.age+dt>=SurfaceLifetime(s,p);});
    // Bounded tangential relaxation distributes foam without vertical stacking.
    // Four small iterations, at most 64*63/2 pairs each; no per-pixel neighbours.
    if(dt>0)for(int iteration=0;iteration<4;iteration++){
        for(size_t a=0;a<m.surfaceBubbles.size();a++)for(size_t b=a+1;b<m.surfaceBubbles.size();b++){
            auto& x=m.surfaceBubbles[a];auto& y=m.surfaceBubbles[b];
            Vec d=y.position-x.position;d=d-normal*Dot(d,normal);float squaredDistance=Dot(d,d);
            float target=(x.radius+y.radius)*1.02f;
            if(SurfacePairSeparated(squaredDistance,target))continue;
            float length=std::sqrt(squaredDistance);
            if(length>=target)continue;
            if(length<.00001f){
                float angle=float(a*37+b*17)*2.399963f;Vec seed{std::cos(angle),std::sin(angle),.37f};
                d=Normalize(seed-normal*Dot(seed,normal));
                if(Length(d)<.1f)d=Normalize(Vec{normal.z,0,-normal.x});
            }else d=d*(1/length);
            Vec correction=d*((target-length)*.5f*std::min(dt*30.f,1.f));
            x.position=x.position-correction;y.position=y.position+correction;
        }
    }
    int surfaceRingHint=1,clearanceRingHint=1;
    auto surfacePoint=[&](Vec q,float radius){
        for(int i=0;i<6;i++){
            float gap=height-Dot(q,normal)+Wave(q,m.time*s.waveTimeScale,primaryWave,secondaryWave,s.waveFrequency,mediumWave,c)+MeniscusHeightHinted(c,q,surfaceRingHint);
            Vec next=q+normal*(gap-radius*.65f);
            // An exact fixed point cannot change in the remaining iterations.
            // No tolerance or reduced iteration budget: preserve Test71 output.
            if(std::memcmp(&next,&q,sizeof(Vec))==0)return next;
            q=next;
        }
        return q;
    };
    auto clearance=[&](Vec q,float extent){
        return std::min({RadiusHinted(c,q.z-extent,clearanceRingHint),RadiusHinted(c,q.z,clearanceRingHint),RadiusHinted(c,q.z+extent,clearanceRingHint)})-CrossSection(c,q.x,q.y)-extent-.002f;
    };
    // Find an interior surface anchor from the authored cavity, including horizontal
    // bottles where intersecting the bottle axis with the plane is ill-conditioned.
    Vec anchor{};float best=-1e30f;
    if(!m.surfaceBubbles.empty())for(int i=1;i<c.ringCount;i++){
        Vec q=surfacePoint({0,0,(c.rings[i-1].x+c.rings[i].x)*.5f},0);
        float room=clearance(q,.01f);
        if(room>best){best=room;anchor=q;}
    }
    float lastCenterRadius=-1;Vec lastCenter{};
    for(size_t index=0;index<m.surfaceBubbles.size();index++){
        auto& p=m.surfaceBubbles[index];p.age+=dt;
        const float extent=p.radius*1.3f;
        // Identical consecutive radii have an identical anchor projection.
        Vec center;
        if(p.radius==lastCenterRadius)center=lastCenter;
        else {center=surfacePoint(anchor,p.radius);lastCenter=center;lastCenterRadius=p.radius;}
        Vec outward=p.position-center;outward=outward-normal*Dot(outward,normal);
        if(Length(outward)<.001f){
            float angle=float(index)*2.399963f;Vec seed{std::cos(angle),std::sin(angle),.37f};
            outward=seed-normal*Dot(seed,normal);
            if(Length(outward)<.1f)outward={normal.z,0,-normal.x};
        }
        outward=Normalize(outward);
        // The target is just inside the actual tilted liquid/cavity intersection.
        float range=2*(c.rings[c.ringCount-1].x-c.rings[0].x+c.rings[0].z);
        if(clearance(center,extent)<0){++m.foamFitRejected;p.age=SurfaceLifetime(s,p);continue;}
        float lo=0,hi=range;Vec edge{};bool foundEdge=false;
        for(int step=0;step<12;step++){
            float mid=(lo+hi)*.5f;
            Vec point=surfacePoint(center+outward*mid,p.radius);
            if(clearance(point,extent)>=0){lo=mid;edge=point;foundEdge=true;}else hi=mid;
        }
        // The final lower-bound point was already evaluated by the search.
        if(!foundEdge)edge=surfacePoint(center+outward*lo,p.radius);
        Vec drift=bubbleFlow.Current(p.position);drift=drift-normal*Dot(drift,normal);
        // Keep only along-rim current near the wall; no independent perpetual spin.
        float nearEdge=std::clamp(Length(p.position-center)/std::max(lo,.001f),0.f,1.f);
        drift=drift-outward*(Dot(drift,outward)*nearEdge);
        Vec candidate=surfacePoint(p.position+(edge-p.position)*(1-std::exp(-dt*1.8f))+drift*(dt*.25f),p.radius);
        if(clearance(candidate,extent)<0){
            // Constrain contact rather than killing foam at the wall.
            float a=0,b=1;Vec fitted{};bool found=false;
            for(int step=0;step<10;step++){
                float t=(a+b)*.5f;Vec q=surfacePoint(center+(candidate-center)*t,p.radius);
                if(clearance(q,extent)>=0){a=t;fitted=q;found=true;}else b=t;
            }
            candidate=found?fitted:surfacePoint(center+(candidate-center)*a,p.radius);
        }
        p.position=candidate;
        p.visibleRadius=p.radius; // Keep the cell full-sized until its membrane bursts.
    }
    std::erase_if(m.surfaceBubbles,[&](const auto& p){return p.age>=SurfaceLifetime(s,p);});
