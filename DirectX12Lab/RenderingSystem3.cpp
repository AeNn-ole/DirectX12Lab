// ═══════════════════════════════════════════════════════════════════════════
// Функции построения ресурсов billboard pass:
//   BuildShaders()          — добавлен билборд
//   BuildConstantBuffers()  — добавлен BillboardFrameCB + centers buf
//   BuildDescriptorViews()  — добавлены слоты 2N+4, 2N+5
//   BuildRootSignatures()   — добавлен billboard root sig
//   BuildPSOs()             — добавлен billboard PSO
//   ComputeModelBounds()    — задаёт m_billboardSize
// ═══════════════════════════════════════════════════════════════════════════
#include "RenderingSystem.h"
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <algorithm>
#include <DirectXMath.h>
using namespace DirectX;
using Microsoft::WRL::ComPtr;

static void ThrowIfFailedB(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        char buf[256];
        std::snprintf(buf,sizeof(buf),"%s (hr=0x%08X)",what,(unsigned)hr);
        throw std::runtime_error(buf);
    }
}
static uint32_t AlignCBB(uint32_t size){ return (size+255u)&~255u; }
static D3D12_HEAP_PROPERTIES HeapB(D3D12_HEAP_TYPE t){
    D3D12_HEAP_PROPERTIES p{}; p.Type=t; p.CreationNodeMask=1; p.VisibleNodeMask=1; return p;
}
static D3D12_RESOURCE_DESC BufB(UINT64 bytes){
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width=bytes; d.Height=1; d.DepthOrArraySize=1; d.MipLevels=1;
    d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return d;
}

