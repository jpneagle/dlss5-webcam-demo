#include "D3D12Renderer.h"
#include "Log.h"
#include <d3dcompiler.h>
#include <wincodec.h>
#include <ctime>
#include <string>
#include <algorithm>
#include <cstring>
#include <cmath>

using Microsoft::WRL::ComPtr;

static bool HR(HRESULT hr, const char* what) {
    if (FAILED(hr)) { LOG(what << " failed hr=0x" << std::hex << hr); return false; }
    return true;
}
static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{}; p.Type=type; p.CreationNodeMask=1; p.VisibleNodeMask=1; return p;
}
static D3D12_RESOURCE_DESC Tex2D(DXGI_FORMAT fmt,uint32_t w,uint32_t h,D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=w; d.Height=h;
    d.DepthOrArraySize=1; d.MipLevels=1; d.Format=fmt; d.SampleDesc={1,0}; d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags=flags; return d;
}
static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource=r;
    x.Transition.StateBefore=a; x.Transition.StateAfter=b; x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; return x;
}

D3D12Renderer::~D3D12Renderer() {
    WaitGPU();
    for (uint32_t i=0;i<FrameCount;++i) {
        if (m_upload[i] && m_uploadMapped[i]) m_upload[i]->Unmap(0,nullptr);
        if (m_guideUpload[i] && m_guideMapped[i]) m_guideUpload[i]->Unmap(0,nullptr);
        m_uploadMapped[i]=nullptr;
        m_guideMapped[i]=nullptr;
    }
    m_dlss.Shutdown();
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
}

bool D3D12Renderer::Initialize(HWND hwnd,uint32_t sourceW,uint32_t sourceH,uint32_t outputW,uint32_t outputH,uint32_t gridW,uint32_t gridH,NVSDK_NGX_PerfQuality_Value quality,bool tryDirectNR) {
    m_tryDirectNR=tryDirectNR;
    m_hwnd=hwnd; m_sourceW=sourceW; m_sourceH=sourceH; m_outputW=outputW; m_outputH=outputH; m_gridW=gridW; m_gridH=gridH; m_quality=quality;
    if(!m_gridW||!m_gridH)return false;
    if(!CreateDeviceAndSwapchain(hwnd) || !CreateHeapsAndBackbuffers() || !CreatePipelines()) return false;
    if(!InitializeDLSS()) {
        LOG("DLSS unavailable; using D3D12 scaler fallback.");
        m_renderW=std::max(1u,outputW*2u/3u); m_renderH=std::max(1u,outputH*2u/3u);
    }
    if(!CreateVideoResources()) return false;
    LOG("V11 guide contract: compact CPU optical-flow grid expanded on GPU into full R16G16_FLOAT MVs + R8 bias; depth is written directly into the same R32_TYPELESS/D32_FLOAT resource passed to NGX; temporal reset only on discontinuities.");
    return true;
}

bool D3D12Renderer::CreateDeviceAndSwapchain(HWND hwnd) {
    UINT ff=0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> dbg; if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); ff|=DXGI_CREATE_FACTORY_DEBUG; }
#endif
    if(!HR(CreateDXGIFactory2(ff,IID_PPV_ARGS(&m_factory)),"CreateDXGIFactory2")) return false;
    ComPtr<IDXGIAdapter1> fallback;
    for(UINT i=0;;++i){
        ComPtr<IDXGIAdapter1>a; if(m_factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&a))==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d); if(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if(FAILED(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_12_0,_uuidof(ID3D12Device),nullptr))) continue;
        if(!fallback) fallback=a; if(d.VendorId==0x10DE){m_adapter=a;break;}
    }
    if(!m_adapter)m_adapter=fallback; if(!m_adapter){LOG("No D3D12 hardware adapter.");return false;}
    DXGI_ADAPTER_DESC1 ad{};m_adapter->GetDesc1(&ad);LOG("D3D12 adapter vendor=0x"<<std::hex<<ad.VendorId<<" device=0x"<<ad.DeviceId);
    if(!HR(D3D12CreateDevice(m_adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&m_device)),"D3D12CreateDevice"))return false;
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(!HR(m_device->CreateCommandQueue(&q,IID_PPV_ARGS(&m_queue)),"CreateCommandQueue"))return false;
    for(uint32_t i=0;i<FrameCount;++i) {
        if(!HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&m_allocators[i])),"CreateCommandAllocator"))return false;
    }
    for(uint32_t i=0;i<FrameCount;++i) {
        if(!HR(m_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,m_allocators[i].Get(),nullptr,IID_PPV_ARGS(&m_cmds[i])),"CreateCommandList"))return false;
        m_cmds[i]->Close();
    }
    BOOL tearing=FALSE;if(SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing))))m_allowTearing=tearing==TRUE;
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=m_outputW;sd.Height=m_outputH;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc={1,0};sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount=FrameCount;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sd.Scaling=DXGI_SCALING_STRETCH;sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;sd.Flags=m_allowTearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
    ComPtr<IDXGISwapChain1>sc1;if(!HR(m_factory->CreateSwapChainForHwnd(m_queue.Get(),hwnd,&sd,nullptr,nullptr,&sc1),"CreateSwapChainForHwnd"))return false;
    m_factory->MakeWindowAssociation(hwnd,DXGI_MWA_NO_ALT_ENTER);sc1.As(&m_swapchain);
    if(m_swapchain) m_swapchain->SetMaximumFrameLatency(2);
    if(!HR(m_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&m_fence)),"CreateFence"))return false;
    m_fenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);return m_fenceEvent!=nullptr;
}

bool D3D12Renderer::CreateHeapsAndBackbuffers(){
    D3D12_DESCRIPTOR_HEAP_DESC rh{};rh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rh.NumDescriptors=FrameCount+3;
    if(!HR(m_device->CreateDescriptorHeap(&rh,IID_PPV_ARGS(&m_rtvHeap)),"Create RTV heap"))return false;m_rtvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for(uint32_t i=0;i<FrameCount;++i){if(!HR(m_swapchain->GetBuffer(i,IID_PPV_ARGS(&m_backbuffers[i])),"Get backbuffer"))return false;m_device->CreateRenderTargetView(m_backbuffers[i].Get(),nullptr,RTV(i));}
    D3D12_DESCRIPTOR_HEAP_DESC sh{};sh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;sh.NumDescriptors=8;sh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(!HR(m_device->CreateDescriptorHeap(&sh,IID_PPV_ARGS(&m_srvHeap)),"Create SRV heap"))return false;
    m_srvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC dh{};dh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;dh.NumDescriptors=1;
    if(!HR(m_device->CreateDescriptorHeap(&dh,IID_PPV_ARGS(&m_dsvHeap)),"Create DSV heap"))return false;
    m_dsvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return true;
}

