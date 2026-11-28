// Skyrim VR draw interception. D3D11 switches the immediate context's draw
// entries between implementations, so every implementation is hooked as it
// appears; instanced bottle draws are handed to the renderer.
//
// DBL_VR_DIAGNOSTICS builds add the bring-up counters: every draw method on the
// immediate and deferred contexts, per-thread and per-pass counts, sampled
// callers and a report every 10 s. Normal builds hook only DrawIndexed and
// DrawIndexedInstanced and log just the first few unexpected bottle draws.
struct VRDiagnostics {
#ifdef DBL_VR_DIAGNOSTICS
    static constexpr bool counting=true;
#else
    static constexpr bool counting=false;
#endif
    enum Method{kDrawIndexed,kDraw,kDrawIndexedInstanced,kDrawInstanced,kIndexedIndirect,kInstancedIndirect,kMethods};
    static constexpr const char* methodNames[kMethods]{"DrawIndexed","Draw","DrawIndexedInstanced","DrawInstanced","DrawIndexedInstancedIndirect","DrawInstancedIndirect"};
    static constexpr unsigned detailLimit=8;
    struct ThreadCounts{DWORD id=GetCurrentThreadId();std::atomic<uint64_t> setups{0},draws{0},activeDraws{0};};
    static constexpr auto relaxed=std::memory_order_relaxed;

    bool enabled=false;
    std::mutex lock;
    std::vector<std::unique_ptr<ThreadCounts>> threads;
    std::unordered_map<UINT,uint64_t> instanceCounts;
    std::unordered_map<uint32_t,uint64_t> drawnPasses,undrawnPasses;
    std::atomic<uint64_t> passes{0},presents{0},gamePresents{0},matched{0},crossThreadMatched[2]{};
    std::atomic<uint64_t> activeDraws[2][kMethods]{};
    std::atomic<const Profile*> lastProfile{nullptr};std::atomic<DWORD> lastSetupThread{0};
    std::atomic<unsigned> detailed{0};
    // Test 2 saw only 2-3 draws per frame on the hooked functions. Track who
    // calls them and whether the context's function table changes after install.
    std::unordered_map<const void*,uint64_t> callers;
    static constexpr int slots[kMethods]{12,13,20,21,39,40};
    ID3D11DeviceContext* installedContext=nullptr;void** installedTable=nullptr;void* installedEntries[kMethods]{};
    double nextReport=0;
    static inline thread_local ThreadCounts* thread=nullptr;
    static inline thread_local bool drewThisPass=false;