// ─────────────────────────────────────────────────────────────────────────────
// ИЗМЕНЕНО: BuildShaders — компилируем Billboard.hlsl
// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::BuildShaders()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> err;
    auto compile = [&](const wchar_t* file, const char* entry, const char* target,
                        ComPtr<ID3DBlob>& out)
    {
        err.Reset();
        HRESULT hr = D3DCompileFromFile(file, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entry, target, flags, 0, &out, &err);
        if (FAILED(hr)) {
            if (err) throw std::runtime_error((char*)err->GetBufferPointer());
            ThrowIfFailedB(hr, entry);
        }
    };

    compile(L"GBuffer.hlsl",   "VSMain",       "vs_5_0", m_gbVS);
    compile(L"GBuffer.hlsl",   "PSMain",       "ps_5_0", m_gbPS);
    compile(L"Lighting.hlsl",  "VSMain_Light", "vs_5_0", m_lightVS);
    compile(L"Lighting.hlsl",  "PSMain_Light", "ps_5_0", m_lightPS);
    compile(L"Billboard.hlsl", "VSMain",       "vs_5_0", m_billVS);
    compile(L"Billboard.hlsl", "PSMain",       "ps_5_0", m_billPS);

    m_inputLayout[0] = {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0, 0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
    m_inputLayout[1] = {"NORMAL",  0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
    m_inputLayout[2] = {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,   0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0};
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ИЗМЕНЕНО: BuildConstantBuffers — добавляем BillboardFrameCB + centers buf
// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::BuildConstantBuffers()
{
    // Instance CB
    m_instanceCBByteSize = AlignCBB(sizeof(PerInstanceCB));
    {
        UINT64 total = (UINT64)kMaxInstances * m_instanceCBByteSize;
        auto up=HeapB(D3D12_HEAP_TYPE_UPLOAD); auto bd=BufB(total);
        ThrowIfFailedB(m_device->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&bd,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&m_instanceCB)),"Instance CB");
        D3D12_RANGE rr{0,0};
        ThrowIfFailedB(m_instanceCB->Map(0,&rr,(void**)&m_mappedInstanceCB),"Map Instance CB");
    }

    // Material CB
    m_materialCBByteSize = AlignCBB(sizeof(PerMaterialCB));
    {
        UINT64 total = (UINT64)m_numMaterials * m_materialCBByteSize;
        auto up=HeapB(D3D12_HEAP_TYPE_UPLOAD); auto bd=BufB(total);
        ThrowIfFailedB(m_device->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&bd,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&m_materialCB)),"Material CB");
        D3D12_RANGE rr{0,0};
        ThrowIfFailedB(m_materialCB->Map(0,&rr,(void**)&m_mappedMaterialCB),"Map Material CB");
    }

    // Lighting CB
    {
        UINT64 total = AlignCBB(sizeof(LightingConstants));
        auto up=HeapB(D3D12_HEAP_TYPE_UPLOAD); auto bd=BufB(total);
        ThrowIfFailedB(m_device->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&bd,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&m_lightingCB)),"Lighting CB");
        D3D12_RANGE rr{0,0};
        ThrowIfFailedB(m_lightingCB->Map(0,&rr,(void**)&m_mappedLightingCB),"Map Lighting CB");
    }

    // НОВОЕ: BillboardFrameCB — один на кадр
    {
        UINT64 total = AlignCBB(sizeof(BillboardFrameCB));
        auto up=HeapB(D3D12_HEAP_TYPE_UPLOAD); auto bd=BufB(total);
        ThrowIfFailedB(m_device->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&bd,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&m_billboardFrameCB)),"Bill CB");
        D3D12_RANGE rr{0,0};
        ThrowIfFailedB(m_billboardFrameCB->Map(0,&rr,(void**)&m_mappedBillboardFrameCB),"Map Bill CB");
    }

    // НОВОЕ: Centers StructuredBuffer — kMaxInstances × float4
    // Upload heap, постоянно mapped — обновляется каждый кадр
    {
        UINT64 total = (UINT64)kMaxInstances * sizeof(float) * 4;
        auto up=HeapB(D3D12_HEAP_TYPE_UPLOAD); auto bd=BufB(total);
        ThrowIfFailedB(m_device->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&bd,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&m_billboardCentersBuf)),"Bill Centers");
        D3D12_RANGE rr{0,0};
        ThrowIfFailedB(m_billboardCentersBuf->Map(0,&rr,(void**)&m_mappedBillboardCenters),"Map Bill Centers");
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ИЗМЕНЕНО: BuildDescriptorViews — 2 новых слота: 2N+4 (tex) и 2N+5 (centers)
// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::BuildDescriptorViews()
{
    const uint32_t N = m_numMaterials;

    // Было 2N+4 слота, стало 2N+6
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 2*N + 6;
        hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailedB(m_device->CreateDescriptorHeap(&hd,
            IID_PPV_ARGS(&m_cbvSrvHeap)),"CBV/SRV Heap");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase =
        m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart();

    // [0..N-1]: CBV материалов
    for (uint32_t i = 0; i < N; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvd{};
        cbvd.BufferLocation = m_materialCB->GetGPUVirtualAddress() +
                              (UINT64)i * m_materialCBByteSize;
        cbvd.SizeInBytes    = m_materialCBByteSize;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
        h.ptr += (SIZE_T)(i * m_cbvSrvDescriptorSize);
        m_device->CreateConstantBufferView(&cbvd, h);
    }

    // [N..2N-1]: SRV текстур материалов
    m_gpuMaterials.resize(N);
    for (uint32_t i = 0; i < N; ++i)
    {
        const wchar_t* texPath = m_model.materials[i].map_Kd.empty()
            ? L"texture.png" : m_model.materials[i].map_Kd.c_str();
        LoadAndUploadTexture(texPath, m_gpuMaterials[i].texture);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
        h.ptr += (SIZE_T)((N + i) * m_cbvSrvDescriptorSize);
        m_device->CreateShaderResourceView(m_gpuMaterials[i].texture.Get(), &srvd, h);
    }

    // [2N..2N+2]: G-Buffer SRV
    m_gbuffer.Create(m_device.Get(), m_width, m_height,
        m_rtvHeap.Get(), kSwapChainBufferCount, m_rtvDescriptorSize,
        m_cbvSrvHeap.Get(), 2*N, m_cbvSrvDescriptorSize);

    // [2N+3]: depth SRV
    RecreateDepthSRV();

    // НОВОЕ [2N+4]: billboard текстура SRV
    {
        LoadAndUploadTexture(L"billboard.png", m_billboardTexture);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
        h.ptr += (SIZE_T)((2*N + 4) * m_cbvSrvDescriptorSize);
        m_device->CreateShaderResourceView(m_billboardTexture.Get(), &srvd, h);
    }

    // НОВОЕ [2N+5]: billboard centers StructuredBuffer SRV
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format                         = DXGI_FORMAT_UNKNOWN;
        srvd.ViewDimension                  = D3D12_SRV_DIMENSION_BUFFER;
        srvd.Shader4ComponentMapping        = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Buffer.FirstElement            = 0;
        srvd.Buffer.NumElements             = (UINT)kMaxInstances;
        srvd.Buffer.StructureByteStride     = sizeof(float) * 4;
        srvd.Buffer.Flags                   = D3D12_BUFFER_SRV_FLAG_NONE;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
        h.ptr += (SIZE_T)((2*N + 5) * m_cbvSrvDescriptorSize);
        m_device->CreateShaderResourceView(m_billboardCentersBuf.Get(), &srvd, h);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ИЗМЕНЕНО: BuildRootSignatures — добавлен billboard root sig
//
// Billboard root sig:
//   param[0]: inline CBV b0  → BillboardFrameCB
//   param[1]: table 1×SRV t0 → billboard текстура
//   param[2]: table 1×SRV t1 → centers StructuredBuffer
// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::BuildRootSignatures()
{
    // ── Geometry root sig (без изменений) ─────────────────────────────────
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,1,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor={0,0}; params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable={1,&ranges[0]}; params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable={1,&ranges[1]}; params[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU=samp.AddressV=samp.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samp.MaxAnisotropy=1; samp.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        samp.MaxLOD=D3D12_FLOAT32_MAX; samp.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd{3,params,1,&samp,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ComPtr<ID3DBlob> ser,err;
        HRESULT hr=D3D12SerializeRootSignature(&rsd,D3D_ROOT_SIGNATURE_VERSION_1,&ser,&err);
        if(FAILED(hr)){if(err)throw std::runtime_error((char*)err->GetBufferPointer());}
        ThrowIfFailedB(m_device->CreateRootSignature(0,ser->GetBufferPointer(),
            ser->GetBufferSize(),IID_PPV_ARGS(&m_geometryRootSig)),"Geom RS");
    }

    // ── Lighting root sig (без изменений) ─────────────────────────────────
    {
        D3D12_DESCRIPTOR_RANGE srvRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable={1,&srvRange}; params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor={0,0}; params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU=samp.AddressV=samp.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxAnisotropy=1; samp.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        samp.MaxLOD=D3D12_FLOAT32_MAX; samp.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd{2,params,1,&samp,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> ser,err;
        D3D12SerializeRootSignature(&rsd,D3D_ROOT_SIGNATURE_VERSION_1,&ser,&err);
        ThrowIfFailedB(m_device->CreateRootSignature(0,ser->GetBufferPointer(),
            ser->GetBufferSize(),IID_PPV_ARGS(&m_lightingRootSig)),"Light RS");
    }

    // ── НОВОЕ: Billboard root sig ─────────────────────────────────────────
    {
        // t0 = billboard texture, t1 = centers buffer
        D3D12_DESCRIPTOR_RANGE texRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        D3D12_DESCRIPTOR_RANGE cenRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,1,0,
            D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};

        D3D12_ROOT_PARAMETER params[3]{};
        // param[0]: inline CBV b0 — BillboardFrameCB
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor={0,0}; params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        // param[1]: table t0 — billboard texture (PS)
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable={1,&texRange}; params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        // param[2]: table t1 — centers StructuredBuffer (VS)
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable={1,&cenRange}; params[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU=samp.AddressV=samp.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samp.MaxAnisotropy=1; samp.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        samp.MaxLOD=D3D12_FLOAT32_MAX; samp.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd{3,params,1,&samp,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> ser,err;
        HRESULT hr=D3D12SerializeRootSignature(&rsd,D3D_ROOT_SIGNATURE_VERSION_1,&ser,&err);
        if(FAILED(hr)){if(err)throw std::runtime_error((char*)err->GetBufferPointer());}
        ThrowIfFailedB(m_device->CreateRootSignature(0,ser->GetBufferPointer(),
            ser->GetBufferSize(),IID_PPV_ARGS(&m_billboardRootSig)),"Bill RS");
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ИЗМЕНЕНО: BuildPSOs — добавлен billboard PSO
// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::BuildPSOs()
{
    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode=D3D12_FILL_MODE_SOLID; rast.CullMode=D3D12_CULL_MODE_BACK;
    rast.DepthClipEnable=TRUE; rast.DepthBias=D3D12_DEFAULT_DEPTH_BIAS;
    rast.DepthBiasClamp=D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rast.SlopeScaledDepthBias=D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;

    // Geometry PSO (без изменений)
    {
        D3D12_BLEND_DESC blend{}; for(int i=0;i<3;++i)
            blend.RenderTarget[i].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable=TRUE; ds.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc=D3D12_COMPARISON_FUNC_LESS; ds.StencilEnable=FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature=m_geometryRootSig.Get();
        pd.VS={m_gbVS->GetBufferPointer(),m_gbVS->GetBufferSize()};
        pd.PS={m_gbPS->GetBufferPointer(),m_gbPS->GetBufferSize()};
        pd.BlendState=blend; pd.RasterizerState=rast; pd.DepthStencilState=ds;
        pd.SampleMask=UINT_MAX; pd.InputLayout={m_inputLayout,3};
        pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets=3;
        pd.RTVFormats[0]=GBuffer::kFormats[GBuffer::Albedo];
        pd.RTVFormats[1]=GBuffer::kFormats[GBuffer::Normal];
        pd.RTVFormats[2]=GBuffer::kFormats[GBuffer::Specular];
        pd.DSVFormat=DXGI_FORMAT_D24_UNORM_S8_UINT; pd.SampleDesc.Count=1;
        ThrowIfFailedB(m_device->CreateGraphicsPipelineState(&pd,
            IID_PPV_ARGS(&m_geometryPSO)),"Geom PSO");
    }

    // Lighting PSO (без изменений)
    {
        D3D12_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable=FALSE; ds.StencilEnable=FALSE;
        D3D12_RASTERIZER_DESC rLight=rast; rLight.CullMode=D3D12_CULL_MODE_NONE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature=m_lightingRootSig.Get();
        pd.VS={m_lightVS->GetBufferPointer(),m_lightVS->GetBufferSize()};
        pd.PS={m_lightPS->GetBufferPointer(),m_lightPS->GetBufferSize()};
        pd.BlendState=blend; pd.RasterizerState=rLight; pd.DepthStencilState=ds;
        pd.SampleMask=UINT_MAX; pd.InputLayout={nullptr,0};
        pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets=1; pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.DSVFormat=DXGI_FORMAT_UNKNOWN; pd.SampleDesc.Count=1;
        ThrowIfFailedB(m_device->CreateGraphicsPipelineState(&pd,
            IID_PPV_ARGS(&m_lightingPSO)),"Light PSO");
    }

    // НОВОЕ: Billboard PSO
    // - Depth test ON (спрайты должны быть за ближними 3D объектами)
    // - Depth write ON (спрайты перекрывают друг друга корректно)
    // - Culling NONE (квад может смотреть в любую сторону)
    // - Alpha-to-coverage OFF (используем clip() в шейдере вместо blending)
    {
        D3D12_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable    = TRUE;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
        ds.StencilEnable  = FALSE;

        D3D12_RASTERIZER_DESC rBill = rast;
        rBill.CullMode = D3D12_CULL_MODE_NONE;   // квад двухсторонний

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature=m_billboardRootSig.Get();
        pd.VS={m_billVS->GetBufferPointer(),m_billVS->GetBufferSize()};
        pd.PS={m_billPS->GetBufferPointer(),m_billPS->GetBufferSize()};
        pd.BlendState=blend; pd.RasterizerState=rBill; pd.DepthStencilState=ds;
        pd.SampleMask=UINT_MAX; pd.InputLayout={nullptr,0};  // нет VB
        pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets=1; pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.DSVFormat=DXGI_FORMAT_D24_UNORM_S8_UINT; pd.SampleDesc.Count=1;
        ThrowIfFailedB(m_device->CreateGraphicsPipelineState(&pd,
            IID_PPV_ARGS(&m_billboardPSO)),"Billboard PSO");
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ИЗМЕНЕНО: ComputeModelBounds — дополнительно задаёт m_billboardSize
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::ComputeModelBounds()
{
    if (m_model.vertices.empty()) { m_modelRadius=1.f; m_billboardSize=2.f; return; }

    XMVECTOR vmin=XMVectorReplicate( FLT_MAX);
    XMVECTOR vmax=XMVectorReplicate(-FLT_MAX);
    for (const auto& v : m_model.vertices) {
        XMVECTOR p=XMLoadFloat3(&v.Pos);
        vmin=XMVectorMin(vmin,p); vmax=XMVectorMax(vmax,p);
    }
    XMVECTOR center=XMVectorScale(XMVectorAdd(vmin,vmax),0.5f);
    XMStoreFloat3(&m_modelCenter,center);

    XMVECTOR halfDiag=XMVectorScale(XMVectorSubtract(vmax,vmin),0.5f);
    m_modelRadius  = XMVectorGetX(XMVector3Length(halfDiag));

    // Размер квада = диаметр AABB. Немного увеличен (×1.1) чтобы не обрезать края.
    XMFLOAT3 ext;
    XMStoreFloat3(&ext, XMVectorSubtract(vmax,vmin));
    m_billboardSize = (std::max)((std::max)(ext.x, ext.y), ext.z) * 1.1f;
}