bool D3D12Renderer::CreatePipelines(){
    const char* hlsl=R"(
Texture2D T:register(t0); Texture2D TRef:register(t1); Texture2D TMatte:register(t2); SamplerState S:register(s0);
cbuffer Params:register(b0){
    float2 JitterUV;
    float2 Misc;   // x = mirror
    float4 ColorA; // brightness, contrast, saturation, gamma
    float4 ColorB; // temperature, tint, reserved, reserved
    float4 MaskA;  // enabled, centerX, centerY, radiusX
    float4 MaskB;  // radiusY, feather, strength, segmentationEnabled
    float4 NeuralGain; // per-channel gain applied to the reconstructed image only
    float4 Crop;       // xy = uv scale, zw = uv offset; identity is (1,1,0,0)
}
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);V o;o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}
float3 SRGBToLinear(float3 c){float3 lo=c/12.92;float3 hi=pow(max((c+0.055)/1.055,0),2.4);return lerp(hi,lo,step(c,0.04045));}
float3 LinearToSRGB(float3 c){c=max(c,0);float3 lo=c*12.92;float3 hi=1.055*pow(c,1.0/2.4)-0.055;return saturate(lerp(hi,lo,step(c,0.0031308)));}
float4 PSConvert(V i):SV_Target{float3 c=T.SampleLevel(S,i.uv+JitterUV,0).rgb;return float4(SRGBToLinear(c),1);}
float3 ApplyVideoAdjustments(float3 c){
    float brightness=ColorA.x;
    float contrast=max(ColorA.y,0.0);
    float saturation=max(ColorA.z,0.0);
    float gamma=max(ColorA.w,0.05);
    float temperature=clamp(ColorB.x,-1.0,1.0);
    float tint=clamp(ColorB.y,-1.0,1.0);

    c=max(c,0.0);
    c*=exp2(brightness);
    c=(c-0.18)*contrast+0.18;
    float l=dot(c,float3(0.2126,0.7152,0.0722));
    c=lerp(l.xxx,c,saturation);
    c*=float3(1.0+0.12*temperature,1.0,1.0-0.12*temperature);
    c*=float3(1.0+0.05*tint,1.0-0.10*tint,1.0+0.05*tint);
    c=pow(max(c,0.0),1.0/gamma);
    return c;
}
// Mirroring is a presentation choice only: the DLSS inputs are never flipped, so
// motion vectors and history stay in capture space.
// Crop first, mirror second. The crop is centred, so the two commute and the
// mirrored view stays on the same region.
float2 ViewUV(float2 uv){
    uv=uv*Crop.xy+Crop.zw;
    return float2(Misc.x>0.5?1.0-uv.x:uv.x,uv.y);
}
float4 PSPresent(V i):SV_Target{float3 c=T.SampleLevel(S,ViewUV(i.uv),0).rgb*NeuralGain.rgb;c=ApplyVideoAdjustments(c);return float4(LinearToSRGB(c),1);}
// Where the neural result is allowed to replace the original. Screen space, so the
// oval stays put when the view is mirrored.
float OvalMask(float2 uv){
    float2 d=(uv-float2(MaskA.y,MaskA.z))/float2(max(MaskA.w,1e-4),max(MaskB.x,1e-4));
    float r=length(d);
    float feather=clamp(MaskB.y,0.01,0.99);
    return saturate(1.0-smoothstep(1.0-feather,1.0,r));
}
// The oval is a screen-space frame the user drags, so it is evaluated at the raw
// screen uv. The matte comes from the camera image, so it must follow the same uv
// as the color -- mirrored when the view is mirrored.
float CombinedMask(float2 screenUV,float2 cameraUV){
    float m=1.0;
    if(MaskA.x>0.5) m*=OvalMask(screenUV);
    if(MaskB.w>0.5) m*=saturate(TMatte.SampleLevel(S,cameraUV,0).r);
    return m*saturate(MaskB.z);
}
// final = lerp(original, neural, mask). T is the reconstructed output, TRef the
// linear color that was handed to NGX.
float4 PSComposite(V i):SV_Target{
    float2 uv=ViewUV(i.uv);
    float3 neural=T.SampleLevel(S,uv,0).rgb*NeuralGain.rgb;
    float3 orig=TRef.SampleLevel(S,uv,0).rgb;
    float m=CombinedMask(i.uv,uv);
    float3 c=lerp(orig,neural,m);
    c=ApplyVideoAdjustments(c);
    return float4(LinearToSRGB(c),1);
}
float4 PSMaskPreview(V i):SV_Target{float m=CombinedMask(i.uv,ViewUV(i.uv));return float4(m,m,m,1);}
float3 hsv2rgb(float3 c){float4 K=float4(1,2.0/3.0,1.0/3.0,3);float3 p=abs(frac(c.xxx+K.xyz)*6-K.www);return c.z*lerp(K.xxx,saturate(p-K.xxx),c.y);}
float4 PSMotion(V i):SV_Target{float2 m=T.SampleLevel(S,ViewUV(i.uv),0).rg;if(Misc.x>0.5)m.x=-m.x;float mag=length(m);float h=frac(atan2(-m.y,m.x)/6.2831853+1.0);float v=saturate(0.22+mag/24.0);float3 c=hsv2rgb(float3(h,saturate(mag/1.0),v));return float4(c,1);}
float4 PSDepth(V i):SV_Target{float d=saturate(T.SampleLevel(S,ViewUV(i.uv),0).r);d=pow(d,0.7);return float4(d,d,d,1);}
    // Depth comes directly from compact-guide B and is written through SV_Depth into
    // the exact typeless/D32 resource that NGX receives later in the frame.
    float PSWriteDepth(V i):SV_Depth{return saturate(T.SampleLevel(S,i.uv+JitterUV,0).b);}
    struct GuideOut{float2 mv:SV_Target0;float bias:SV_Target1;};
    GuideOut PSExpandGuides(V i){float4 g=T.SampleLevel(S,i.uv+JitterUV,0);GuideOut o;o.mv=g.xy;o.bias=g.w>=0.5?1.0:0.0;return o;}
)";
    UINT flags=D3DCOMPILE_OPTIMIZATION_LEVEL3;ComPtr<ID3DBlob>vs,convert,present,composite,maskPreview,motion,depth,depthWrite,expand,err;
    auto C=[&](const char*entry,const char*target,ComPtr<ID3DBlob>&out)->bool{err.Reset();HRESULT hr=D3DCompile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,entry,target,flags,0,&out,&err);if(FAILED(hr)){if(err)LOG((char*)err->GetBufferPointer());return false;}return true;};
    if(!C("VS","vs_5_1",vs)||!C("PSConvert","ps_5_1",convert)||!C("PSPresent","ps_5_1",present)||!C("PSComposite","ps_5_1",composite)||!C("PSMaskPreview","ps_5_1",maskPreview)||!C("PSMotion","ps_5_1",motion)||!C("PSDepth","ps_5_1",depth)||!C("PSWriteDepth","ps_5_1",depthWrite)||!C("PSExpandGuides","ps_5_1",expand))return false;
    D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;range.BaseShaderRegister=0;
    // t1 is a separate table so the existing single-texture passes keep binding
    // exactly one descriptor; only the mask composite reads the second slot.
    D3D12_DESCRIPTOR_RANGE rangeRef{};rangeRef.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;rangeRef.NumDescriptors=1;rangeRef.BaseShaderRegister=1;
    D3D12_DESCRIPTOR_RANGE rangeMatte{};rangeMatte.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;rangeMatte.NumDescriptors=1;rangeMatte.BaseShaderRegister=2;
    D3D12_ROOT_PARAMETER rp[4]{};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[0].DescriptorTable.NumDescriptorRanges=1;rp[0].DescriptorTable.pDescriptorRanges=&range;
    rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;rp[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[1].Constants.Num32BitValues=28;rp[1].Constants.ShaderRegister=0;
    rp[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[2].DescriptorTable.NumDescriptorRanges=1;rp[2].DescriptorTable.pDescriptorRanges=&rangeRef;
    rp[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[3].DescriptorTable.NumDescriptorRanges=1;rp[3].DescriptorTable.pDescriptorRanges=&rangeMatte;
    D3D12_STATIC_SAMPLER_DESC smp{};smp.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;smp.AddressU=smp.AddressV=smp.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;smp.ShaderRegister=0;smp.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;smp.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=4;rs.pParameters=rp;rs.NumStaticSamplers=1;rs.pStaticSamplers=&smp;rs.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob>sig;if(!HR(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err),"SerializeRootSignature"))return false;
    if(!HR(m_device->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&m_rootSig)),"CreateRootSignature"))return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=m_rootSig.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={convert->GetBufferPointer(),convert->GetBufferSize()};
    p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.BlendState.RenderTarget[1].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.BlendState.RenderTarget[2].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
    p.DepthStencilState.DepthEnable=FALSE;p.DepthStencilState.StencilEnable=FALSE;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.SampleDesc={1,0};
    p.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoConvert)),"Create convert PSO"))return false;
    p.PS={present->GetBufferPointer(),present->GetBufferSize()};
    p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoPresent)),"Create present PSO"))return false;
    p.PS={composite->GetBufferPointer(),composite->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoComposite)),"Create mask composite PSO"))return false;
    p.PS={maskPreview->GetBufferPointer(),maskPreview->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoMaskPreview)),"Create mask preview PSO"))return false;
    p.PS={motion->GetBufferPointer(),motion->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoMotionDebug)),"Create MV debug PSO"))return false;
    p.PS={depth->GetBufferPointer(),depth->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoDepthDebug)),"Create depth debug PSO"))return false;
    p.PS={expand->GetBufferPointer(),expand->GetBufferSize()};p.NumRenderTargets=2;p.RTVFormats[0]=DXGI_FORMAT_R16G16_FLOAT;p.RTVFormats[1]=DXGI_FORMAT_R8_UNORM;p.RTVFormats[2]=DXGI_FORMAT_UNKNOWN;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoExpandGuides)),"Create GPU guide expansion PSO"))return false;
    p.PS={depthWrite->GetBufferPointer(),depthWrite->GetBufferSize()};
    p.NumRenderTargets=0;p.RTVFormats[0]=DXGI_FORMAT_UNKNOWN;p.RTVFormats[1]=DXGI_FORMAT_UNKNOWN;p.RTVFormats[2]=DXGI_FORMAT_UNKNOWN;p.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    p.DepthStencilState.DepthEnable=TRUE;p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;p.DepthStencilState.StencilEnable=FALSE;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoDepthWrite)),"Create real depth-buffer PSO"))return false;
    return true;
}