    ThreadCounts& Thread(){
        if(!thread){auto counts=std::make_unique<ThreadCounts>();thread=counts.get();std::scoped_lock guard(lock);threads.push_back(std::move(counts));}
        return *thread;
    }
    static std::string ModuleOf(const void* address){
        HMODULE module{};
        if(!address||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,static_cast<LPCWSTR>(address),&module))
            return std::format("{}(no module)",address);
        wchar_t path[MAX_PATH]{};GetModuleFileNameW(module,path,MAX_PATH);
        return std::format("{}+0x{:X}",std::filesystem::path(path).filename().string(),reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(module));
    }
    static UINT BufferSize(ID3D11Buffer* b){if(!b)return 0;D3D11_BUFFER_DESC d{};b->GetDesc(&d);return d.ByteWidth;}
    // Logs the first few bottle-sized draws in full: the state the stereo
    // vertex shader, optics and native constant capture depend on.
    void Describe(ID3D11DeviceContext* ctx,Method method,UINT indexCount,UINT instanceCount,bool deferred){
        if(detailed.fetch_add(1,relaxed)>=detailLimit)return;
        ID3D11RenderTargetView* targets[8]{};ComPtr<ID3D11DepthStencilView> ds;ctx->OMGetRenderTargets(8,targets,&ds);
        std::string rts;
        for(int i=0;i<8;++i)if(targets[i]){
            D3D11_RENDER_TARGET_VIEW_DESC rv{};targets[i]->GetDesc(&rv);
            ComPtr<ID3D11Resource> r;targets[i]->GetResource(&r);ComPtr<ID3D11Texture2D> t;D3D11_TEXTURE2D_DESC td{};
            if(r&&SUCCEEDED(r.As(&t)))t->GetDesc(&td);
            rts+=std::format(" rt{}=fmt{}:{}x{}:array{}:dim{}",i,int(rv.Format),td.Width,td.Height,td.ArraySize,int(rv.ViewDimension));
            targets[i]->Release();
        }
        D3D11_VIEWPORT vps[16]{};UINT vpCount=16;ctx->RSGetViewports(&vpCount,vps);
        std::string viewports;for(UINT i=0;i<vpCount;++i)viewports+=std::format(" [{},{} {}x{}]",vps[i].TopLeftX,vps[i].TopLeftY,vps[i].Width,vps[i].Height);
        ID3D11Buffer* vsCB[14]{};ctx->VSGetConstantBuffers(0,14,vsCB);
        ID3D11Buffer* psCB[14]{};ctx->PSGetConstantBuffers(0,14,psCB);
        std::string vsSizes,psSizes;
        for(int i=0;i<14;++i){
            if(vsCB[i]){vsSizes+=std::format(" b{}={}",i,BufferSize(vsCB[i]));vsCB[i]->Release();}
            if(psCB[i]){psSizes+=std::format(" b{}={}",i,BufferSize(psCB[i]));psCB[i]->Release();}
        }
        ComPtr<ID3D11Buffer> vb;UINT stride=0,offset=0;ctx->IAGetVertexBuffers(0,1,&vb,&stride,&offset);
        ComPtr<ID3D11GeometryShader> gs;ctx->GSGetShader(&gs,nullptr,nullptr);
        SKSE::log::info("[DBL-VR-draw] {} context={} thread={} pass=0x{:08X} geometry={} indices={} expected={} instances={} stride={} geometry_shader={} viewports={}{} targets:{} vs_cb:{} ps_cb:{}",
            methodNames[method],deferred?"deferred":"immediate",GetCurrentThreadId(),active?active->passEnum:0u,
            active&&active->geometry?active->geometry->name.c_str():"?",indexCount,profile?profile->indices:0,instanceCount,stride,gs?"yes":"no",
            vpCount,viewports,rts,vsSizes,psSizes);
    }
    void Setup(RE::BSRenderPass* pass){
        if constexpr(!counting)return;
        passes.fetch_add(1,relaxed);Thread().setups.fetch_add(1,relaxed);drewThisPass=false;
        lastProfile.store(profile,relaxed);lastSetupThread.store(GetCurrentThreadId(),relaxed);
    }
    void Restore(RE::BSRenderPass* pass){
        if constexpr(!counting)return;
        std::scoped_lock guard(lock);++(drewThisPass?drawnPasses:undrawnPasses)[pass->passEnum];
    }
    void Observe(ID3D11DeviceContext* ctx,Method method,UINT count,UINT instances,const void* caller){
        if constexpr(!counting){
            // A non-instanced draw of bottle geometry during a bottle pass gets no
            // liquid (VR test 5 saw about 1 per 140). Describe the first few.
            if(method==kDrawIndexed&&active&&profile&&count==profile->indices)Describe(ctx,method,count,instances,false);
            return;
        }
        // Callers are sampled: once every draw is hooked, a lock per draw is too costly.
        if((Thread().draws.fetch_add(1,relaxed)&63)==0){std::scoped_lock guard(lock);++callers[caller];}
        const bool indexed=method==kDrawIndexed||method==kDrawIndexedInstanced;
        if(active&&profile){
            const bool deferred=ctx->GetType()==D3D11_DEVICE_CONTEXT_DEFERRED;
            drewThisPass=true;Thread().activeDraws.fetch_add(1,relaxed);activeDraws[deferred][method].fetch_add(1,relaxed);
            if(method==kDrawIndexedInstanced){std::scoped_lock guard(lock);++instanceCounts[instances];}
            if(indexed&&count==profile->indices){matched.fetch_add(1,relaxed);Describe(ctx,method,count,instances,deferred);}
        }else if(indexed){
            // A bottle-sized draw outside any pass on this thread: the pass may
            // have been set up on another thread.
            auto* last=lastProfile.load(relaxed);
            if(last&&count==last->indices&&GetCurrentThreadId()!=lastSetupThread.load(relaxed))
                crossThreadMatched[ctx->GetType()==D3D11_DEVICE_CONTEXT_DEFERRED].fetch_add(1,relaxed);
        }
    }
    static std::string Top(const std::unordered_map<uint32_t,uint64_t>& counts){
        std::vector<std::pair<uint32_t,uint64_t>> sorted(counts.begin(),counts.end());
        std::ranges::sort(sorted,[](auto& a,auto& b){return a.second>b.second;});
        std::string text;for(size_t i=0;i<sorted.size()&&i<12;++i)text+=std::format(" 0x{:08X}x{}",sorted[i].first,sorted[i].second);
        return text.empty()?" none":text;
    }
    void Present(bool game){
        HookCurrentTable();
        if constexpr(!counting)return;
        presents.fetch_add(1,relaxed);if(game)gamePresents.fetch_add(1,relaxed);
        const double t=now();if(t<nextReport)return;nextReport=t+10;
        std::string methods;
        for(int d=0;d<2;++d)for(int m=0;m<kMethods;++m)if(auto n=activeDraws[d][m].load(relaxed))methods+=std::format(" {}/{}={}",d?"deferred":"immediate",methodNames[m],n);
        std::scoped_lock guard(lock);
        std::string counts;for(const auto& [n,c]:instanceCounts)counts+=std::format(" {}x{}",n,c);
        std::string threadText;
        for(const auto& c:threads)if(auto s=c->setups.load(relaxed),a=c->activeDraws.load(relaxed),d=c->draws.load(relaxed);s||a||d)
            threadText+=std::format(" [{} setups={} draws={} during_pass={}]",c->id,s,d,a);
        SKSE::log::info("[DBL-VR] presents={} game_presents={} bottle_passes={} matched_draws={} cross_thread_matched immediate={} deferred={}",
            presents.load(relaxed),gamePresents.load(relaxed),passes.load(relaxed),matched.load(relaxed),crossThreadMatched[0].load(relaxed),crossThreadMatched[1].load(relaxed));
        SKSE::log::info("[DBL-VR]   draws during a pass:{} instance_counts:{}",methods.empty()?" none":methods,counts.empty()?" none":counts);
        SKSE::log::info("[DBL-VR]   passes that drew:{}",Top(drawnPasses));
        SKSE::log::info("[DBL-VR]   passes that did not draw:{}",Top(undrawnPasses));
        SKSE::log::info("[DBL-VR]   threads:{}",threadText.empty()?" none":threadText);
        std::vector<std::pair<const void*,uint64_t>> sortedCallers(callers.begin(),callers.end());
        std::ranges::sort(sortedCallers,[](auto& a,auto& b){return a.second>b.second;});
        std::string callerText;for(size_t i=0;i<sortedCallers.size()&&i<8;++i)callerText+=std::format(" [{} x{}]",ModuleOf(sortedCallers[i].first),sortedCallers[i].second);
        SKSE::log::info("[DBL-VR]   callers of hooked draws (1 in 64 per thread):{}",callerText.empty()?" none":callerText);
        ReportTable();
    }
    // Compares the renderer's context and its draw entries with what was hooked.
    void ReportTable(){
        auto* data=RE::BSGraphics::Renderer::GetRendererData();
        auto* context=data?reinterpret_cast<ID3D11DeviceContext*>(data->context):nullptr;
        if(!context){SKSE::log::info("[DBL-VR]   context: unavailable");return;}
        auto** table=*reinterpret_cast<void***>(context);
        std::string changes;
        for(int m=0;m<kMethods;++m)if(table[slots[m]]!=installedEntries[m])
            changes+=std::format(" {}: {} -> {}",methodNames[m],ModuleOf(installedEntries[m]),ModuleOf(table[slots[m]]));
        SKSE::log::info("[DBL-VR]   context {} (installed {}), table {} (installed {}), DrawIndexedInstanced entry {}; changed entries:{}",
            static_cast<const void*>(context),static_cast<const void*>(installedContext),ModuleOf(table),ModuleOf(installedTable),
            ModuleOf(table[slots[kDrawIndexedInstanced]]),changes.empty()?" none":changes);
    }
    // D3D11 switches the immediate context's draw entries between several
    // implementations in d3d11.dll as pipeline state changes (test 3). Hook each
    // implementation the first time the renderer's table points at it.
    // DrawIndexed is left to upstream's RefreshIndexedDraw, which routes every
    // switched implementation through draw().
    static constexpr int variants=8; // 0: immediate at install, 1: deferred, 2+: found later
    std::unordered_set<void*> hooked;int nextVariant[kMethods]{2,2,2,2,2,2};
    void Hook(const char* kind,Method method,void* target,int variant);
    void HookCurrentTable(){
        auto* data=RE::BSGraphics::Renderer::GetRendererData();
        auto* context=data?reinterpret_cast<ID3D11DeviceContext*>(data->context):nullptr;if(!context)return;
        auto** table=*reinterpret_cast<void***>(context);
        for(int m=0;m<kMethods;++m){
            void* target=table[slots[m]];
            if(m==kDrawIndexed||(!counting&&m!=kDrawIndexedInstanced))continue;
            if(!target||hooked.contains(target))continue;
            if(nextVariant[m]>=variants){hooked.insert(target);SKSE::log::warn("[DBL-VR] {} entry {}: no detour slots left",methodNames[m],ModuleOf(target));continue;}
            Hook("immediate (switched)",Method(m),target,nextVariant[m]++);
        }
    }
    void Install(ID3D11Device* device,void** immediate);
} vrDiagnostics;

