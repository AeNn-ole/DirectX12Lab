#include "RenderingSystem.h"
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <wincodec.h>
#include <DirectXMath.h>

#pragma comment(lib, "windowscodecs.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ─────────────────────────────────────────────────────────────────────────────
static void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (hr=0x%08X)", what, (unsigned)hr);
        OutputDebugStringA("RenderingSystem Error: ");
        OutputDebugStringA(buf); OutputDebugStringA("\n");
        throw std::runtime_error(buf);
    }
}

static uint32_t AlignCB(uint32_t size) { return (size + 255u) & ~255u; }

static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type; p.CreationNodeMask = 1; p.VisibleNodeMask = 1;
    return p;
}

static D3D12_RESOURCE_DESC BufDesc(UINT64 bytes)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    m_initialized = false;
    m_hwnd = hwnd; m_width = width; m_height = height;

#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
            dbg->EnableDebugLayer();
    }
#endif

    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory1");

    CreateDevice();
    CreateCommandObjects();

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&m_fence)), "CreateFence");
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) throw std::runtime_error("CreateEvent failed");

    CreateSwapChain();

    m_rtvDescriptorSize    = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_dsvDescriptorSize    = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_cbvSrvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    CreateDescriptorHeaps();
    CreateRtvForBackBuffers();
    CreateDepthStencil();

    m_viewport    = { 0, 0, (float)width, (float)height, 0, 1 };
    m_scissorRect = { 0, 0, (LONG)width,  (LONG)height };

    XMVECTOR eye = XMVectorSet(m_eyePos.x, m_eyePos.y, m_eyePos.z, 1);
    XMStoreFloat4x4(&m_view, XMMatrixLookAtLH(eye, XMVectorZero(), XMVectorSet(0,1,0,0)));
    float asp = height > 0 ? (float)width / height : 1.f;
    XMStoreFloat4x4(&m_proj, XMMatrixPerspectiveFovLH(0.25f * XM_PI, asp, 0.1f, 5000.f));

    // ── Загрузка модели ───────────────────────────────────────────────────
    if (!LoadObj(L"model.obj", m_model))
        throw std::runtime_error(
            "Не удалось открыть model.obj.\n"
            "Положите OBJ-файл рядом с .exe и переименуйте его в model.obj");

    if (m_model.materials.empty())
        m_model.materials.push_back({ "__default__" });

    m_numMaterials = (uint32_t)m_model.materials.size();

    ComputeModelBounds();
    ScatterInstances();
    BuildOctree();

    BuildShaders();
    BuildGeometry();
    BuildConstantBuffers();
    BuildDescriptorViews();
    BuildRootSignatures();
    BuildPSOs();
    CreateHdrRT();
    BuildPostFxPSO();
    CreateShadowMap();
    BuildShadowPSO();
    InitLights();

    m_initialized = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::ComputeModelBounds()
{
    if (m_model.vertices.empty()) { m_modelRadius = 1.f; return; }

    XMVECTOR vmin = XMVectorReplicate( FLT_MAX);
    XMVECTOR vmax = XMVectorReplicate(-FLT_MAX);

    for (const auto& v : m_model.vertices)
    {
        XMVECTOR p = XMLoadFloat3(&v.Pos);
        vmin = XMVectorMin(vmin, p);
        vmax = XMVectorMax(vmax, p);
    }

    XMVECTOR center = XMVectorScale(XMVectorAdd(vmin, vmax), 0.5f);
    XMStoreFloat3(&m_modelCenter, center);

    XMVECTOR halfDiag = XMVectorScale(XMVectorSubtract(vmax, vmin), 0.5f);
    m_modelRadius = XMVectorGetX(XMVector3Length(halfDiag));
}

// ─────────────────────────────────────────────────────────────────────────────
// ScatterInstances — теперь создаёт ровно ОДИН инстанс модели (Identity).
// Octree/frustum culling остаются в коде (полезны при многих subMesh-ах
// у Sponza), ноculling-сетка инстансов больше не нужна.
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::ScatterInstances()
{
    m_instances.clear();

    InstanceData d{};
    XMStoreFloat4x4(&d.World, XMMatrixIdentity());
    d.Center = m_modelCenter;
    d.Radius = m_modelRadius;

    m_instances.push_back(d);
}

// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::Shutdown()
{
    if (m_cmdQueue) FlushCommandQueue();
    if (m_instanceCB && m_mappedInstanceCB)
    { m_instanceCB->Unmap(0, nullptr); m_mappedInstanceCB = nullptr; }
    if (m_materialCB && m_mappedMaterialCB)
    { m_materialCB->Unmap(0, nullptr); m_mappedMaterialCB = nullptr; }
    if (m_lightingCB && m_mappedLightingCB)
    { m_lightingCB->Unmap(0, nullptr); m_mappedLightingCB = nullptr; }
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
}

// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::CreateDevice()
{
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) {
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailed(m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter");
        ThrowIfFailed(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&m_device)), "D3D12CreateDevice WARP");
    }

    // Проверяем поддержку тесселяции (обязательна начиная с D3D_FEATURE_LEVEL_11_0)
    D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
    if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))))
        OutputDebugStringA("CheckFeatureSupport failed\n");
    // Tessellation всегда доступна на FL 11+, просто логируем
    OutputDebugStringA("Device created. Tessellation supported (FL12_0).\n");

    return true;
}

bool RenderingSystem::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_cmdQueue)), "CreateCommandQueue");
    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_cmdAlloc)), "CreateCommandAllocator");
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&m_cmdList)), "CreateCommandList");
    ThrowIfFailed(m_cmdList->Close(), "CmdList Close (init)");
    return true;
}

bool RenderingSystem::CreateSwapChain()
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc   = { m_width, m_height, {60,1}, DXGI_FORMAT_R8G8B8A8_UNORM };
    sd.SampleDesc   = { 1, 0 };
    sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount  = kSwapChainBufferCount;
    sd.OutputWindow = m_hwnd;
    sd.Windowed     = TRUE;
    sd.SwapEffect   = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    m_swapChain.Reset();
    ThrowIfFailed(m_factory->CreateSwapChain(m_cmdQueue.Get(), &sd,
        m_swapChain.GetAddressOf()), "CreateSwapChain");
    m_currBackBuffer = 0;
    return true;
}

bool RenderingSystem::CreateDescriptorHeaps()
{
    {
        D3D12_DESCRIPTOR_HEAP_DESC rd{};
        rd.NumDescriptors = kSwapChainBufferCount + GBuffer::Count;
        rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&m_rtvHeap)), "RTV Heap");
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC dd{};
        dd.NumDescriptors = 1;
        dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&dd, IID_PPV_ARGS(&m_dsvHeap)), "DSV Heap");
    }
    return true;
}

bool RenderingSystem::CreateRtvForBackBuffers()
{
    auto h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kSwapChainBufferCount; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_swapChainBuffers[i])), "GetBuffer");
        m_device->CreateRenderTargetView(m_swapChainBuffers[i].Get(), nullptr, h);
        h.ptr += m_rtvDescriptorSize;
    }
    return true;
}