bool D3D12Renderer::InitializeDLSS(){
    auto* cmd=m_cmds[0].Get();
    m_allocators[0]->Reset();cmd->Reset(m_allocators[0].Get(),nullptr);bool ok=m_dlss.Initialize(m_device.Get(),cmd,m_sourceW,m_sourceH,m_outputW,m_outputH,m_quality);
    if(ok){m_renderW=m_dlss.RenderWidth();m_renderH=m_dlss.RenderHeight();}
    // Neural Rendering runs after super sampling, so it works at output resolution.
    // Off by default: the runtime refuses direct initialisation, and probing it would
    // load the same DLL that ReShade/RenoDX is about to drive through the NGX core.
    if(m_tryDirectNR) m_nr.Initialize(m_device.Get(),m_outputW,m_outputH);
    else m_nr.SetUnavailableReason(L"direct path off (--nr-direct to probe); NR via ReShade/RenoDX");
    cmd->Close();ID3D12CommandList*l[]={cmd};m_queue->ExecuteCommandLists(1,l);WaitGPU();return ok;
}

bool D3D12Renderer::CreateUploadForTexture(const D3D12_RESOURCE_DESC&desc,ComPtr<ID3D12Resource>&upload,uint8_t*&mapped,D3D12_PLACED_SUBRESOURCE_FOOTPRINT&fp,uint32_t&rows,uint64_t&rowBytes,uint64_t&total,const char*name){
    m_device->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&rowBytes,&total);D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=total;b.Height=1;b.DepthOrArraySize=1;b.MipLevels=1;b.SampleDesc={1,0};b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto hp=HeapProps(D3D12_HEAP_TYPE_UPLOAD);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),name))return false;D3D12_RANGE r{0,0};return HR(upload->Map(0,&r,reinterpret_cast<void**>(&mapped)),"Map upload resource");
}

bool D3D12Renderer::EnsurePersonMaskResources(uint32_t width,uint32_t height){
    if(m_personMask&&m_personMaskW==width&&m_personMaskH==height)return true;
    if(!m_device||!width||!height)return false;
    WaitGPU();   // the old matte may still be referenced by an in-flight frame
    for(uint32_t i=0;i<FrameCount;++i){m_personMaskUpload[i].Reset();m_personMaskMapped[i]=nullptr;}
    m_personMask.Reset();

    auto hp=HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto desc=Tex2D(DXGI_FORMAT_R8_UNORM,width,height,D3D12_RESOURCE_FLAG_NONE);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_personMask)),"Create person matte"))return false;
    m_personMask->SetName(L"PersonMatte_R8");
    for(uint32_t i=0;i<FrameCount;++i){
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};uint32_t rows=0;uint64_t rowBytes=0,total=0;
        if(!CreateUploadForTexture(desc,m_personMaskUpload[i],m_personMaskMapped[i],fp,rows,rowBytes,total,"Create person matte upload"))return false;
        if(i==0){m_personMaskFootprint=fp;m_personMaskRows=rows;m_personMaskRowSize=rowBytes;m_personMaskBytes=total;}
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;srv.Format=DXGI_FORMAT_R8_UNORM;
    m_device->CreateShaderResourceView(m_personMask.Get(),&srv,SRVCPU(7));
    m_personMaskW=width;m_personMaskH=height;
    LOG("Person matte resource ready: R8_UNORM " << width << "x" << height);
    return true;
}