// One detour set per hooked implementation. DrawIndexed on the immediate context
// goes through draw() in Plugin.cpp; the other methods are hooked here.
template<int Variant> struct VRDrawHooks {
    using M=VRDiagnostics::Method;
    static inline void(STDMETHODCALLTYPE* drawIndexed)(ID3D11DeviceContext*,UINT,UINT,INT){};
    static inline void(STDMETHODCALLTYPE* drawPlain)(ID3D11DeviceContext*,UINT,UINT){};
    static inline void(STDMETHODCALLTYPE* drawIndexedInstanced)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT){};
    static inline void(STDMETHODCALLTYPE* drawInstanced)(ID3D11DeviceContext*,UINT,UINT,UINT,UINT){};
    static inline void(STDMETHODCALLTYPE* indexedIndirect)(ID3D11DeviceContext*,ID3D11Buffer*,UINT){};
    static inline void(STDMETHODCALLTYPE* instancedIndirect)(ID3D11DeviceContext*,ID3D11Buffer*,UINT){};
    static void STDMETHODCALLTYPE DrawIndexed(ID3D11DeviceContext* c,UINT n,UINT s,INT b){vrDiagnostics.Observe(c,M::kDrawIndexed,n,1,_ReturnAddress());drawIndexed(c,n,s,b);}
    static void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* c,UINT n,UINT s){vrDiagnostics.Observe(c,M::kDraw,n,1,_ReturnAddress());drawPlain(c,n,s);}
    // Immediate-context bottle draws go to the renderer; variant 1 is the deferred context.
    // DBL's own submissions pass straight through: some d3d11 implementations re-dispatch
    // through the context's current table, which would re-enter the renderer (VR test 6).
    static void STDMETHODCALLTYPE DrawIndexedInstanced(ID3D11DeviceContext* c,UINT n,UINT i,UINT s,INT b,UINT si){
        vrDiagnostics.Observe(c,M::kDrawIndexedInstanced,n,i,_ReturnAddress());
        if constexpr(Variant!=1)if(vrStereo&&!instancedCall){
            if(active&&profile){DrawBottleInstanced(c,n,i,s,b,si,drawIndexedInstanced);return;}
            // TMD's reflection mesh and shielded glass shells (DrawTmdInstanced).
            if(DrawTmdInstanced(c,n,i,s,b,si,drawIndexedInstanced))return;
            // A cork or fitting's draw: read its lighting for the bottle, then draw it as is.
            CaptureVRFittingDraw(c,n);
        }
        drawIndexedInstanced(c,n,i,s,b,si);
    }
    static void STDMETHODCALLTYPE DrawInstanced(ID3D11DeviceContext* c,UINT n,UINT i,UINT s,UINT si){vrDiagnostics.Observe(c,M::kDrawInstanced,n,i,_ReturnAddress());drawInstanced(c,n,i,s,si);}
    static void STDMETHODCALLTYPE IndexedIndirect(ID3D11DeviceContext* c,ID3D11Buffer* a,UINT o){vrDiagnostics.Observe(c,M::kIndexedIndirect,0,0,_ReturnAddress());indexedIndirect(c,a,o);}
    static void STDMETHODCALLTYPE InstancedIndirect(ID3D11DeviceContext* c,ID3D11Buffer* a,UINT o){vrDiagnostics.Observe(c,M::kInstancedIndirect,0,0,_ReturnAddress());instancedIndirect(c,a,o);}
    static std::pair<void*,void**> Slot(M method){
        switch(method){
        case M::kDrawIndexed:return {reinterpret_cast<void*>(&DrawIndexed),reinterpret_cast<void**>(&drawIndexed)};
        case M::kDraw:return {reinterpret_cast<void*>(&Draw),reinterpret_cast<void**>(&drawPlain)};
        case M::kDrawIndexedInstanced:return {reinterpret_cast<void*>(&DrawIndexedInstanced),reinterpret_cast<void**>(&drawIndexedInstanced)};
        case M::kDrawInstanced:return {reinterpret_cast<void*>(&DrawInstanced),reinterpret_cast<void**>(&drawInstanced)};
        case M::kIndexedIndirect:return {reinterpret_cast<void*>(&IndexedIndirect),reinterpret_cast<void**>(&indexedIndirect)};
        default:return {reinterpret_cast<void*>(&InstancedIndirect),reinterpret_cast<void**>(&instancedIndirect)};
        }
    }
};
template<int... V> std::pair<void*,void**> VRHookSlot(int variant,VRDiagnostics::Method method,std::integer_sequence<int,V...>){
    std::pair<void*,void**> slot{};((variant==V?(slot=VRDrawHooks<V>::Slot(method),0):0),...);return slot;
}