bool RenderingSystem::CreateDepthStencil()
{
    m_depthStencilBuffer.Reset();

    D3D12_RESOURCE_DESC td{};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = m_width;
    td.Height           = m_height;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.Format           = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv{};
    cv.Format              = DXGI_FORMAT_D24_UNORM_S8_UINT;
    cv.DepthStencil.Depth  = 1.f;
    cv.DepthStencil.Stencil = 0;

    auto hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COMMON, &cv, IID_PPV_ARGS(&m_depthStencilBuffer)), "Create DS");

    ThrowIfFailed(m_cmdAlloc->Reset(), "Alloc Reset DS");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "List Reset DS");
    D3D12_RESOURCE_BARRIER b{};
    b.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { m_depthStencilBuffer.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                     D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE };
    m_cmdList->ResourceBarrier(1, &b);
    ThrowIfFailed(m_cmdList->Close(), "CmdList Close DS");
    ID3D12CommandList* ls[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, ls);
    FlushCommandQueue();

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvd{};
    dsvd.Format        = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(m_depthStencilBuffer.Get(), &dsvd,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Depth SRV находится по смещению [4N+3] в куче
// ─────────────────────────────────────────────────────────────────────────────
void RenderingSystem::RecreateDepthSRV()
{
    const uint32_t N = m_numMaterials;
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)((4 * N + 3) * m_cbvSrvDescriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                  = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels     = 1;
    m_device->CreateShaderResourceView(m_depthStencilBuffer.Get(), &srvd, h);
}

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
            ThrowIfFailed(hr, entry);
        }
    };

    // Geometry pass: VS + HS + DS + PS
    compile(L"GBuffer.hlsl", "VSMain",       "vs_5_0", m_gbVS);
    compile(L"GBuffer.hlsl", "HSMain",       "hs_5_0", m_gbHS);
    compile(L"GBuffer.hlsl", "DSMain",       "ds_5_0", m_gbDS);
    compile(L"GBuffer.hlsl", "PSMain",       "ps_5_0", m_gbPS);

    // Lighting pass
    compile(L"Lighting.hlsl", "VSMain_Light", "vs_5_0", m_lightVS);
    compile(L"Lighting.hlsl", "PSMain_Light", "ps_5_0", m_lightPS);

    // Input layout: POSITION, NORMAL, TANGENT, TEXCOORD
    // Смещения соответствуют ObjVertex: Pos(0), Normal(12), Tangent(24), TexCoord(36)
    m_inputLayout[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[1] = { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[2] = { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    m_inputLayout[3] = { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::BuildGeometry()
{
    const UINT64 vbBytes = (UINT64)m_model.vertices.size() * sizeof(ObjVertex);
    const UINT64 ibBytes = (UINT64)m_model.indices.size()  * sizeof(uint32_t);

    auto defHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto upHeap  = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto vbDesc  = BufDesc(vbBytes), ibDesc = BufDesc(ibBytes);

    ThrowIfFailed(m_device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_vertexBufferGPU)), "VB GPU");
    ThrowIfFailed(m_device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_indexBufferGPU)), "IB GPU");

    ComPtr<ID3D12Resource> vbUp, ibUp;
    ThrowIfFailed(m_device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vbUp)), "VB Upload");
    ThrowIfFailed(m_device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ibUp)), "IB Upload");

    auto upload = [](ID3D12Resource* r, const void* data, size_t sz) {
        void* p; D3D12_RANGE rr{0,0};
        r->Map(0, &rr, &p); std::memcpy(p, data, sz); r->Unmap(0, nullptr);
    };
    upload(vbUp.Get(), m_model.vertices.data(), (size_t)vbBytes);
    upload(ibUp.Get(),  m_model.indices.data(),  (size_t)ibBytes);

    ThrowIfFailed(m_cmdAlloc->Reset(), "Alloc Geom");
    ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc.Get(), nullptr), "List Geom");

    m_cmdList->CopyBufferRegion(m_vertexBufferGPU.Get(), 0, vbUp.Get(), 0, vbBytes);
    m_cmdList->CopyBufferRegion(m_indexBufferGPU.Get(),  0, ibUp.Get(), 0, ibBytes);

    D3D12_RESOURCE_BARRIER bars[2]{};
    bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[0].Transition = { m_vertexBufferGPU.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER };
    bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bars[1].Transition = { m_indexBufferGPU.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER };
    m_cmdList->ResourceBarrier(2, bars);

    ThrowIfFailed(m_cmdList->Close(), "CmdList Close Geom");
    ID3D12CommandList* ls[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, ls);
    FlushCommandQueue();

    m_vbv = { m_vertexBufferGPU->GetGPUVirtualAddress(), (UINT)vbBytes, sizeof(ObjVertex) };
    m_ibv = { m_indexBufferGPU->GetGPUVirtualAddress(),  (UINT)ibBytes, DXGI_FORMAT_R32_UINT };
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::BuildConstantBuffers()
{
    m_instanceCBByteSize = AlignCB(sizeof(PerInstanceCB));
    {
        UINT64 total = (UINT64)kMaxInstances * m_instanceCBByteSize;
        auto up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = BufDesc(total);
        ThrowIfFailed(m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_instanceCB)),
            "Create Instance CB");
        D3D12_RANGE rr{0,0};
        ThrowIfFailed(m_instanceCB->Map(0, &rr,
            reinterpret_cast<void**>(&m_mappedInstanceCB)), "Map Instance CB");
    }

    m_materialCBByteSize = AlignCB(sizeof(PerMaterialCB));
    {
        UINT64 total = (UINT64)m_numMaterials * m_materialCBByteSize;
        auto up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = BufDesc(total);
        ThrowIfFailed(m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_materialCB)),
            "Create Material CB");
        D3D12_RANGE rr{0,0};
        ThrowIfFailed(m_materialCB->Map(0, &rr,
            reinterpret_cast<void**>(&m_mappedMaterialCB)), "Map Material CB");
    }

    {
        UINT64 total = AlignCB(sizeof(LightingConstants));
        auto up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = BufDesc(total);
        ThrowIfFailed(m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_lightingCB)),
            "Create Light CB");
        D3D12_RANGE rr{0,0};
        ThrowIfFailed(m_lightingCB->Map(0, &rr,
            reinterpret_cast<void**>(&m_mappedLightingCB)), "Map Light CB");
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Раскладка кучи (новая):
//   [0  .. N-1 ]      CBV → PerMaterialCB
//   [N  .. 2N-1]      SRV → albedo texture
//   [2N .. 3N-1]      SRV → normal map
//   [3N .. 4N-1]      SRV → displacement map
//   [4N .. 4N+2]      SRV → G-Buffer (3 текстуры)
//   [4N+3]            SRV → глубина
// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::BuildDescriptorViews()
{
    const uint32_t N = m_numMaterials;

    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 4 * N + 6;   // N CBV + N albedo + N normal + N disp + 3 GBuf + 1 depth + 1 HDR RT + 1 shadow map
        hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd,
            IID_PPV_ARGS(&m_cbvSrvHeap)), "CBV/SRV Heap");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase =
        m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart();

    // ── CBV [0..N-1] ──────────────────────────────────────────────────────
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

    // Вспомогательная функция создания SRV с заглушкой
    auto makeSRV = [&](ID3D12Resource* tex, uint32_t heapSlot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels     = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
        h.ptr += (SIZE_T)(heapSlot * m_cbvSrvDescriptorSize);
        m_device->CreateShaderResourceView(tex, &srvd, h);
    };

    m_gpuMaterials.resize(N);
    for (uint32_t i = 0; i < N; ++i)
    {
        const auto& mat = m_model.materials[i];

        // Albedo [N + i]
        const wchar_t* albedoPath = mat.map_Kd.empty() ? L"texture.png" : mat.map_Kd.c_str();
        LoadAndUploadTexture(albedoPath, m_gpuMaterials[i].texture);
        makeSRV(m_gpuMaterials[i].texture.Get(), N + i);

        // Normal map [2N + i]
        const wchar_t* normalPath = mat.map_Bump.empty() ? L"normal_default.png" : mat.map_Bump.c_str();
        LoadAndUploadTexture(normalPath, m_gpuMaterials[i].normalMap);
        makeSRV(m_gpuMaterials[i].normalMap.Get(), 2 * N + i);

        // Displacement map [3N + i]
        const wchar_t* dispPath = mat.map_Disp.empty() ? L"disp_default.png" : mat.map_Disp.c_str();
        LoadAndUploadTexture(dispPath, m_gpuMaterials[i].dispMap);
        makeSRV(m_gpuMaterials[i].dispMap.Get(), 3 * N + i);
    }

    // ── G-Buffer SRV [4N..4N+2] ───────────────────────────────────────────
    m_gbuffer.Create(m_device.Get(), m_width, m_height,
        m_rtvHeap.Get(), kSwapChainBufferCount, m_rtvDescriptorSize,
        m_cbvSrvHeap.Get(), 4 * N, m_cbvSrvDescriptorSize);

    // ── Depth SRV [4N+3] ─────────────────────────────────────────────────
    RecreateDepthSRV();

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Root Signatures
//
// Geometry pass (новая раскладка):
//   param[0]: inline CBV  → b0  (PerInstanceCB) — меняется на каждый инстанс
//   param[1]: table 1×CBV → b1  (PerMaterialCB) — меняется на каждый материал
//   param[2]: table 1×SRV → t0  (albedo)
//   param[3]: table 1×SRV → t1  (normal map)
//   param[4]: table 1×SRV → t2  (displacement map)
//   s0: wrap sampler (albedo/normal)
//   s1: point sampler (displacement — для SampleLevel в DS)
//
// Lighting pass — без изменений.
// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::BuildRootSignatures()
{
    // ── Geometry ──────────────────────────────────────────────────────────
    {
        D3D12_DESCRIPTOR_RANGE ranges[4]{};
        // param[1]: b1 — PerMaterialCB
        ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
        // param[2]: t0 — albedo
        ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
        // param[3]: t1 — normal map
        ranges[2] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
        // param[4]: t2 — displacement
        ranges[3] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };

        D3D12_ROOT_PARAMETER params[5]{};

        // param[0]: inline CBV → b0
        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace  = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        // param[1]: table CBV → b1
        params[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable  = { 1, &ranges[0] };
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // param[2]: table SRV → t0 (albedo) — нужен PS
        params[2].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable  = { 1, &ranges[1] };
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // param[3]: table SRV → t1 (normal map) — нужен PS
        params[3].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable  = { 1, &ranges[2] };
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // param[4]: table SRV → t2 (displacement) — нужен DS и PS
        params[4].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[4].DescriptorTable  = { 1, &ranges[3] };
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Два статических сэмплера: wrap (s0) и point (s1)
        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        // s0 — wrap, для albedo/normal в PS
        samplers[0].Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[0].MaxAnisotropy  = 1;
        samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[0].MaxLOD         = D3D12_FLOAT32_MAX;
        samplers[0].ShaderRegister = 0;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // s1 — point wrap, для displacement в DS (SampleLevel)
        samplers[1].Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[1].MaxAnisotropy  = 1;
        samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[1].MaxLOD         = D3D12_FLOAT32_MAX;
        samplers[1].ShaderRegister = 1;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters     = 5; rsd.pParameters = params;
        rsd.NumStaticSamplers = 2; rsd.pStaticSamplers = samplers;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> ser, err;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &err);
        if (FAILED(hr)) { if (err) throw std::runtime_error((char*)err->GetBufferPointer()); ThrowIfFailed(hr, "SerializeRS Geom"); }
        ThrowIfFailed(m_device->CreateRootSignature(0, ser->GetBufferPointer(),
            ser->GetBufferSize(), IID_PPV_ARGS(&m_geometryRootSig)), "CreateRS Geom");
    }

    // ── Lighting (без изменений) ──────────────────────────────────────────
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable  = { 1, &srvRange };
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 0;
        params[1].Descriptor.RegisterSpace  = 0;
        params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        // s0 — point clamp (G-Buffer сэмплирование)
        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        samplers[0].Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].MaxAnisotropy  = 1;
        samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[0].MaxLOD         = D3D12_FLOAT32_MAX;
        samplers[0].ShaderRegister = 0;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // s1 — comparison sampler для PCF (SampleCmpLevelZero)
        samplers[1].Filter         = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[1].MaxAnisotropy  = 1;
        samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samplers[1].MaxLOD         = D3D12_FLOAT32_MAX;
        samplers[1].ShaderRegister = 1;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters     = 2; rsd.pParameters = params;
        rsd.NumStaticSamplers = 2; rsd.pStaticSamplers = samplers;
        rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> ser, err;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &err);
        if (FAILED(hr)) { if (err) throw std::runtime_error((char*)err->GetBufferPointer()); ThrowIfFailed(hr, "SerializeRS Light"); }
        ThrowIfFailed(m_device->CreateRootSignature(0, ser->GetBufferPointer(),
            ser->GetBufferSize(), IID_PPV_ARGS(&m_lightingRootSig)), "CreateRS Light");
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildPSOs — geometry PSO с HS+DS, тип топологии PATCH
// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::BuildPSOs()
{
    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode        = D3D12_FILL_MODE_SOLID;
    rast.CullMode        = D3D12_CULL_MODE_BACK;
    rast.FrontCounterClockwise = FALSE;
    rast.DepthClipEnable = TRUE;
    rast.DepthBias       = D3D12_DEFAULT_DEPTH_BIAS;
    rast.DepthBiasClamp  = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rast.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;

    // ── Geometry PSO (с тесселяцией) ─────────────────────────────────────
    {
        D3D12_BLEND_DESC blend{};
        for (int i = 0; i < 3; ++i)
            blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable    = TRUE;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
        ds.StencilEnable  = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature    = m_geometryRootSig.Get();
        pd.VS                = { m_gbVS->GetBufferPointer(), m_gbVS->GetBufferSize() };
        pd.HS                = { m_gbHS->GetBufferPointer(), m_gbHS->GetBufferSize() };
        pd.DS                = { m_gbDS->GetBufferPointer(), m_gbDS->GetBufferSize() };
        pd.PS                = { m_gbPS->GetBufferPointer(), m_gbPS->GetBufferSize() };
        pd.BlendState        = blend;
        pd.RasterizerState   = rast;
        pd.DepthStencilState = ds;
        pd.SampleMask        = UINT_MAX;
        pd.InputLayout       = { m_inputLayout, 4 };
        // Тесселяция требует PATCH топологию
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        pd.NumRenderTargets  = 3;
        pd.RTVFormats[0]     = GBuffer::kFormats[GBuffer::Albedo];
        pd.RTVFormats[1]     = GBuffer::kFormats[GBuffer::Normal];
        pd.RTVFormats[2]     = GBuffer::kFormats[GBuffer::Specular];
        pd.DSVFormat         = DXGI_FORMAT_D24_UNORM_S8_UINT;
        pd.SampleDesc.Count  = 1;

        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd,
            IID_PPV_ARGS(&m_geometryPSO)), "Create Geometry PSO");

        // ── Wireframe PSO — всё то же самое, только FillMode = WIREFRAME ──
        // Рендерим в back buffer напрямую (один RT, без G-Buffer),
        // чтобы видеть сетку тесселяции поверх обычного кадра.
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // чтобы видеть back faces
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0]    = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.RTVFormats[1]    = DXGI_FORMAT_UNKNOWN;
        pd.RTVFormats[2]    = DXGI_FORMAT_UNKNOWN;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd,
            IID_PPV_ARGS(&m_geometryWirePSO)), "Create Wireframe PSO");
    }

    // ── Lighting PSO (без изменений) ──────────────────────────────────────
    {
        D3D12_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable   = FALSE;
        ds.StencilEnable = FALSE;

        D3D12_RASTERIZER_DESC rastLight = rast;
        rastLight.CullMode = D3D12_CULL_MODE_NONE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature    = m_lightingRootSig.Get();
        pd.VS                = { m_lightVS->GetBufferPointer(), m_lightVS->GetBufferSize() };
        pd.PS                = { m_lightPS->GetBufferPointer(), m_lightPS->GetBufferSize() };
        pd.BlendState        = blend;
        pd.RasterizerState   = rastLight;
        pd.DepthStencilState = ds;
        pd.SampleMask        = UINT_MAX;
        pd.InputLayout       = { nullptr, 0 };
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets  = 1;
        pd.RTVFormats[0]     = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.DSVFormat         = DXGI_FORMAT_UNKNOWN;
        pd.SampleDesc.Count  = 1;

        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd,
            IID_PPV_ARGS(&m_lightingPSO)), "Create Lighting PSO");
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// CreateHdrRT — промежуточный HDR render target
// Формат R16G16B16A16_FLOAT — сохраняет HDR-диапазон после lighting pass
// SRV кладём в m_cbvSrvHeap слот [4N+4]
// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::CreateHdrRT()
{
    m_hdrRT.Reset();

    // ── Texture ───────────────────────────────────────────────────────────
    D3D12_RESOURCE_DESC td{};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = m_width;
    td.Height           = m_height;
    td.DepthOrArraySize = 1;
    td.MipLevels        = 1;
    td.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE cv{};
    cv.Format   = DXGI_FORMAT_R16G16B16A16_FLOAT;
    cv.Color[0] = cv.Color[1] = cv.Color[2] = 0.f; cv.Color[3] = 1.f;

    auto hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
        IID_PPV_ARGS(&m_hdrRT)), "Create HDR RT");

    // ── RTV ───────────────────────────────────────────────────────────────
    if (!m_hdrRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd,
            IID_PPV_ARGS(&m_hdrRtvHeap)), "HDR RTV Heap");
    }
    m_hdrRtv = m_hdrRtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(m_hdrRT.Get(), nullptr, m_hdrRtv);

    // ── SRV в слоте [4N+4] ────────────────────────────────────────────────
    const uint32_t slot = 4 * m_numMaterials + 5;
    D3D12_CPU_DESCRIPTOR_HANDLE srvH =
        m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart();
    srvH.ptr += (SIZE_T)(slot * m_cbvSrvDescriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels     = 1;
    m_device->CreateShaderResourceView(m_hdrRT.Get(), &srvd, srvH);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::BuildPostFxPSO()
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
            ThrowIfFailed(hr, entry);
        }
    };
    compile(L"PostFx.hlsl", "VSMain_Post", "vs_5_0", m_postVS);
    compile(L"PostFx.hlsl", "PSMain_Post", "ps_5_0", m_postPS);

    // ── Root Signature ────────────────────────────────────────────────────
    {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
                     D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };

        D3D12_ROOT_PARAMETER params[2]{};
        // param[0]: SRV table → t0 (HDR RT)
        params[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable  = { 1, &srvRange };
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        // param[1]: inline CBV → b0 (PostFxConstants)
        params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 0;
        params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxAnisotropy  = 1;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.MaxLOD         = D3D12_FLOAT32_MAX;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters     = 2; rsd.pParameters = params;
        rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
        rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> ser, rserr;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
            &ser, &rserr);
        if (FAILED(hr)) {
            if (rserr) throw std::runtime_error((char*)rserr->GetBufferPointer());
            ThrowIfFailed(hr, "SerializeRS PostFx");
        }
        ThrowIfFailed(m_device->CreateRootSignature(0, ser->GetBufferPointer(),
            ser->GetBufferSize(), IID_PPV_ARGS(&m_postFxRootSig)), "CreateRS PostFx");
    }

    // ── PSO ───────────────────────────────────────────────────────────────
    {
        D3D12_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_RASTERIZER_DESC rast{};
        rast.FillMode = D3D12_FILL_MODE_SOLID;
        rast.CullMode = D3D12_CULL_MODE_NONE;
        rast.DepthClipEnable = TRUE;

        D3D12_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = FALSE; ds.StencilEnable = FALSE;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature        = m_postFxRootSig.Get();
        pd.VS                    = { m_postVS->GetBufferPointer(), m_postVS->GetBufferSize() };
        pd.PS                    = { m_postPS->GetBufferPointer(), m_postPS->GetBufferSize() };
        pd.BlendState            = blend;
        pd.RasterizerState       = rast;
        pd.DepthStencilState     = ds;
        pd.SampleMask            = UINT_MAX;
        pd.InputLayout           = { nullptr, 0 };
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM; // back buffer
        pd.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        pd.SampleDesc.Count      = 1;

        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd,
            IID_PPV_ARGS(&m_postFxPSO)), "Create PostFx PSO");
    }

    // ── Constant Buffer ───────────────────────────────────────────────────
    {
        auto up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto bd = BufDesc((UINT64)((sizeof(PostFxConstants) + 255) & ~255));
        ThrowIfFailed(m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_postFxCB)), "PostFx CB");
        D3D12_RANGE rr{0,0};
        ThrowIfFailed(m_postFxCB->Map(0, &rr,
            reinterpret_cast<void**>(&m_mappedPostFxCB)), "Map PostFx CB");
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// CreateShadowMap — Texture2DArray глубины (kCsmCascades слоёв)
// SRV кладём в cbvSrvHeap слот [4N+5]
// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::CreateShadowMap()
{
    m_shadowMap.Reset();

    D3D12_RESOURCE_DESC td{};
    td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width            = kShadowMapSize;
    td.Height           = kShadowMapSize;
    td.DepthOrArraySize = (UINT16)kCsmCascades;  // массив слоёв
    td.MipLevels        = 1;
    td.Format           = DXGI_FORMAT_R32_TYPELESS; // DSV=D32_FLOAT, SRV=R32_FLOAT
    td.SampleDesc.Count = 1;
    td.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv{};
    cv.Format              = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth  = 1.f;
    cv.DepthStencil.Stencil = 0;

    auto hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
        IID_PPV_ARGS(&m_shadowMap)), "Create Shadow Map Array");

    // ── DSV heap: kCsmCascades дескрипторов, по одному на слой ──────────
    if (!m_shadowDsvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = kCsmCascades;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd,
            IID_PPV_ARGS(&m_shadowDsvHeap)), "Shadow DSV Heap");
    }

    uint32_t dsvSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvBase =
        m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();

    for (int i = 0; i < kCsmCascades; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvd{};
        dsvd.Format                         = DXGI_FORMAT_D32_FLOAT;
        dsvd.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvd.Texture2DArray.FirstArraySlice = (UINT)i;
        dsvd.Texture2DArray.ArraySize       = 1;
        dsvd.Texture2DArray.MipSlice        = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE h = dsvBase;
        h.ptr += (SIZE_T)(i * dsvSize);
        m_device->CreateDepthStencilView(m_shadowMap.Get(), &dsvd, h);
    }

    // ── SRV — Texture2DArray, читается в lighting pass как t4 ─────────────
    const uint32_t slot = 4 * m_numMaterials + 4;
    D3D12_CPU_DESCRIPTOR_HANDLE srvH =
        m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart();
    srvH.ptr += (SIZE_T)(slot * m_cbvSrvDescriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                          = DXGI_FORMAT_R32_FLOAT;
    srvd.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvd.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2DArray.MostDetailedMip  = 0;
    srvd.Texture2DArray.MipLevels        = 1;
    srvd.Texture2DArray.FirstArraySlice  = 0;
    srvd.Texture2DArray.ArraySize        = kCsmCascades;
    m_device->CreateShaderResourceView(m_shadowMap.Get(), &srvd, srvH);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool RenderingSystem::BuildShadowPSO()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompileFromFile(L"Shadow.hlsl", nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain_Shadow", "vs_5_0", flags, 0, &m_shadowVS, &err);
    if (FAILED(hr)) {
        if (err) throw std::runtime_error((char*)err->GetBufferPointer());
        ThrowIfFailed(hr, "Compile Shadow VS");
    }

    // ── Root Signature: только b0 (ShadowCB = LightViewProj + World) ─────
    {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        param.Descriptor.ShaderRegister = 0;
        param.Descriptor.RegisterSpace  = 0;
        param.ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1; rsd.pParameters = &param;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> ser, rserr;
        hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
            &ser, &rserr);
        if (FAILED(hr)) {
            if (rserr) throw std::runtime_error((char*)rserr->GetBufferPointer());
            ThrowIfFailed(hr, "SerializeRS Shadow");
        }
        ThrowIfFailed(m_device->CreateRootSignature(0, ser->GetBufferPointer(),
            ser->GetBufferSize(), IID_PPV_ARGS(&m_shadowRootSig)),
            "CreateRS Shadow");
    }

    // ── PSO: только VS, нет color RT, глубина D32_FLOAT ──────────────────
    {
        // Input layout: только POSITION (первый элемент из m_inputLayout)
        D3D12_INPUT_ELEMENT_DESC posOnly = m_inputLayout[0]; // POSITION

        D3D12_RASTERIZER_DESC rast{};
        rast.FillMode              = D3D12_FILL_MODE_SOLID;
        rast.CullMode              = D3D12_CULL_MODE_FRONT; // Peter Pan fix
        rast.FrontCounterClockwise = FALSE;
        rast.DepthClipEnable       = TRUE;
        rast.DepthBias             = 1000;      // константный bias (D32 формула)
        rast.SlopeScaledDepthBias  = 2.0f;      // slope-scaled bias против acne
        rast.DepthBiasClamp        = 0.01f;

        D3D12_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable    = TRUE;
        ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
        ds.StencilEnable  = FALSE;

        D3D12_BLEND_DESC blend{};  // нет color RT — blend не важен

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature        = m_shadowRootSig.Get();
        pd.VS                    = { m_shadowVS->GetBufferPointer(),
                                     m_shadowVS->GetBufferSize() };
        pd.BlendState            = blend;
        pd.RasterizerState       = rast;
        pd.DepthStencilState     = ds;
        pd.SampleMask            = UINT_MAX;
        pd.InputLayout           = { &posOnly, 1 };
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 0;   // нет color output
        pd.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count      = 1;

        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd,
            IID_PPV_ARGS(&m_shadowPSO)), "Create Shadow PSO");
    }

    // ── Shadow CB: два float4x4 (128 байт) выровнено до 256, × каскады × инстансы
    UINT64 shadowCBSize = 256ULL * kCsmCascades * kMaxInstances;
    auto up = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto bd = BufDesc(shadowCBSize);
    ThrowIfFailed(m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_shadowCB)), "Shadow CB");
    D3D12_RANGE rr{0,0};
    ThrowIfFailed(m_shadowCB->Map(0, &rr,
        reinterpret_cast<void**>(&m_mappedShadowCB)), "Map Shadow CB");

    return true;
}