bool D3D12Renderer::UpdatePersonMask(const uint8_t*r8,uint32_t width,uint32_t height){
    if(!r8||!EnsurePersonMaskResources(width,height))return false;
    m_pendingPersonMask.assign(r8,r8+size_t(width)*height);
    m_personMaskDirty=true;
    return true;
}

bool D3D12Renderer::CreateVideoResources(){
    auto hp=HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto src=Tex2D(DXGI_FORMAT_B8G8R8A8_UNORM,m_sourceW,m_sourceH,D3D12_RESOURCE_FLAG_NONE);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&src,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_decodedTexture)),"Create decoded texture"))return false;
    m_decodedTexture->SetName(L"Video_Decoded_BGRA_sRGB");
    for(uint32_t i=0;i<FrameCount;++i) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; uint32_t rows=0; uint64_t rowBytes=0,total=0;
        if(!CreateUploadForTexture(src,m_upload[i],m_uploadMapped[i],fp,rows,rowBytes,total,"Create video upload"))return false;
        if(i==0){m_uploadFootprint=fp;m_numRows=rows;m_rowSize=rowBytes;m_uploadBytes=total;}
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
    srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;m_device->CreateShaderResourceView(m_decodedTexture.Get(),&srv,SRVCPU(0));

    D3D12_CLEAR_VALUE cv{};cv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;auto col=Tex2D(cv.Format,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&col,D3D12_RESOURCE_STATE_RENDER_TARGET,&cv,IID_PPV_ARGS(&m_dlssColor)),"Create DLSS color"))return false;m_dlssColor->SetName(L"DLSS_Color_Input_Linear_FP16");m_device->CreateRenderTargetView(m_dlssColor.Get(),nullptr,RTV(FrameCount));
    srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;m_device->CreateShaderResourceView(m_dlssColor.Get(),&srv,SRVCPU(4));

    auto mot=Tex2D(DXGI_FORMAT_R16G16_FLOAT,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&mot,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_motion)),"Create motion guide"))return false;
    m_motion->SetName(L"DLSS_MotionVectors_CurrentToPrevious_RG16F");srv.Format=DXGI_FORMAT_R16G16_FLOAT;m_device->CreateShaderResourceView(m_motion.Get(),&srv,SRVCPU(2));m_device->CreateRenderTargetView(m_motion.Get(),nullptr,RTV(FrameCount+1));

    // One depth resource, two views: D32_FLOAT DSV for real depth writes / ReShade
    // discovery and R32_FLOAT SRV for debug/NGX sampling. Passing this exact resource
    // to NGX avoids the old "R32 proxy + unrelated mirrored D32" ambiguity.
    D3D12_CLEAR_VALUE dcv{};dcv.Format=DXGI_FORMAT_D32_FLOAT;dcv.DepthStencil.Depth=1.0f;dcv.DepthStencil.Stencil=0;
    auto dep=Tex2D(DXGI_FORMAT_R32_TYPELESS,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&dep,D3D12_RESOURCE_STATE_DEPTH_WRITE,&dcv,IID_PPV_ARGS(&m_depth)),"Create unified DLSS depth"))return false;
    m_depth->SetName(L"DLSS_Depth_R32_TYPELESS_D32_DSV_R32_SRV");
    srv.Format=DXGI_FORMAT_R32_FLOAT;m_device->CreateShaderResourceView(m_depth.Get(),&srv,SRVCPU(3));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT;dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;m_device->CreateDepthStencilView(m_depth.Get(),&dsv,DSV());

    auto bias=Tex2D(DXGI_FORMAT_R8_UNORM,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bias,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_biasCurrent)),"Create BiasCurrentColor mask"))return false;
    m_biasCurrent->SetName(L"DLSS_BiasCurrentColor_Disocclusion_R8");srv.Format=DXGI_FORMAT_R8_UNORM;m_device->CreateShaderResourceView(m_biasCurrent.Get(),&srv,SRVCPU(5));m_device->CreateRenderTargetView(m_biasCurrent.Get(),nullptr,RTV(FrameCount+2));
    auto grid=Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT,m_gridW,m_gridH,D3D12_RESOURCE_FLAG_NONE);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&grid,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_guideGrid)),"Create compact temporal guide grid"))return false;
    m_guideGrid->SetName(L"DLSS_CompactGuideGrid_RGBA32F");
    for(uint32_t i=0;i<FrameCount;++i) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; uint32_t rows=0; uint64_t rowBytes=0,total=0;
        if(!CreateUploadForTexture(grid,m_guideUpload[i],m_guideMapped[i],fp,rows,rowBytes,total,"Create compact guide upload"))return false;
        if(i==0){m_guideFootprint=fp;m_guideRows=rows;m_guideRowSize=rowBytes;m_guideUploadBytes=total;}
    }
    srv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;m_device->CreateShaderResourceView(m_guideGrid.Get(),&srv,SRVCPU(6));
    auto out=Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,m_outputW,m_outputH,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&m_dlssOutput)),"Create DLSS output"))return false;
    m_dlssOutput->SetName(L"DLSS_Output_Linear_FP16_UAV");
    srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;m_device->CreateShaderResourceView(m_dlssOutput.Get(),&srv,SRVCPU(1));
    LOG("DLSS resource contract ready: Color=R16G16B16A16_FLOAT " << m_renderW << "x" << m_renderH
        << ", MV=R16G16_FLOAT " << m_renderW << "x" << m_renderH
        << ", Depth=R32_TYPELESS resource / D32_FLOAT DSV / R32_FLOAT SRV " << m_renderW << "x" << m_renderH
        << ", BiasCurrentColor=R8_UNORM " << m_renderW << "x" << m_renderH
        << ", Output=R16G16B16A16_FLOAT UAV " << m_outputW << "x" << m_outputH
        << ", CompactGrid=R32G32B32A32_FLOAT " << m_gridW << "x" << m_gridH << " -> GPU MV/bias expansion + direct SV_Depth write");
    return true;
}

void D3D12Renderer::CopyMappedRows(uint8_t*mapped,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT&fp,const void*src,size_t tight,uint32_t rows){const uint8_t*s=static_cast<const uint8_t*>(src);for(uint32_t y=0;y<rows;++y)memcpy(mapped+fp.Offset+size_t(fp.Footprint.RowPitch)*y,s+tight*y,tight);}