void VRDiagnostics::Hook(const char* kind,Method method,void* target,int variant){
    if(!target||!hooked.insert(target).second){SKSE::log::info("[DBL-VR] {} {}: shared with an existing hook",kind,methodNames[method]);return;}
    auto [detour,original]=VRHookSlot(variant,method,std::make_integer_sequence<int,variants>{});
    auto status=MH_CreateHook(target,detour,original);
    if(status==MH_OK)status=MH_EnableHook(target);
    SKSE::log::info("[DBL-VR] {} {} {}: {}",kind,methodNames[method],ModuleOf(target),MH_StatusToString(status));
}

void VRDiagnostics::Install(ID3D11Device* device,void** immediate){
    if(auto* data=RE::BSGraphics::Renderer::GetRendererData())installedContext=reinterpret_cast<ID3D11DeviceContext*>(data->context);
    installedTable=immediate;for(int m=0;m<kMethods;++m)installedEntries[m]=immediate[slots[m]];
    SKSE::log::info("[DBL-VR] context table {}, DrawIndexed entry {}, DrawIndexedInstanced entry {}",ModuleOf(immediate),ModuleOf(immediate[12]),ModuleOf(immediate[20]));
    hooked.insert(immediate[12]); // draw() already detours immediate DrawIndexed.
    for(int m=kDraw;m<kMethods;++m)if(counting||m==kDrawIndexedInstanced)Hook("immediate",Method(m),immediate[slots[m]],0);
    if constexpr(!counting)return;
    // Deferred contexts use separate implementations in d3d11.dll. The code
    // stays loaded after this probe context is released.
    ComPtr<ID3D11DeviceContext> probe;
    if(FAILED(device->CreateDeferredContext(0,&probe))||!probe){SKSE::log::info("[DBL-VR] deferred context unavailable; deferred draws not observed");return;}
    auto** deferred=*reinterpret_cast<void***>(probe.Get());
    for(int m=0;m<kMethods;++m)Hook("deferred",Method(m),deferred[slots[m]],1);
}
