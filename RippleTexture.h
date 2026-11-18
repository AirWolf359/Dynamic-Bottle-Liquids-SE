#pragma once
#include <vector>
#include <array>
#include <cmath>
// Deterministic, seamless slopes made once per device. No per-frame CPU noise.
struct RippleTexture {
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11SamplerState> sampler;
    void Create(ID3D11Device* device){
        constexpr int N=128;
        std::array<std::vector<int16_t>,8> mips;
        mips[0].resize(N*N*4);
        for(int field=0;field<2;++field){
        std::vector<float> h(N*N),temp(N*N);
        uint32_t seed=field?0x72ef13bu:0x52b17a3u;
        for(auto& v:h){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;v=float(seed&65535)/32767.5f-1;}
        // Periodic low-pass noise, then its gradient. Smooth slopes, no grain.
        for(int pass=0;pass<8;++pass){
            for(int y=0;y<N;++y)for(int x=0;x<N;++x)
                temp[y*N+x]=(h[y*N+(x+N-1)%N]+2*h[y*N+x]+h[y*N+(x+1)%N])*.25f;
            for(int y=0;y<N;++y)for(int x=0;x<N;++x)
                h[y*N+x]=(temp[((y+N-1)%N)*N+x]+2*temp[y*N+x]+temp[((y+1)%N)*N+x])*.25f;
        }

        for(int y=0;y<N;++y)for(int x=0;x<N;++x){
            float dx=(h[y*N+(x+1)%N]-h[y*N+(x+N-1)%N])*5;
            float dy=(h[((y+1)%N)*N+x]-h[((y+N-1)%N)*N+x])*5;
            mips[0][(y*N+x)*4+field*2]=int16_t(std::clamp(dx,-1.f,1.f)*32767);
            mips[0][(y*N+x)*4+field*2+1]=int16_t(std::clamp(dy,-1.f,1.f)*32767);
        }
        }
        std::array<D3D11_SUBRESOURCE_DATA,8> data{};
        for(int level=0,n=N;level<8;++level,n/=2){
            if(level){mips[level].resize(n*n*4);for(int y=0;y<n;++y)for(int x=0;x<n;++x)for(int c=0;c<4;++c){
                int sum=0;for(int j=0;j<2;++j)for(int i=0;i<2;++i)sum+=mips[level-1][((y*2+j)*n*2+x*2+i)*4+c];
                mips[level][(y*n+x)*4+c]=int16_t(sum/4);
            }}
            data[level]={mips[level].data(),UINT(n*8),0};
        }
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=N;d.MipLevels=8;d.ArraySize=1;d.Format=DXGI_FORMAT_R16G16B16A16_SNORM;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;check(device->CreateTexture2D(&d,data.data(),&texture));
        view.Reset();sampler.Reset();check(device->CreateShaderResourceView(texture.Get(),nullptr,&view));
        D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;s.MaxLOD=D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&s,&sampler));
    }
    // Restore Skyrim's slot even when the draw takes the optical-buffer path.
    struct Binding {
        ID3D11DeviceContext* context;
        ComPtr<ID3D11ShaderResourceView> oldView;ComPtr<ID3D11SamplerState> oldSampler;
        Binding(ID3D11DeviceContext* c,const RippleTexture& r):context(c){
            c->PSGetShaderResources(13,1,&oldView);c->PSGetSamplers(13,1,&oldSampler);
            auto* v=r.view.Get();auto* s=r.sampler.Get();c->PSSetShaderResources(13,1,&v);c->PSSetSamplers(13,1,&s);
        }
        ~Binding(){auto* v=oldView.Get();auto* s=oldSampler.Get();context->PSSetShaderResources(13,1,&v);context->PSSetSamplers(13,1,&s);}
        Binding(const Binding&)=delete;Binding& operator=(const Binding&)=delete;
    };
};