float D3D12Renderer::Halton(uint32_t index,uint32_t base){float f=1.0f,r=0.0f;while(index){f/=float(base);r+=f*float(index%base);index/=base;}return r;}

bool D3D12Renderer::RenderFrame(const uint8_t*bgra,size_t bytes,const float*guideGridRGBA32F,size_t guideBytes,uint32_t gridW,uint32_t gridH,bool temporalReset,float frameTimeMs){
    const size_t videoRow=size_t(m_sourceW)*4u,guideRow=size_t(m_gridW)*sizeof(float)*4u;
    if(!bgra||bytes<videoRow*m_sourceH||!guideGridRGBA32F||gridW!=m_gridW||gridH!=m_gridH||guideBytes<guideRow*m_gridH)return false;
    const uint32_t slot=m_frameSlot%FrameCount;
    if(!WaitForFrameSlot(slot)) return false;
    CopyMappedRows(m_uploadMapped[slot],m_uploadFootprint,bgra,videoRow,m_sourceH);
    CopyMappedRows(m_guideMapped[slot],m_guideFootprint,guideGridRGBA32F,guideRow,m_gridH);
    if(!HR(m_allocators[slot]->Reset(),"Reset frame allocator")) return false;
    auto* cmd=m_cmds[slot].Get();
    if(!HR(cmd->Reset(m_allocators[slot].Get(),nullptr),"Reset frame command list")) return false;
    ID3D12DescriptorHeap*heaps[]={m_srvHeap.Get()};cmd->SetDescriptorHeaps(1,heaps);

    if(!m_sourceInCopyDest)Barrier(cmd,m_decodedTexture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION d{};d.pResource=m_decodedTexture.Get();d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION s{};s.pResource=m_upload[slot].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;s.PlacedFootprint=m_uploadFootprint;cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);Barrier(cmd,m_decodedTexture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_sourceInCopyDest=false;

    if(!m_gridInCopyDest)Barrier(cmd,m_guideGrid.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    d.pResource=m_guideGrid.Get();s.pResource=m_guideUpload[slot].Get();s.PlacedFootprint=m_guideFootprint;cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);
    Barrier(cmd,m_guideGrid.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_gridInCopyDest=false;

    // The matte arrives from the segmentation thread at its own rate, so it is only
    // uploaded on the frames where a new one showed up.
    if(m_personMaskDirty&&m_personMask&&m_personMaskMapped[slot]){
        CopyMappedRows(m_personMaskMapped[slot],m_personMaskFootprint,m_pendingPersonMask.data(),size_t(m_personMaskW),m_personMaskH);
        if(!m_personMaskInCopyDest)Barrier(cmd,m_personMask.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        d.pResource=m_personMask.Get();s.pResource=m_personMaskUpload[slot].Get();s.PlacedFootprint=m_personMaskFootprint;cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);
        Barrier(cmd,m_personMask.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_personMaskInCopyDest=false;m_personMaskDirty=false;
    }

    // One temporal jitter sample drives BOTH the color reconstruction input and the
    // spatial lookup of all guide buffers.  The motion-vector VALUES themselves remain
    // unjittered (hence no MVJittered create flag), matching the standard DLSS contract.
    const float jitterX=Halton(uint32_t(m_framesPresented%1024)+1,2)-0.5f;
    const float jitterY=Halton(uint32_t(m_framesPresented%1024)+1,3)-0.5f;
    const float jitterUVX=jitterX/float(m_renderW), jitterUVY=jitterY/float(m_renderH);

    // GPU-expand the compact CPU optical-flow/mask analysis to exact DLSS input
    // resolution. Depth is deliberately NOT mirrored through a color RT anymore:
    // it is written directly into the same typeless depth resource that NGX receives.
    if(!m_guidesInRT){Barrier(cmd,m_motion.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);Barrier(cmd,m_biasCurrent.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);}m_guidesInRT=true;
    D3D12_VIEWPORT gvp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT gsc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&gvp);cmd->RSSetScissorRects(1,&gsc);
    D3D12_CPU_DESCRIPTOR_HANDLE grt[2]={RTV(FrameCount+1),RTV(FrameCount+2)};cmd->OMSetRenderTargets(2,grt,FALSE,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoExpandGuides.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(6));float guideParams[4]={jitterUVX,jitterUVY,0,0};cmd->SetGraphicsRoot32BitConstants(1,4,guideParams,0);cmd->DrawInstanced(3,1,0,0);
    Barrier(cmd,m_motion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);Barrier(cmd,m_biasCurrent.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);m_guidesInRT=false;

    // Populate the exact depth resource passed to NGX. The resource is R32_TYPELESS,
    // viewed as D32_FLOAT while writing and R32_FLOAT while sampling/debugging.
    if(!m_depthInWrite)Barrier(cmd,m_depth.Get(),DepthGuideReadState,D3D12_RESOURCE_STATE_DEPTH_WRITE);m_depthInWrite=true;
    D3D12_VIEWPORT dvp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT dsc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&dvp);cmd->RSSetScissorRects(1,&dsc);
    auto dsvh=DSV();cmd->OMSetRenderTargets(0,nullptr,FALSE,&dsvh);cmd->ClearDepthStencilView(dsvh,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoDepthWrite.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(6));cmd->SetGraphicsRoot32BitConstants(1,4,guideParams,0);cmd->DrawInstanced(3,1,0,0);
    Barrier(cmd,m_depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,DepthGuideReadState);m_depthInWrite=false;

    if(!m_colorInRT)Barrier(cmd,m_dlssColor.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);m_colorInRT=true;
    D3D12_VIEWPORT vp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT sc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&vp);cmd->RSSetScissorRects(1,&sc);
    auto crt=RTV(FrameCount);cmd->OMSetRenderTargets(1,&crt,FALSE,nullptr);const float black[4]={0,0,0,1};cmd->ClearRenderTargetView(crt,black,0,nullptr);cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoConvert.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(0));
    float params[4]={jitterUVX,jitterUVY,0,0};cmd->SetGraphicsRoot32BitConstants(1,4,params,0);cmd->DrawInstanced(3,1,0,0);Barrier(cmd,m_dlssColor.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);m_colorInRT=false;

    ++m_framesPresented;

    // Create/recreate the NGX feature on an open command list, submit that list,
    // and only then evaluate on a fresh list. This mirrors the robust game-style
    // NGX lifetime instead of relying on CreateFeature and EvaluateFeature being
    // accepted back-to-back before the creation commands have reached the GPU.
    bool needFeatureFlush = false;
    if (DLSSEnabled() && !m_dlss.FeatureCreated() && m_framesPresented >= 2) {
        // Intentionally allow one complete Present before the first NGX CreateFeature.
        // ReShade add-ons finish their swapchain/runtime initialization on that first frame;
        // creating on frame 2 makes the raw CreateFeature much harder for RenoDX to miss.
        needFeatureFlush = m_dlss.EnsureFeature(cmd);
        temporalReset = true;
        m_recreateRequested = false;
    // The automatic recreate existed so a late-arriving ReShade/RenoDX hook could
    // still capture a fresh CreateFeature. Deep Fried Chicken hooks from DllMain and
    // tears its whole neural graph down on every native recreate, so the automatic
    // one is pure churn. F6 still forces one on demand.
    } else if (DLSSEnabled() && m_recreateRequested) {
        needFeatureFlush = m_dlss.RecreateFeature(cmd);
        temporalReset = true;
        m_recreateRequested = false;
    }
    if (needFeatureFlush) {
        if (!HR(cmd->Close(), "Close command list after NGX CreateFeature")) return false;
        ID3D12CommandList* initLists[] = { cmd };
        m_queue->ExecuteCommandLists(1, initLists);
        WaitGPU();
        if (!HR(m_allocators[slot]->Reset(), "Reset allocator after NGX CreateFeature")) return false;
        if (!HR(cmd->Reset(m_allocators[slot].Get(), nullptr), "Reset command list after NGX CreateFeature")) return false;
        ID3D12DescriptorHeap* postCreateHeaps[] = { m_srvHeap.Get() };
        cmd->SetDescriptorHeaps(1, postCreateHeaps);
        LOG("NGX feature creation flushed before EvaluateFeature; temporal history reset.");
    }

    bool used=false;if(DLSSEnabled() && m_dlss.FeatureCreated()){
        if(!m_outputInUAV)Barrier(cmd,m_dlssOutput.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);m_outputInUAV=true;
        used=m_dlss.Evaluate(cmd,m_dlssColor.Get(),m_dlssOutput.Get(),m_depth.Get(),m_motion.Get(),m_biasCurrent.Get(),temporalReset,frameTimeMs,jitterX,jitterY);
        if(used){Barrier(cmd,m_dlssOutput.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_outputInUAV=false;}
    }

    // Feature-creation probe for the direct DLSS 5 path. Creating it needs a command
    // list, so it happens here rather than at initialisation.
    if(used) m_nr.EnsureFeature(cmd);

    m_lastDLSSUsed=used;
    uint32_t bi=m_swapchain->GetCurrentBackBufferIndex();Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);D3D12_VIEWPORT ovp{0,0,float(m_outputW),float(m_outputH),0,1};D3D12_RECT osc{0,0,LONG(m_outputW),LONG(m_outputH)};cmd->RSSetViewports(1,&ovp);cmd->RSSetScissorRects(1,&osc);auto brt=RTV(bi);cmd->OMSetRenderTargets(1,&brt,FALSE,nullptr);cmd->ClearRenderTargetView(brt,black,0,nullptr);
    RecordPresentPass(cmd,brt,used);
    Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    if(!HR(cmd->Close(),"Close frame command list")) return false;
    ID3D12CommandList*ls[]={cmd};m_queue->ExecuteCommandLists(1,ls);
    HRESULT phr=m_swapchain->Present(0,m_allowTearing?DXGI_PRESENT_ALLOW_TEARING:0);
    if(FAILED(phr)){LOG("Present failed hr=0x"<<std::hex<<phr);return false;}
    SignalFrameSlot(slot);
    m_frameSlot=(slot+1u)%FrameCount;
    return true;
}

namespace {

float HalfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) bits = sign;
        else {  // subnormal: renormalize
            uint32_t e = 127 - 15 + 1, m = mant;
            while (!(m & 0x400u)) { m <<= 1; --e; }
            bits = sign | (e << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

uint8_t LinearToSRGB8(float c) {
    c = c > 0.0f ? c : 0.0f;
    const float s = (c <= 0.0031308f) ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return uint8_t(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
}

bool WritePNG(const std::wstring& path, const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&fmt))) return false;
    if (FAILED(frame->WritePixels(h, w * 4, UINT(bgra.size()), const_cast<BYTE*>(bgra.data())))) return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

} // namespace

bool D3D12Renderer::SaveComparisonPNG(const std::wstring& directory, std::wstring& outMessage) {
    if (!m_device || !m_queue || !m_dlssOutput || !m_dlssColor) { outMessage = L"renderer not ready"; return false; }
    if (!m_lastDLSSUsed) { outMessage = L"no DLSS output yet"; return false; }

    enum class Pixels { HalfLinear, RGBA8, R8 };
    struct Target {
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES state;
        uint32_t w, h;
        Pixels pixels;
        const wchar_t* suffix;
    };
    const uint32_t backIndex = m_swapchain->GetCurrentBackBufferIndex();
    std::vector<Target> targets = {
        { m_dlssColor.Get(),  GuideReadState,                             m_renderW, m_renderH, Pixels::HalfLinear, L"_A_input" },
        { m_dlssOutput.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_outputW, m_outputH, Pixels::HalfLinear, L"_B_dlss"  },
        // What the viewer actually sees: wipe, mask composite and mirroring applied.
        { m_backbuffers[backIndex].Get(), D3D12_RESOURCE_STATE_PRESENT,   m_outputW, m_outputH, Pixels::RGBA8, L"_C_screen" },
    };
    // The matte belongs in the same set: judging a masked reconstruction means seeing
    // where the mask actually let it through.
    if (m_personMask)
        targets.push_back({ m_personMask.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                            m_personMaskW, m_personMaskH, Pixels::R8, L"_D_matte" });

    wchar_t stamp[32]{};
    {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        wcsftime(stamp, std::size(stamp), L"%Y%m%d_%H%M%S", &tm);
    }

    WaitGPU();
    bool allOk = true;
    for (const Target& target : targets) {
        const D3D12_RESOURCE_DESC desc = target.res->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0;
        UINT64 rowBytes = 0, total = 0;
        m_device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);

        ComPtr<ID3D12Resource> readback;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = total;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc = {1, 0};
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!HR(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&readback)), "Create readback buffer")) {
            outMessage = L"readback allocation failed";
            return false;
        }

        auto* cmd = m_cmds[0].Get();
        if (!HR(m_allocators[0]->Reset(), "Reset capture allocator")) return false;
        if (!HR(cmd->Reset(m_allocators[0].Get(), nullptr), "Reset capture command list")) return false;
        Barrier(cmd, target.res, target.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        src.pResource = target.res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(cmd, target.res, D3D12_RESOURCE_STATE_COPY_SOURCE, target.state);
        if (!HR(cmd->Close(), "Close capture command list")) return false;
        ID3D12CommandList* lists[] = { cmd };
        m_queue->ExecuteCommandLists(1, lists);
        WaitGPU();

        uint8_t* mapped = nullptr;
        D3D12_RANGE readRange{0, size_t(total)};
        if (!HR(readback->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "Map readback")) return false;

        std::vector<uint8_t> out(size_t(target.w) * target.h * 4);
        for (uint32_t y = 0; y < target.h; ++y) {
            const uint8_t* srcRow = mapped + size_t(fp.Footprint.RowPitch) * y;
            uint8_t* dstRow = out.data() + size_t(y) * target.w * 4;
            if (target.pixels == Pixels::HalfLinear) {
                const uint16_t* src16 = reinterpret_cast<const uint16_t*>(srcRow);
                for (uint32_t x = 0; x < target.w; ++x) {
                    const float r = HalfToFloat(src16[x * 4 + 0]);
                    const float g = HalfToFloat(src16[x * 4 + 1]);
                    const float b = HalfToFloat(src16[x * 4 + 2]);
                    dstRow[x * 4 + 0] = LinearToSRGB8(b);
                    dstRow[x * 4 + 1] = LinearToSRGB8(g);
                    dstRow[x * 4 + 2] = LinearToSRGB8(r);
                    dstRow[x * 4 + 3] = 255;
                }
            } else if (target.pixels == Pixels::RGBA8) {
                for (uint32_t x = 0; x < target.w; ++x) {   // RGBA8 -> BGRA8 for WIC
                    dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
                    dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                    dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
                    dstRow[x * 4 + 3] = 255;
                }
            } else {
                for (uint32_t x = 0; x < target.w; ++x) {   // R8 matte -> grayscale
                    const uint8_t v = srcRow[x];
                    dstRow[x * 4 + 0] = v;
                    dstRow[x * 4 + 1] = v;
                    dstRow[x * 4 + 2] = v;
                    dstRow[x * 4 + 3] = 255;
                }
            }
        }
        D3D12_RANGE noWrite{0, 0};
        readback->Unmap(0, &noWrite);

        const std::wstring path = directory + L"\\dlss5cam_" + stamp + target.suffix + L".png";
        if (WritePNG(path, out, target.w, target.h)) {
            LOG("Saved A/B capture: " << Narrow(path));
        } else {
            LOG("Failed to write A/B capture: " << Narrow(path));
            allOk = false;
        }
    }
    outMessage = allOk ? (std::wstring(L"saved dlss5cam_") + stamp + L"_A.._"
                          + std::to_wstring(targets.size()) + L" files") : L"capture failed";
    return allOk;
}

void D3D12Renderer::SetSplitPosition(float t){m_splitPos=std::clamp(t,0.02f,0.98f);}

// Shared by the live-frame path and the idle re-present path so both agree on
// debug views, the A/B wipe and mirroring.
void D3D12Renderer::RecordPresentPass(ID3D12GraphicsCommandList* cmd,
                                      D3D12_CPU_DESCRIPTOR_HANDLE rtv, bool dlssUsed){
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const bool applyColor=(m_debugView==DebugView::Final);
    const ColorSettings cs=applyColor?m_colorSettings:ColorSettings{};
    const float mirror=m_mirror?1.0f:0.0f;
    float params[28]={0,0,mirror,0,
                      cs.brightness,cs.contrast,cs.saturation,cs.gamma,
                      cs.temperature,cs.tint,0,0,
                      m_mask.enabled?1.0f:0.0f,m_mask.centerX,m_mask.centerY,m_mask.radiusX,
                      m_mask.radiusY,m_mask.feather,m_mask.strength,
                      (m_mask.segmentation&&m_personMask)?1.0f:0.0f,
                      m_neuralGain.r,m_neuralGain.g,m_neuralGain.b,0,
                      1.0f,1.0f,0.0f,0.0f};
    // The reference half of the wipe and the raw-input debug view must never be
    // tinted, or the A/B stops measuring what DLSS actually did.
    const bool neutralPass=(m_debugView==DebugView::Input);
    if(neutralPass){params[20]=params[21]=params[22]=1.0f;}
    cmd->SetGraphicsRoot32BitConstants(1,28,params,0);

    // DLSS inputs stay in the state NGX expects. Only the resource this pass samples
    // is temporarily made pixel-shader readable, and restored before the frame ends.
    ID3D12Resource* transitioned=nullptr;
    D3D12_RESOURCE_STATES transitionedBefore=GuideReadState;
    ID3D12PipelineState* pso=m_psoPresent.Get();
    uint32_t srvIndex=1;
    switch(m_debugView){
        case DebugView::MotionVectors:transitioned=m_motion.Get();pso=m_psoMotionDebug.Get();srvIndex=2;break;
        case DebugView::Depth:transitioned=m_depth.Get();transitionedBefore=DepthGuideReadState;pso=m_psoDepthDebug.Get();srvIndex=3;break;
        case DebugView::BiasMask:transitioned=m_biasCurrent.Get();pso=m_psoDepthDebug.Get();srvIndex=5;break;
        case DebugView::PersonMask:transitioned=m_dlssColor.Get();pso=m_psoMaskPreview.Get();srvIndex=4;break;
        case DebugView::Input:transitioned=m_dlssColor.Get();srvIndex=4;break;
        default:
            if(dlssUsed)srvIndex=1;
            else{transitioned=m_dlssColor.Get();srvIndex=4;}
            break;
    }

    const bool finalView=(m_debugView==DebugView::Final);
    const bool compare=(m_compare!=CompareMode::Off)&&finalView&&dlssUsed;
    const bool split=compare&&m_compare==CompareMode::Wipe;
    const bool sideBySide=compare&&m_compare==CompareMode::SideBySide;
    // Composite blends the reconstruction back over the original inside the oval;
    // without a mask the reconstruction covers the whole frame as before.
    const bool composite=(m_mask.enabled||(m_mask.segmentation&&m_personMask))&&finalView&&dlssUsed;
    if(split||composite)transitioned=m_dlssColor.Get();   // sampled as the reference

    if(transitioned)Barrier(cmd,transitioned,transitionedBefore,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // t1 always points at the pre-DLSS color so the composite PSO never samples a
    // stale descriptor, even on the passes that ignore it.
    cmd->SetGraphicsRootDescriptorTable(2,SRVGPU(4));
    // t2 always resolves to a real descriptor; slot 7 holds the matte once created.
    cmd->SetGraphicsRootDescriptorTable(3,SRVGPU(m_personMask?7u:4u));
    cmd->SetPipelineState(composite?m_psoComposite.Get():pso);

    const LONG w=LONG(m_outputW),h=LONG(m_outputH);
    // The reference draw must never carry the neural white balance.
    float refParams[28];
    memcpy(refParams,params,sizeof(params));
    refParams[20]=refParams[21]=refParams[22]=1.0f;
    const float line[4]={0.95f,0.95f,0.95f,1.0f};

    if(split){
        const LONG x=std::clamp(LONG(std::lround(double(w)*m_splitPos)),LONG(1),w-1);
        D3D12_RECT left{0,0,x,h};
        cmd->RSSetScissorRects(1,&left);
        cmd->SetPipelineState(m_psoPresent.Get());
        cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));
        cmd->SetGraphicsRoot32BitConstants(1,28,refParams,0);
        cmd->DrawInstanced(3,1,0,0);
        cmd->SetGraphicsRoot32BitConstants(1,28,params,0);
        D3D12_RECT right{x,0,w,h};
        cmd->RSSetScissorRects(1,&right);
        cmd->SetPipelineState(composite?m_psoComposite.Get():m_psoPresent.Get());
        cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(1));
        cmd->DrawInstanced(3,1,0,0);
        D3D12_RECT full{0,0,w,h};
        cmd->RSSetScissorRects(1,&full);
        D3D12_RECT divider{std::max(LONG(0),x-1),0,std::min(w,x+2),h};
        cmd->ClearRenderTargetView(rtv,line,1,&divider);
    }else if(sideBySide){
        // Each half is a full-height viewport showing a centred crop of the frame, so
        // the pair fills the window with no letterbox. A 16:9 source in a half-width
        // viewport keeps the middle (halfAspect / sourceAspect) of its width -- half of
        // it at 16:9, which is where a webcam subject sits anyway.
        const float halfW=float(w)*0.5f;
        const float sourceAspect=(h!=0)?float(w)/float(h):1.0f;
        const float halfAspect=(h!=0)?halfW/float(h):1.0f;
        const float cropX=std::clamp(halfAspect/std::max(sourceAspect,0.0001f),0.05f,1.0f);
        D3D12_VIEWPORT vpLeft{0.0f,0.0f,halfW,float(h),0.0f,1.0f};
        D3D12_VIEWPORT vpRight{halfW,0.0f,halfW,float(h),0.0f,1.0f};

        float sbsRef[28];memcpy(sbsRef,refParams,sizeof(refParams));
        float sbsNeural[28];memcpy(sbsNeural,params,sizeof(params));
        for(float* q:{sbsRef,sbsNeural}){q[24]=cropX;q[25]=1.0f;q[26]=(1.0f-cropX)*0.5f;q[27]=0.0f;}

        cmd->RSSetViewports(1,&vpLeft);
        cmd->SetPipelineState(m_psoPresent.Get());
        cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));
        cmd->SetGraphicsRoot32BitConstants(1,28,sbsRef,0);
        cmd->DrawInstanced(3,1,0,0);

        cmd->RSSetViewports(1,&vpRight);
        cmd->SetGraphicsRoot32BitConstants(1,28,sbsNeural,0);
        cmd->SetPipelineState(composite?m_psoComposite.Get():m_psoPresent.Get());
        cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(1));
        cmd->DrawInstanced(3,1,0,0);

        D3D12_VIEWPORT vpFull{0.0f,0.0f,float(w),float(h),0.0f,1.0f};
        cmd->RSSetViewports(1,&vpFull);
        D3D12_RECT divider{LONG(halfW)-1,0,LONG(halfW)+2,h};
        cmd->ClearRenderTargetView(rtv,line,1,&divider);
    }else{
        cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(srvIndex));
        cmd->DrawInstanced(3,1,0,0);
    }

    if(transitioned)Barrier(cmd,transitioned,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,transitionedBefore);
}

bool D3D12Renderer::PresentCurrent(){
    if(!m_swapchain||!m_queue||!m_rootSig)return false;
    const uint32_t slot=m_frameSlot%FrameCount;
    if(!WaitForFrameSlot(slot))return false;
    if(!HR(m_allocators[slot]->Reset(),"Reset static-present allocator"))return false;
    auto* cmd=m_cmds[slot].Get();
    if(!HR(cmd->Reset(m_allocators[slot].Get(),nullptr),"Reset static-present command list"))return false;
    ID3D12DescriptorHeap*heaps[]={m_srvHeap.Get()};cmd->SetDescriptorHeaps(1,heaps);

    const float black[4]={0,0,0,1};
    uint32_t bi=m_swapchain->GetCurrentBackBufferIndex();
    Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_VIEWPORT ovp{0,0,float(m_outputW),float(m_outputH),0,1};
    D3D12_RECT osc{0,0,LONG(m_outputW),LONG(m_outputH)};
    cmd->RSSetViewports(1,&ovp);cmd->RSSetScissorRects(1,&osc);
    auto brt=RTV(bi);cmd->OMSetRenderTargets(1,&brt,FALSE,nullptr);cmd->ClearRenderTargetView(brt,black,0,nullptr);
    RecordPresentPass(cmd,brt,m_lastDLSSUsed&&DLSSEnabled());
    Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    if(!HR(cmd->Close(),"Close static-present command list"))return false;
    ID3D12CommandList*ls[]={cmd};m_queue->ExecuteCommandLists(1,ls);
    HRESULT phr=m_swapchain->Present(0,m_allowTearing?DXGI_PRESENT_ALLOW_TEARING:0);
    if(FAILED(phr)){LOG("Static Present failed hr=0x"<<std::hex<<phr);return false;}
    SignalFrameSlot(slot);m_frameSlot=(slot+1u)%FrameCount;
    return true;
}

void D3D12Renderer::Barrier(ID3D12GraphicsCommandList*cmd,ID3D12Resource*res,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){if(a==b)return;auto x=Transition(res,a,b);cmd->ResourceBarrier(1,&x);}
bool D3D12Renderer::WaitForFrameSlot(uint32_t slot){
    if(slot>=FrameCount||!m_fence||!m_fenceEvent)return false;
    const uint64_t v=m_frameFence[slot];
    if(v && m_fence->GetCompletedValue()<v){
        if(FAILED(m_fence->SetEventOnCompletion(v,m_fenceEvent)))return false;
        WaitForSingleObject(m_fenceEvent,INFINITE);
    }
    return true;
}
void D3D12Renderer::SignalFrameSlot(uint32_t slot){
    if(slot>=FrameCount||!m_queue||!m_fence)return;
    const uint64_t v=++m_fenceValue;
    if(SUCCEEDED(m_queue->Signal(m_fence.Get(),v)))m_frameFence[slot]=v;
}
void D3D12Renderer::WaitGPU(){
    if(!m_queue||!m_fence||!m_fenceEvent)return;
    uint64_t v=++m_fenceValue;m_queue->Signal(m_fence.Get(),v);
    if(m_fence->GetCompletedValue()<v){m_fence->SetEventOnCompletion(v,m_fenceEvent);WaitForSingleObject(m_fenceEvent,INFINITE);}
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::RTV(uint32_t i)const{auto h=m_rtvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*m_rtvInc;return h;}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::DSV()const{return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::SRVCPU(uint32_t i)const{auto h=m_srvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*m_srvInc;return h;}
D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::SRVGPU(uint32_t i)const{auto h=m_srvHeap->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(i)*m_srvInc;return h;}
